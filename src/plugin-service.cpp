#include "plugin-service.h"

#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <memory>
#include <mutex>
#include <string>

#include "core/api.h"
#include "core/auth.h"
#include "core/http-server.h"
#include "core/server-binding.h"
#include "core/server-config.h"
#include "dock.h"
#include "net-info.h"
#include "web-index.h" // generated from src/web/index.html at configure time

namespace {

struct PluginRuntime {
	himitsukao::HimitsukaoService service;

	std::mutex config_mutex; // config data only: never held while stopping the server (request threads take it)
	himitsukao::ServerConfig config;
	std::string config_path;

	std::mutex server_mutex; // serialises start/stop of the server
	std::unique_ptr<himitsukao::ApiHandler> api;
	std::unique_ptr<himitsukao::HttpServer> server;
	std::string active_host; // host the running server is bound to

	void save_config_locked()
	{
		if (!config_path.empty() && !config.save(config_path))
			obs_log(LOG_WARNING, "could not save the server config");
	}

	himitsukao::ServerConfig config_copy()
	{
		std::lock_guard<std::mutex> lock(config_mutex);
		return config;
	}

	// Makes the server match the config (see decide_server_binding): phone page enabled -> all interfaces, disabled ->
	// loopback only (for the OBS dock). Rebinds when the host or the port changed.
	void apply_server_state()
	{
		const himitsukao::ServerConfig c = config_copy();
		const himitsukao::ServerBinding want = himitsukao::decide_server_binding(c.enabled, kDockAvailable);
		bool changed = false;
		{
			std::lock_guard<std::mutex> lock(server_mutex);
			if (server->running() && (!want.run || server->port() != c.port || active_host != want.host)) {
				server->stop();
				obs_log(LOG_INFO, "HimitsuKao server stopped");
				changed = true;
			}
			if (want.run && !server->running()) {
				std::string error;
				if (server->start(want.host, c.port, &error)) {
					active_host = want.host;
					obs_log(LOG_INFO, "HimitsuKao server listening on %s:%d (%s)",
						want.host.c_str(), server->port(),
						c.enabled ? "phone page enabled" : "OBS dock only");
				} else {
					obs_log(LOG_ERROR,
						"HimitsuKao server not started: %s. Masking keeps working without it.",
						error.c_str());
				}
				changed = true;
			}
		}
		if (changed)
			dock_refresh_url();
	}
};

std::unique_ptr<PluginRuntime> g_runtime;

int64_t clock_ms()
{
	return static_cast<int64_t>(os_gettime_ns() / 1000000ULL);
}

} // namespace

void plugin_service_init()
{
	if (g_runtime)
		return;
	g_runtime = std::make_unique<PluginRuntime>();
	PluginRuntime &rt = *g_runtime;

	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}

	if (char *path = obs_module_config_path("known.json")) {
		const himitsukao::LoadResult r = rt.service.database().load(path);
		obs_log(r.ok ? LOG_INFO : LOG_ERROR, "known people database: %s", r.message.c_str());
		bfree(path);
	} else {
		obs_log(LOG_WARNING, "no config path: known people will not be saved");
	}

	if (char *path = obs_module_config_path("server.json")) {
		rt.config_path = path;
		bfree(path);
	}
	if (!rt.config_path.empty()) {
		const himitsukao::LoadResult r = rt.config.load(rt.config_path);
		obs_log(r.ok ? LOG_INFO : LOG_ERROR, "server config: %s", r.message.c_str());
		if (r.needs_save)
			rt.save_config_locked(); // first run, repaired config or migrated access code
	} else {
		rt.config.code = himitsukao::generate_access_code();
	}

	// Style and strength are persisted; the mode always starts at "normal".
	rt.service.runtime().set_style(rt.config.style, rt.config.strength);
	PluginRuntime *prt = g_runtime.get();
	rt.service.set_style_listener([prt] {
		std::lock_guard<std::mutex> lock(prt->config_mutex);
		prt->config.style = prt->service.runtime().style();
		prt->config.strength = prt->service.runtime().strength();
		prt->save_config_locked();
	});

	rt.api = std::make_unique<himitsukao::ApiHandler>(
		rt.service, std::string(reinterpret_cast<const char *>(kWebIndexHtml), kWebIndexHtmlSize),
		rt.config.code, clock_ms, [](const std::string &msg) { obs_log(LOG_WARNING, "%s", msg.c_str()); });
	rt.server = std::make_unique<himitsukao::HttpServer>(*rt.api);
	rt.apply_server_state();
	if (!rt.config.enabled)
		obs_log(LOG_INFO, "phone page is disabled: only the OBS dock on this PC can reach the server");
}

void plugin_service_shutdown()
{
	if (!g_runtime)
		return;
	if (g_runtime->server)
		g_runtime->server->stop();
	g_runtime.reset();
}

himitsukao::HimitsukaoService *plugin_service()
{
	return g_runtime ? &g_runtime->service : nullptr;
}

PluginServerInfo plugin_server_info()
{
	PluginServerInfo info;
	if (!g_runtime)
		return info;
	const himitsukao::ServerConfig c = g_runtime->config_copy();
	info.enabled = c.enabled;
	info.configured_port = c.port;
	info.code = c.code;
	{
		std::lock_guard<std::mutex> lock(g_runtime->server_mutex);
		info.active_port = g_runtime->server && g_runtime->server->running() ? g_runtime->server->port() : 0;
	}
	info.addresses = local_ipv4_addresses();
	return info;
}

std::string plugin_regenerate_code()
{
	if (!g_runtime)
		return {};
	const std::string code = himitsukao::generate_access_code();
	{
		std::lock_guard<std::mutex> lock(g_runtime->config_mutex);
		g_runtime->config.code = code;
		g_runtime->save_config_locked();
	}
	g_runtime->api->set_code(code);
	dock_refresh_url();
	return code;
}

void plugin_set_port(int port)
{
	if (!g_runtime || port < 1024 || port > 65535)
		return;
	{
		std::lock_guard<std::mutex> lock(g_runtime->config_mutex);
		if (g_runtime->config.port == port)
			return;
		g_runtime->config.port = port;
		g_runtime->save_config_locked();
	}
	g_runtime->apply_server_state();
}

void plugin_set_server_enabled(bool enabled)
{
	if (!g_runtime)
		return;
	{
		std::lock_guard<std::mutex> lock(g_runtime->config_mutex);
		if (g_runtime->config.enabled == enabled)
			return;
		g_runtime->config.enabled = enabled;
		g_runtime->save_config_locked();
	}
	g_runtime->apply_server_state();
}
