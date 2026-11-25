#pragma once

#include <string>
#include <vector>

#include "core/himitsukao-service.h"

// Module-wide state: one people database, one masking state and one (opt-in) HTTP server shared by every filter
// instance.
void plugin_service_init();     // loads the database and server.json, applies the saved style, starts the server if enabled
void plugin_service_shutdown(); // stops the server cleanly
// nullptr before init / after shutdown.
himitsukao::HimitsukaoService *plugin_service();

struct PluginServerInfo {
	bool enabled = false;    // the user opted in to the phone page
	int configured_port = 0;
	int active_port = 0;     // 0 = nothing is listening (disabled, or the port is busy)
	std::string code;        // the access code
	std::vector<std::string> addresses;
};
PluginServerInfo plugin_server_info();
std::string plugin_regenerate_code(); // returns the new access code
void plugin_set_port(int port);       // saved; the server restarts on the new port if it is running
void plugin_set_server_enabled(bool enabled); // saved; starts / stops the server right away
