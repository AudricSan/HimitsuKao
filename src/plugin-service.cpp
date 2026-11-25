#include "plugin-service.h"

#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <memory>
#include <string>

namespace {
std::unique_ptr<himitsukao::HimitsukaoService> g_service;
}

void plugin_service_init()
{
	if (g_service)
		return;
	g_service = std::make_unique<himitsukao::HimitsukaoService>();

	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	char *path = obs_module_config_path("known.json");
	if (!path) {
		obs_log(LOG_WARNING, "no config path: known people will not be saved");
		return;
	}
	const himitsukao::LoadResult r = g_service->database().load(path);
	obs_log(r.ok ? LOG_INFO : LOG_ERROR, "known people database: %s", r.message.c_str());
	bfree(path);
}

void plugin_service_shutdown()
{
	g_service.reset();
}

himitsukao::HimitsukaoService *plugin_service()
{
	return g_service.get();
}
