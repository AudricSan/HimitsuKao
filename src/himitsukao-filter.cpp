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
#include "core/face-recognizer.h"
#include "core/face-tracker.h"
#include "core/identity-manager.h"
#include "core/image-utils.h"
#include "plugin-service.h"

namespace {

constexpr const char *kModelFile = "models/face_detection_yunet_2023mar.onnx";
constexpr const char *kRecognizerModelFile = "models/face_recognition_sface_2021dec.onnx";
constexpr const char *kEffectFile = "effects/pixelate.effect";
constexpr const char *kGaussianEffectFile = "effects/gaussian.effect";
constexpr uint32_t kAnalysisWidth = 640;

// Setting keys
constexpr const char *kThreshold = "threshold";
constexpr const char *kHoldMs = "hold_ms";
constexpr const char *kPadding = "padding";
constexpr const char *kAnalysisFps = "analysis_fps";
constexpr const char *kRecogThreshold = "recognition_threshold";
constexpr const char *kRecheckMs = "recheck_ms";
constexpr const char *kStyle = "style";
constexpr const char *kStrength = "strength";
constexpr const char *kServerPort = "server_port";
constexpr const char *kServerInfo = "server_info";
constexpr const char *kRegenPin = "regen_pin";

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
	std::atomic<int> analysis_fps{15};
	std::atomic<float> recog_threshold{0.363f};
	std::atomic<int> recheck_ms{1000};

	// Graphics-thread only.
	gs_effect_t *pixelate_effect = nullptr;
	gs_effect_t *gaussian_effect = nullptr;
	gs_texrender_t *blur_render = nullptr; // horizontal gaussian pass
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

	// Identity (recognition, enrollment) and the track snapshot shared with the render thread.
	std::string recognizer_model_path;
	std::unique_ptr<himitsukao::IdentityManager> identity;
	std::string service_key;
};

void worker_main(himitsukao_filter *f)
{
	std::unique_ptr<himitsukao::FaceDetector> detector;
	try {
		detector = std::make_unique<himitsukao::FaceDetector>(f->model_path, f->threshold.load());
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "cannot load the face detection model '%s': %s", f->model_path.c_str(), e.what());
		return;
	}

	// Without the recognizer every face stays unknown, hence masked (fails closed).
	std::unique_ptr<himitsukao::FaceRecognizer> recognizer;
	try {
		recognizer = std::make_unique<himitsukao::FaceRecognizer>(f->recognizer_model_path);
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "cannot load the face recognition model '%s': %s (known people will be masked)",
			f->recognizer_model_path.c_str(), e.what());
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
			const int64_t now = now_ms();
			const auto ids = tracker.update(rects, now);
			f->identity->process(
				tracker, ids, now,
				[&](size_t i) {
					return recognizer ? recognizer->embed(frame, faces[i]) : himitsukao::Embedding{};
				},
				[&](size_t i) { return himitsukao::face_thumbnail_b64(frame, faces[i].rect); });
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

gs_effect_t *load_effect(const char *relative_path)
{
	char *path = obs_module_file(relative_path);
	if (!path) {
		obs_log(LOG_ERROR, "effect file not found (%s)", relative_path);
		return nullptr;
	}
	char *error = nullptr;
	obs_enter_graphics();
	gs_effect_t *effect = gs_effect_create_from_file(path, &error);
	obs_leave_graphics();
	if (!effect)
		obs_log(LOG_ERROR, "cannot load the effect %s: %s", relative_path, error ? error : "unknown error");
	bfree(error);
	bfree(path);
	return effect;
}

const char *filter_get_name(void *)
{
	return obs_module_text("HimitsuKao");
}

