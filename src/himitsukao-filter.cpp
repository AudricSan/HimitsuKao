/*
HimitsuKao - blur unknown faces in OBS before they reach the stream
Copyright (C) 2026 AudricSan

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "himitsukao-filter.h"

#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/core.hpp>

#include "core/face-detector.h"
#include "core/face-tracker.h"

namespace {

constexpr const char *kModelFile = "models/face_detection_yunet_2023mar.onnx";
constexpr const char *kEffectFile = "effects/pixelate.effect";
constexpr uint32_t kAnalysisWidth = 640;

// Setting keys
constexpr const char *kThreshold = "threshold";
constexpr const char *kHoldMs = "hold_ms";
constexpr const char *kPadding = "padding";
constexpr const char *kBlockSize = "block_size";
constexpr const char *kAnalysisFps = "analysis_fps";

int64_t now_ms()
{
	return static_cast<int64_t>(os_gettime_ns() / 1000000ULL);
}

struct stage_slot {
	gs_stagesurf_t *surf = nullptr;
	bool pending = false;
	uint64_t staged_frame = 0;
};

struct himitsukao_filter {
	obs_source_t *source = nullptr;

	// Settings (written by the UI thread in update, read by the graphics thread and the worker).
	std::atomic<float> threshold{0.5f};
	std::atomic<int> hold_ms{300};
	std::atomic<float> padding{1.3f};
	std::atomic<int> block_size{16};
	std::atomic<int> analysis_fps{15};

	// Graphics-thread only.
	gs_effect_t *pixelate_effect = nullptr;
	gs_texrender_t *full_render = nullptr;
	gs_texrender_t *small_render = nullptr;
	stage_slot stages[2];
	int next_stage = 0;
	uint32_t stage_w = 0;
	uint32_t stage_h = 0;
	uint64_t frame_counter = 0;
	uint64_t last_analysis_ns = 0;

	// Worker thread.
	std::string model_path;
	std::thread worker;
	std::mutex frame_mutex;
	std::condition_variable frame_cv;
	cv::Mat latest_frame; // BGRA, written by the render thread, consumed by the worker
	bool has_frame = false;
	bool stop = false;

	// Worker -> render thread: snapshot of the tracks.
	std::mutex tracks_mutex;
	std::vector<himitsukao::Track> tracks;
};

void publish_tracks(himitsukao_filter *f, const himitsukao::FaceTracker &tracker)
{
	std::lock_guard<std::mutex> lock(f->tracks_mutex);
	f->tracks = tracker.tracks();
}

void worker_main(himitsukao_filter *f)
{
	std::unique_ptr<himitsukao::FaceDetector> detector;
	try {
		detector = std::make_unique<himitsukao::FaceDetector>(f->model_path, f->threshold.load());
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "cannot load the face detection model '%s': %s", f->model_path.c_str(), e.what());
		return;
	}

	himitsukao::FaceTracker tracker;
	cv::Mat frame;
	float applied_threshold = f->threshold.load();
	int error_count = 0;

	for (;;) {
		{
			std::unique_lock<std::mutex> lock(f->frame_mutex);
			f->frame_cv.wait(lock, [f] { return f->stop || f->has_frame; });
			if (f->stop)
				return;
			std::swap(frame, f->latest_frame);
			f->has_frame = false;
		}

		try {
			const float threshold = f->threshold.load();
			if (threshold != applied_threshold) {
				detector->set_score_threshold(threshold);
				applied_threshold = threshold;
			}
			const auto faces = detector->detect(frame);
			std::vector<himitsukao::Rect> rects;
			rects.reserve(faces.size());
			for (const auto &face : faces)
				rects.push_back(face.rect);
			tracker.update(rects, now_ms());
			publish_tracks(f, tracker);
		} catch (const std::exception &e) {
			if (++error_count <= 5)
				obs_log(LOG_ERROR, "face detection failed: %s", e.what());
		} catch (...) {
			if (++error_count <= 5)
				obs_log(LOG_ERROR, "face detection failed: unknown error");
		}
	}
}

void destroy_stage_surfaces(himitsukao_filter *f)
{
	for (auto &s : f->stages) {
		if (s.surf)
			gs_stagesurface_destroy(s.surf);
		s = stage_slot{};
	}
	f->next_stage = 0;
	f->stage_w = f->stage_h = 0;
}

void ensure_stage_surfaces(himitsukao_filter *f, uint32_t sw, uint32_t sh)
{
	if (f->stage_w == sw && f->stage_h == sh && f->stages[0].surf)
		return;
	destroy_stage_surfaces(f);
	for (auto &s : f->stages)
		s.surf = gs_stagesurface_create(sw, sh, GS_BGRA);
	f->stage_w = sw;
	f->stage_h = sh;
}

// Copies a finished stage surface into the "latest frame" slot of the worker. Never blocks on the worker.
void collect_stage(himitsukao_filter *f, stage_slot &slot)
{
	uint8_t *data = nullptr;
	uint32_t linesize = 0;
	if (!gs_stagesurface_map(slot.surf, &data, &linesize)) {
		slot.pending = false;
		return;
	}
	const int w = static_cast<int>(gs_stagesurface_get_width(slot.surf));
	const int h = static_cast<int>(gs_stagesurface_get_height(slot.surf));
	{
		std::lock_guard<std::mutex> lock(f->frame_mutex);
		if (f->latest_frame.cols != w || f->latest_frame.rows != h || f->latest_frame.type() != CV_8UC4)
			f->latest_frame.create(h, w, CV_8UC4);
		for (int y = 0; y < h; ++y)
			std::memcpy(f->latest_frame.ptr(y), data + static_cast<size_t>(y) * linesize,
				    static_cast<size_t>(w) * 4);
		f->has_frame = true;
	}
	gs_stagesurface_unmap(slot.surf);
	slot.pending = false;
	f->frame_cv.notify_one();
}

// Starts a throttled analysis capture: downscale into small_render, then stage it for a later frame.
void queue_analysis(himitsukao_filter *f, gs_texture_t *full_tex, uint32_t w, uint32_t h)
{
	const uint32_t sw = std::min(w, kAnalysisWidth);
	const uint32_t sh = std::max<uint32_t>(
		8, static_cast<uint32_t>(std::lround(static_cast<double>(h) * sw / static_cast<double>(w))));
	ensure_stage_surfaces(f, sw, sh);

	stage_slot &slot = f->stages[f->next_stage];
	if (slot.pending)
		return; // GPU copy not collected yet: skip this tick rather than stall

	if (!f->small_render)
		f->small_render = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
	gs_texrender_reset(f->small_render);
	if (!gs_texrender_begin(f->small_render, sw, sh))
		return;
	vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, static_cast<float>(sw), 0.0f, static_cast<float>(sh), -100.0f, 100.0f);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_effect_set_texture(gs_effect_get_param_by_name(def, "image"), full_tex);
	while (gs_effect_loop(def, "Draw"))
		gs_draw_sprite(full_tex, 0, sw, sh);
	gs_blend_state_pop();
	gs_texrender_end(f->small_render);

	gs_texture_t *small_tex = gs_texrender_get_texture(f->small_render);
	if (!small_tex)
		return;
	gs_stage_texture(slot.surf, small_tex);
	slot.pending = true;
	slot.staged_frame = f->frame_counter;
	f->next_stage ^= 1;
}

void load_pixelate_effect(himitsukao_filter *f)
{
	char *path = obs_module_file(kEffectFile);
	if (!path) {
		obs_log(LOG_ERROR, "pixelate effect file not found (%s)", kEffectFile);
		return;
	}
	char *error = nullptr;
	obs_enter_graphics();
	f->pixelate_effect = gs_effect_create_from_file(path, &error);
	obs_leave_graphics();
	if (!f->pixelate_effect)
		obs_log(LOG_ERROR, "cannot load the pixelate effect: %s", error ? error : "unknown error");
	bfree(error);
	bfree(path);
}

const char *filter_get_name(void *)
{
	return obs_module_text("HimitsuKao");
}

void filter_update(void *data, obs_data_t *settings)
{
	auto *f = static_cast<himitsukao_filter *>(data);
	f->threshold = static_cast<float>(obs_data_get_double(settings, kThreshold));
	f->hold_ms = static_cast<int>(obs_data_get_int(settings, kHoldMs));
	f->padding = static_cast<float>(obs_data_get_double(settings, kPadding));
	f->block_size = std::max(1, static_cast<int>(obs_data_get_int(settings, kBlockSize)));
	f->analysis_fps = std::max(1, static_cast<int>(obs_data_get_int(settings, kAnalysisFps)));
}

void filter_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_double(settings, kThreshold, 0.5);
	obs_data_set_default_int(settings, kHoldMs, 300);
	obs_data_set_default_double(settings, kPadding, 1.3);
	obs_data_set_default_int(settings, kBlockSize, 16);
	obs_data_set_default_int(settings, kAnalysisFps, 15);
}

obs_properties_t *filter_get_properties(void *)
{
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_float_slider(props, kThreshold, obs_module_text("DetectionThreshold"), 0.2, 0.9, 0.05);
	obs_properties_add_int_slider(props, kHoldMs, obs_module_text("MaskHoldMs"), 0, 1000, 10);
	obs_properties_add_float_slider(props, kPadding, obs_module_text("MaskPadding"), 1.0, 2.0, 0.05);
	obs_properties_add_int_slider(props, kBlockSize, obs_module_text("PixelBlockSize"), 4, 64, 1);
	obs_properties_add_int_slider(props, kAnalysisFps, obs_module_text("AnalysisFps"), 5, 30, 1);
	return props;
}

void *filter_create(obs_data_t *settings, obs_source_t *source)
{
	auto *f = new himitsukao_filter();
	f->source = source;
	filter_update(f, settings);

	load_pixelate_effect(f);

	char *model = obs_module_file(kModelFile);
	if (model) {
		f->model_path = model;
		bfree(model);
		f->worker = std::thread(worker_main, f);
	} else {
		obs_log(LOG_ERROR, "face detection model not found (%s): faces will NOT be masked", kModelFile);
	}
	return f;
}

void filter_destroy(void *data)
{
	auto *f = static_cast<himitsukao_filter *>(data);
	{
		std::lock_guard<std::mutex> lock(f->frame_mutex);
		f->stop = true;
	}
	f->frame_cv.notify_all();
	if (f->worker.joinable())
		f->worker.join();

	obs_enter_graphics();
	destroy_stage_surfaces(f);
	if (f->full_render)
		gs_texrender_destroy(f->full_render);
	if (f->small_render)
		gs_texrender_destroy(f->small_render);
	if (f->pixelate_effect)
		gs_effect_destroy(f->pixelate_effect);
	obs_leave_graphics();

	delete f;
}

void draw_masks(himitsukao_filter *f, gs_texture_t *tex, uint32_t w, uint32_t h,
		const std::vector<himitsukao::Rect> &masks)
{
	gs_eparam_t *image = gs_effect_get_param_by_name(f->pixelate_effect, "image");
	gs_eparam_t *block_uv = gs_effect_get_param_by_name(f->pixelate_effect, "block_uv");
	const int block_px = f->block_size.load();
	vec2 block;
	vec2_set(&block, static_cast<float>(block_px) / static_cast<float>(w),
		 static_cast<float>(block_px) / static_cast<float>(h));
	gs_effect_set_texture(image, tex);
	gs_effect_set_vec2(block_uv, &block);

	for (const auto &m : masks) {
		const uint32_t x0 = std::min<uint32_t>(w - 1, static_cast<uint32_t>(std::floor(m.x * w)));
		const uint32_t y0 = std::min<uint32_t>(h - 1, static_cast<uint32_t>(std::floor(m.y * h)));
		const uint32_t x1 = std::min<uint32_t>(w, static_cast<uint32_t>(std::ceil((m.x + m.w) * w)));
		const uint32_t y1 = std::min<uint32_t>(h, static_cast<uint32_t>(std::ceil((m.y + m.h) * h)));
		if (x1 <= x0 || y1 <= y0)
			continue;
		gs_matrix_push();
		gs_matrix_translate3f(static_cast<float>(x0), static_cast<float>(y0), 0.0f);
		while (gs_effect_loop(f->pixelate_effect, "Draw"))
			gs_draw_sprite_subregion(tex, 0, x0, y0, x1 - x0, y1 - y0);
		gs_matrix_pop();
	}
}

void filter_video_render(void *data, gs_effect_t *)
{
	auto *f = static_cast<himitsukao_filter *>(data);

	obs_source_t *target = obs_filter_get_target(f->source);
	obs_source_t *parent = obs_filter_get_parent(f->source);
	if (!target || !parent || !f->pixelate_effect) {
		obs_source_skip_video_filter(f->source);
		return;
	}
	const uint32_t w = obs_source_get_base_width(target);
	const uint32_t h = obs_source_get_base_height(target);
	if (!w || !h) {
		obs_source_skip_video_filter(f->source);
		return;
	}

	// 1. Capture the target at its base size.
	if (!f->full_render)
		f->full_render = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	gs_texrender_reset(f->full_render);
	if (!gs_texrender_begin(f->full_render, w, h)) {
		obs_source_skip_video_filter(f->source);
		return;
	}
	{
		vec4 clear;
		vec4_zero(&clear);
		gs_blend_state_push();
		gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, static_cast<float>(w), 0.0f, static_cast<float>(h), -100.0f, 100.0f);
		const uint32_t parent_flags = obs_source_get_output_flags(parent);
		const bool custom_draw = (parent_flags & OBS_SOURCE_CUSTOM_DRAW) != 0;
		const bool async = (parent_flags & OBS_SOURCE_ASYNC) != 0;
		if (target == parent && !custom_draw && !async)
			obs_source_default_render(target);
		else
			obs_source_video_render(target);
		gs_blend_state_pop();
		gs_texrender_end(f->full_render);
	}
	gs_texture_t *tex = gs_texrender_get_texture(f->full_render);
	if (!tex) {
		obs_source_skip_video_filter(f->source);
		return;
	}

	// 2. Analysis pipeline: collect finished GPU copies from earlier frames, then queue a new one if due.
	++f->frame_counter;
	for (auto &slot : f->stages) {
		if (slot.pending && f->frame_counter > slot.staged_frame)
			collect_stage(f, slot);
	}
	const uint64_t now_ns = os_gettime_ns();
	const uint64_t interval_ns = 1000000000ULL / static_cast<uint64_t>(std::max(1, f->analysis_fps.load()));
	if (f->worker.joinable() && now_ns - f->last_analysis_ns >= interval_ns) {
		f->last_analysis_ns = now_ns;
		queue_analysis(f, tex, w, h);
	}

	// 3. Draw the frame, then the pixelated faces on top.
	std::vector<himitsukao::Track> tracks;
	{
		std::lock_guard<std::mutex> lock(f->tracks_mutex);
		tracks = f->tracks;
	}
	const auto masks = himitsukao::masks_from_tracks(tracks, now_ms(), f->hold_ms.load(), f->padding.load());

	gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_effect_set_texture(gs_effect_get_param_by_name(def, "image"), tex);
	while (gs_effect_loop(def, "Draw"))
		gs_draw_sprite(tex, 0, w, h);

	if (!masks.empty())
		draw_masks(f, tex, w, h, masks);
}

uint32_t filter_get_width(void *data)
{
	auto *f = static_cast<himitsukao_filter *>(data);
	obs_source_t *target = obs_filter_get_target(f->source);
	return target ? obs_source_get_base_width(target) : 0;
}

uint32_t filter_get_height(void *data)
{
	auto *f = static_cast<himitsukao_filter *>(data);
	obs_source_t *target = obs_filter_get_target(f->source);
	return target ? obs_source_get_base_height(target) : 0;
}

} // namespace

void register_himitsukao_filter()
{
	obs_source_info info = {};
	info.id = "himitsukao_filter";
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO;
	info.get_name = filter_get_name;
	info.create = filter_create;
	info.destroy = filter_destroy;
	info.update = filter_update;
	info.get_defaults = filter_get_defaults;
	info.get_properties = filter_get_properties;
	info.video_render = filter_video_render;
	info.get_width = filter_get_width;
	info.get_height = filter_get_height;
	obs_register_source(&info);
}
