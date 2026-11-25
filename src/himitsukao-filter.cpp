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

namespace {

struct himitsukao_filter {
	obs_source_t *source = nullptr;
};

const char *filter_get_name(void *)
{
	return obs_module_text("HimitsuKao");
}

void *filter_create(obs_data_t *settings, obs_source_t *source)
{
	auto *filter = new himitsukao_filter();
	filter->source = source;
	UNUSED_PARAMETER(settings);
	return filter;
}

void filter_destroy(void *data)
{
	delete static_cast<himitsukao_filter *>(data);
}

void filter_video_render(void *data, gs_effect_t *effect)
{
	auto *filter = static_cast<himitsukao_filter *>(data);
	UNUSED_PARAMETER(effect);
	// Step 1: pass-through. Face masking is added in later steps.
	obs_source_skip_video_filter(filter->source);
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
	info.video_render = filter_video_render;
	obs_register_source(&info);
}