// Applies the per-filter settings. `push_global` is false while the filter is being created (the values saved in
// a scene file must not override the global style), true for updates coming from the properties dialog.
void apply_settings(himitsukao_filter *f, obs_data_t *settings, bool push_global)
{
	f->threshold = static_cast<float>(obs_data_get_double(settings, kThreshold));
	f->hold_ms = static_cast<int>(obs_data_get_int(settings, kHoldMs));
	f->padding = static_cast<float>(obs_data_get_double(settings, kPadding));
	f->analysis_fps = std::max(1, static_cast<int>(obs_data_get_int(settings, kAnalysisFps)));
	f->recog_threshold = static_cast<float>(obs_data_get_double(settings, kRecogThreshold));
	f->recheck_ms = std::max(100, static_cast<int>(obs_data_get_int(settings, kRecheckMs)));
	if (f->identity) {
		himitsukao::IdentityConfig cfg;
		cfg.threshold = f->recog_threshold;
		cfg.recheck_ms = f->recheck_ms;
		f->identity->set_config(cfg);
	}

	if (!push_global)
		return;
	himitsukao::HimitsukaoService *service = plugin_service();
	if (!service)
		return;
	himitsukao::Style style;
	const int strength = static_cast<int>(obs_data_get_int(settings, kStrength));
	if (himitsukao::parse_style(obs_data_get_string(settings, kStyle), style) && strength >= 1 &&
	    (style != service->runtime().style() || strength != service->runtime().strength()))
		service->set_style(style, strength);
	const int port = static_cast<int>(obs_data_get_int(settings, kServerPort));
	if (port >= 1024 && port <= 65535)
		plugin_set_port(port);
}

void filter_update(void *data, obs_data_t *settings)
{
	apply_settings(static_cast<himitsukao_filter *>(data), settings, true);
}

void filter_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_double(settings, kThreshold, 0.5);
	obs_data_set_default_int(settings, kHoldMs, 300);
	obs_data_set_default_double(settings, kPadding, 1.3);
	obs_data_set_default_int(settings, kAnalysisFps, 15);
	obs_data_set_default_double(settings, kRecogThreshold, 0.363);
	obs_data_set_default_int(settings, kRecheckMs, 1000);
	obs_data_set_default_string(settings, kStyle, "pixel");
	obs_data_set_default_int(settings, kStrength, 50);
	obs_data_set_default_int(settings, kServerPort, 8787);
}

std::string server_info_text()
{
	const PluginServerInfo info = plugin_server_info();
	std::string text = std::string(obs_module_text("PhoneAddresses")) + ":";
	if (info.addresses.empty())
		text += " -";
	for (const auto &ip : info.addresses)
		text += " http://" + ip + ":" + std::to_string(info.configured_port);
	text += std::string("\n") + obs_module_text("PhonePin") + ": " + info.pin;
	if (info.active_port == 0)
		text += std::string("\n") + obs_module_text("PhoneNotRunning");
	return text;
}

bool regenerate_pin_clicked(obs_properties_t *props, obs_property_t *, void *)
{
	plugin_regenerate_pin();
	if (obs_property_t *info = obs_properties_get(props, kServerInfo))
		obs_property_set_description(info, server_info_text().c_str());
	return true;
}

