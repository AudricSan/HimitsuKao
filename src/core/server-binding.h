#pragma once

#include <string>

namespace himitsukao {

// Where the HTTP server listens, given the user's choices.
//  - phone page enabled  -> every interface (0.0.0.0); the IP allowlist and the access code still apply;
//  - phone page disabled -> loopback only (127.0.0.1), so only this PC can reach it: that is what the OBS dock
//    uses. Without a dock (build without Qt) there is no reason to listen at all.
struct ServerBinding {
	bool run = false;
	std::string host;

	bool operator==(const ServerBinding &o) const { return run == o.run && host == o.host; }
};

inline ServerBinding decide_server_binding(bool phone_page_enabled, bool dock_available)
{
	if (phone_page_enabled)
		return {true, "0.0.0.0"};
	if (dock_available)
		return {true, "127.0.0.1"};
	return {false, std::string()};
}

} // namespace himitsukao
