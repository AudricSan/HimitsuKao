#pragma once

#include <string>
#include <vector>

// IPv4 addresses of the up, non-loopback network interfaces (LAN, Tailscale, ...). Best effort.
std::vector<std::string> local_ipv4_addresses();