obs_properties_t *filter_get_properties(void *data)
{
	auto *f = static_cast<himitsukao_filter *>(data);

	// The style and the port are global: show the current values, not whatever this filter saved earlier.
	if (himitsukao::HimitsukaoService *service = plugin_service()) {
		obs_data_t *settings = obs_source_get_settings(f->source);
		obs_data_set_string(settings, kStyle, himitsukao::style_name(service->runtime().style()));
		obs_data_set_int(settings, kStrength, service->runtime().strength());
		obs_data_set_int(settings, kServerPort, plugin_server_info().configured_port);
		obs_data_release(settings);
	}

	obs_properties_t *props = obs_properties_create();
	obs_properties_add_float_slider(props, kThreshold, obs_module_text("DetectionThreshold"), 0.2, 0.9, 0.05);
	obs_properties_add_int_slider(props, kHoldMs, obs_module_text("MaskHoldMs"), 0, 1000, 10);
	obs_properties_add_float_slider(props, kPadding, obs_module_text("MaskPadding"), 1.0, 2.0, 0.05);
	obs_properties_add_int_slider(props, kAnalysisFps, obs_module_text("AnalysisFps"), 5, 30, 1);
	obs_properties_add_float_slider(props, kRecogThreshold, obs_module_text("RecognitionThreshold"), 0.2, 0.6, 0.01);
	obs_properties_add_int_slider(props, kRecheckMs, obs_module_text("RecheckIntervalMs"), 200, 3000, 50);

	obs_property_t *style = obs_properties_add_list(props, kStyle, obs_module_text("MaskStyle"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(style, obs_module_text("StylePixel"), "pixel");
	obs_property_list_add_string(style, obs_module_text("StyleGaussian"), "gaussian");
	obs_property_list_add_string(style, obs_module_text("StyleSolid"), "solid");
	obs_properties_add_int_slider(props, kStrength, obs_module_text("MaskStrength"), 1, 100, 1);

	obs_properties_add_int(props, kServerPort, obs_module_text("PhonePort"), 1024, 65535, 1);
	obs_property_t *info = obs_properties_add_text(props, kServerInfo, server_info_text().c_str(), OBS_TEXT_INFO);
	obs_property_text_set_info_type(info, OBS_TEXT_INFO_NORMAL);
	obs_properties_add_button(props, kRegenPin, obs_module_text("RegeneratePin"), regenerate_pin_clicked);
	return props;
}

void *filter_create(obs_data_t *settings, obs_source_t *source)
{
	auto *f = new himitsukao_filter();
	f->source = source;
	apply_settings(f, settings, false);

	f->pixelate_effect = load_effect(kEffectFile);
	f->gaussian_effect = load_effect(kGaussianEffectFile);

	himitsukao::HimitsukaoService *service = plugin_service();
	char *model = obs_module_file(kModelFile);
	char *recognizer_model = obs_module_file(kRecognizerModelFile);
	if (recognizer_model) {
		f->recognizer_model_path = recognizer_model;
		bfree(recognizer_model);
	}
	if (model && service) {
		f->model_path = model;
		himitsukao::IdentityConfig cfg;
		cfg.threshold = f->recog_threshold;
		cfg.recheck_ms = f->recheck_ms;
		f->identity = std::make_unique<himitsukao::IdentityManager>(
			service->database(), service->board(), cfg,
			[](const std::string &msg) { obs_log(LOG_WARNING, "%s", msg.c_str()); });
		const char *name = obs_source_get_name(source);
		f->service_key = service->register_instance(name ? name : "", f->identity.get());
		f->worker = std::thread(worker_main, f);
	} else {
		obs_log(LOG_ERROR, "face detection model not found (%s): faces will NOT be masked", kModelFile);
	}
	bfree(model);
	return f;
}

void filter_destroy(void *data)
{
	auto *f = static_cast<himitsukao_filter *>(data);
	// Leave the service first: nobody may reach the identity manager once the worker stops.
	if (!f->service_key.empty()) {
		if (himitsukao::HimitsukaoService *service = plugin_service())
			service->unregister_instance(f->service_key);
	}
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
	if (f->blur_render)
		gs_texrender_destroy(f->blur_render);
	if (f->pixelate_effect)
		gs_effect_destroy(f->pixelate_effect);
	if (f->gaussian_effect)
		gs_effect_destroy(f->gaussian_effect);
	obs_leave_graphics();

	delete f;
}

// ---------------------------------------------------------------------------------------------------------------
// Masking styles. All of them draw sub-regions of the captured frame texture on top of the plain frame.

struct pixel_rect {
	uint32_t x0, y0, x1, y1;
};

std::vector<pixel_rect> to_pixel_rects(const std::vector<himitsukao::Rect> &masks, uint32_t w, uint32_t h)
{
	std::vector<pixel_rect> out;
	for (const auto &m : masks) {
		const uint32_t x0 = std::min<uint32_t>(w - 1, static_cast<uint32_t>(std::floor(m.x * w)));
		const uint32_t y0 = std::min<uint32_t>(h - 1, static_cast<uint32_t>(std::floor(m.y * h)));
		const uint32_t x1 = std::min<uint32_t>(w, static_cast<uint32_t>(std::ceil((m.x + m.w) * w)));
		const uint32_t y1 = std::min<uint32_t>(h, static_cast<uint32_t>(std::ceil((m.y + m.h) * h)));
		if (x1 > x0 && y1 > y0)
			out.push_back({x0, y0, x1, y1});
	}
	return out;
}

void draw_solid(const std::vector<pixel_rect> &rects)
{
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	vec4 color;
	vec4_set(&color, 0.125f, 0.125f, 0.125f, 1.0f); // fixed dark grey
	gs_effect_set_vec4(gs_effect_get_param_by_name(solid, "color"), &color);
	for (const auto &r : rects) {
		gs_matrix_push();
		gs_matrix_translate3f(static_cast<float>(r.x0), static_cast<float>(r.y0), 0.0f);
		while (gs_effect_loop(solid, "Solid"))
			gs_draw_sprite(nullptr, 0, r.x1 - r.x0, r.y1 - r.y0);
		gs_matrix_pop();
	}
}

void draw_pixel(himitsukao_filter *f, gs_texture_t *tex, uint32_t w, uint32_t h, const std::vector<pixel_rect> &rects,
		int block_px)
{
	gs_effect_t *effect = f->pixelate_effect;
	vec2 block;
	vec2_set(&block, static_cast<float>(block_px) / static_cast<float>(w),
		 static_cast<float>(block_px) / static_cast<float>(h));
	gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), tex);
	gs_effect_set_vec2(gs_effect_get_param_by_name(effect, "block_uv"), &block);
	for (const auto &r : rects) {
		gs_matrix_push();
		gs_matrix_translate3f(static_cast<float>(r.x0), static_cast<float>(r.y0), 0.0f);
		while (gs_effect_loop(effect, "Draw"))
			gs_draw_sprite_subregion(tex, 0, r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0);
		gs_matrix_pop();
	}
}

