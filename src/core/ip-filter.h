#pragma once

#include <string>

namespace himitsukao {

// Client address classification for the phone page server (trusted networks only).

// True for 127/8, ::1.
bool is_loopback_ip(const std::string &ip);

// True for loopback, RFC1918 (10/8, 172.16/12, 192.168/16), CGNAT/Tailscale 100.64/10, IPv6 ULA fc00::/7 (this
// covers Tailscale's fd7a:115c:a1e0::/48) and IPv4-mapped forms (::ffff:a.b.c.d) of the IPv4 ranges. Anything
// else, and anything that does not parse as an IP address, is rejected.
bool is_allowed_client_ip(const std::string &ip);

// Host header check against DNS rebinding: accepts "localhost" or an IP literal (IPv4, or IPv6 in brackets),
// with an optional :port. Everything else (domain names, empty) is rejected.
bool is_acceptable_host_header(const std::string &host);

} // namespace himitsukao
