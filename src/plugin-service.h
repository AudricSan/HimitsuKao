#pragma once

#include <string>
#include <vector>

#include "core/himitsukao-service.h"

// Module-wide state: one people database, one masking state and one HTTP server shared by every filter instance.
void plugin_service_init();     // loads the database and server.json, applies the saved style, starts the server
void plugin_service_shutdown(); // stops the server cleanly
// nullptr before init / after shutdown.
himitsukao::HimitsukaoService *plugin_service();

struct PluginServerInfo {
	int configured_port = 0; // applies after an OBS restart
	int active_port = 0;     // 0 = the server is not running (port busy, ...)
	std::string pin;
	std::vector<std::string> addresses;
};
PluginServerInfo plugin_server_info();
std::string plugin_regenerate_pin(); // returns the new PIN
void plugin_set_port(int port);      // saved now, used at the next start