// Separable gaussian: the horizontal pass fills blur_render (only around the masks, extended vertically by the
// radius), the vertical pass reads it and draws on the frame. Samples are clamped to the frame by the sampler.
void draw_gaussian(himitsukao_filter *f, gs_texture_t *tex, uint32_t w, uint32_t h,
		   const std::vector<pixel_rect> &rects, float radius_px)
{
	gs_effect_t *effect = f->gaussian_effect;
	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
	gs_eparam_t *step = gs_effect_get_param_by_name(effect, "step_uv");
	const float spacing_px = std::max(0.5f, radius_px / 12.0f); // 25 taps cover +-12 spacings = +-radius
	const uint32_t margin = static_cast<uint32_t>(std::ceil(radius_px)) + 1;

	if (!f->blur_render)
		f->blur_render = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	gs_texrender_reset(f->blur_render);
	if (!gs_texrender_begin(f->blur_render, w, h)) {
		draw_solid(rects); // cannot blur: hide with a solid fill rather than show the face
		return;
	}
	vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, static_cast<float>(w), 0.0f, static_cast<float>(h), -100.0f, 100.0f);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	vec2 dir;
	vec2_set(&dir, spacing_px / static_cast<float>(w), 0.0f);
	gs_effect_set_texture(image, tex);
	gs_effect_set_vec2(step, &dir);
	for (const auto &r : rects) {
		const uint32_t y0 = r.y0 > margin ? r.y0 - margin : 0;
		const uint32_t y1 = std::min<uint32_t>(h, r.y1 + margin);
		gs_matrix_push();
		gs_matrix_translate3f(static_cast<float>(r.x0), static_cast<float>(y0), 0.0f);
		while (gs_effect_loop(effect, "Draw"))
			gs_draw_sprite_subregion(tex, 0, r.x0, y0, r.x1 - r.x0, y1 - y0);
		gs_matrix_pop();
	}
	gs_blend_state_pop();
	gs_texrender_end(f->blur_render);

	gs_texture_t *blurred = gs_texrender_get_texture(f->blur_render);
	if (!blurred) {
		draw_solid(rects);
		return;
	}
	vec2_set(&dir, 0.0f, spacing_px / static_cast<float>(h));
	gs_effect_set_texture(image, blurred);
	gs_effect_set_vec2(step, &dir);
	for (const auto &r : rects) {
		gs_matrix_push();
		gs_matrix_translate3f(static_cast<float>(r.x0), static_cast<float>(r.y0), 0.0f);
		while (gs_effect_loop(effect, "Draw"))
			gs_draw_sprite_subregion(blurred, 0, r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0);
		gs_matrix_pop();
	}
}

void draw_masks(himitsukao_filter *f, gs_texture_t *tex, uint32_t w, uint32_t h,
		const std::vector<himitsukao::Rect> &masks, himitsukao::Style style, int strength, bool whole_frame)
{
	const std::vector<pixel_rect> rects = to_pixel_rects(masks, w, h);
	if (rects.empty())
		return;

	// Sizes are expressed for a 1080p frame and scaled up for bigger ones.
	const float scale = std::max(1.0f, static_cast<float>(h) / 1080.0f);
	const float t = static_cast<float>(std::clamp(strength, 1, 100) - 1) / 99.0f;

	if (style == himitsukao::Style::Pixel && f->pixelate_effect) {
		int block = static_cast<int>(std::lround((4.0f + t * 60.0f) * scale)); // 4..64 px
		if (whole_frame)
			block = std::max(block, static_cast<int>(w / 20)); // big blocks for the emergency mode
		draw_pixel(f, tex, w, h, rects, std::max(2, block));
	} else if (style == himitsukao::Style::Gaussian && f->gaussian_effect) {
		float radius = (2.0f + t * 46.0f) * scale; // 2..48 px
		if (whole_frame)
			radius = std::max(radius, 32.0f * scale);
		draw_gaussian(f, tex, w, h, rects, radius);
	} else {
		draw_solid(rects); // solid style, or the effect of the chosen style is unavailable: still hide
	}
}

void filter_video_render(void *data, gs_effect_t *)
{
	auto *f = static_cast<himitsukao_filter *>(data);

	obs_source_t *target = obs_filter_get_target(f->source);
	obs_source_t *parent = obs_filter_get_parent(f->source);
	if (!target || !parent) {
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

	himitsukao::HimitsukaoService *service = plugin_service();
	const himitsukao::Mode mode = service ? service->runtime().mode() : himitsukao::Mode::Normal;

	// 2. Analysis pipeline: collect finished GPU copies from earlier frames, then queue a new one if due.
	// Skipped in the "whole frame" emergency mode: nothing is shown, so nothing to analyse.
	++f->frame_counter;
	for (auto &slot : f->stages) {
		if (slot.pending && f->frame_counter > slot.staged_frame)
			collect_stage(f, slot);
	}
	const uint64_t now_ns = os_gettime_ns();
	const uint64_t interval_ns = 1000000000ULL / static_cast<uint64_t>(std::max(1, f->analysis_fps.load()));
	if (f->worker.joinable() && mode != himitsukao::Mode::FrameAll && now_ns - f->last_analysis_ns >= interval_ns) {
		f->last_analysis_ns = now_ns;
		queue_analysis(f, tex, w, h);
	}

	// 3. Which regions to hide, according to the global mode.
	std::vector<himitsukao::Rect> masks;
	switch (mode) {
	case himitsukao::Mode::Normal:
	case himitsukao::Mode::FacesAll:
		if (f->identity)
			masks = f->identity->masks(now_ms(), f->hold_ms.load(), f->padding.load(),
						   mode == himitsukao::Mode::FacesAll);
		break;
	case himitsukao::Mode::FrameAll:
		masks.push_back(himitsukao::Rect{0.f, 0.f, 1.f, 1.f});
		break;
	case himitsukao::Mode::Off:
		break;
	}

	// 4. Draw the frame, then the masks on top.
	gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_effect_set_texture(gs_effect_get_param_by_name(def, "image"), tex);
	while (gs_effect_loop(def, "Draw"))
		gs_draw_sprite(tex, 0, w, h);

	if (!masks.empty()) {
		himitsukao::Style style = himitsukao::Style::Pixel;
		int strength = 50;
		if (service) {
			style = service->runtime().style();
			strength = service->runtime().strength();
		}
		draw_masks(f, tex, w, h, masks, style, strength, mode == himitsukao::Mode::FrameAll);
	}
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
