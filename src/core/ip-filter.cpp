#include "ip-filter.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#endif

namespace himitsukao {

namespace {

// Strict dotted-quad parser (no leading junk, 4 decimal fields 0..255).
bool parse_ipv4(const std::string &s, std::array<uint8_t, 4> &out)
{
	size_t i = 0;
	for (int part = 0; part < 4; ++part) {
		if (i >= s.size() || !std::isdigit(static_cast<unsigned char>(s[i])))
			return false;
		int value = 0, digits = 0;
		while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
			value = value * 10 + (s[i] - '0');
			if (++digits > 3 || value > 255)
				return false;
			++i;
		}
		out[static_cast<size_t>(part)] = static_cast<uint8_t>(value);
		if (part < 3) {
			if (i >= s.size() || s[i] != '.')
				return false;
			++i;
		}
	}
	return i == s.size();
}

bool parse_ipv6(const std::string &s, std::array<uint8_t, 16> &out)
{
	if (s.empty() || s.find(':') == std::string::npos)
		return false;
	in6_addr addr{};
	if (inet_pton(AF_INET6, s.c_str(), &addr) != 1)
		return false;
	std::copy(reinterpret_cast<const uint8_t *>(&addr), reinterpret_cast<const uint8_t *>(&addr) + 16, out.begin());
	return true;
}

bool allowed_v4(const std::array<uint8_t, 4> &a)
{
	if (a[0] == 127 || a[0] == 10)
		return true;
	if (a[0] == 172 && a[1] >= 16 && a[1] <= 31)
		return true;
	if (a[0] == 192 && a[1] == 168)
		return true;
	if (a[0] == 100 && a[1] >= 64 && a[1] <= 127) // 100.64.0.0/10
		return true;
	return false;
}

// Returns true and fills `v4` if `v6` is ::ffff:a.b.c.d.
bool mapped_v4(const std::array<uint8_t, 16> &v6, std::array<uint8_t, 4> &v4)
{
	for (int i = 0; i < 10; ++i)
		if (v6[static_cast<size_t>(i)] != 0)
			return false;
	if (v6[10] != 0xff || v6[11] != 0xff)
		return false;
	std::copy(v6.begin() + 12, v6.end(), v4.begin());
	return true;
}

std::string strip_zone(const std::string &ip)
{
	const size_t pct = ip.find('%');
	return pct == std::string::npos ? ip : ip.substr(0, pct);
}

} // namespace

bool is_loopback_ip(const std::string &ip_in)
{
	const std::string ip = strip_zone(ip_in);
	std::array<uint8_t, 4> v4{};
	if (parse_ipv4(ip, v4))
		return v4[0] == 127;
	std::array<uint8_t, 16> v6{};
	if (!parse_ipv6(ip, v6))
		return false;
	if (mapped_v4(v6, v4))
		return v4[0] == 127;
	for (int i = 0; i < 15; ++i)
		if (v6[static_cast<size_t>(i)] != 0)
			return false;
	return v6[15] == 1; // ::1
}

bool is_allowed_client_ip(const std::string &ip_in)
{
	const std::string ip = strip_zone(ip_in);
	std::array<uint8_t, 4> v4{};
	if (parse_ipv4(ip, v4))
		return allowed_v4(v4);
	std::array<uint8_t, 16> v6{};
	if (!parse_ipv6(ip, v6))
		return false;
	if (mapped_v4(v6, v4))
		return allowed_v4(v4);
	if (is_loopback_ip(ip))
		return true;
	return (v6[0] & 0xFE) == 0xFC; // fc00::/7 (unique local addresses)
}

bool is_acceptable_host_header(const std::string &host)
{
	if (host.empty())
		return false;
	std::string name;
	std::string port;
	if (host[0] == '[') { // [v6] or [v6]:port
		const size_t close = host.find(']');
		if (close == std::string::npos)
			return false;
		name = host.substr(1, close - 1);
		if (close + 1 < host.size()) {
			if (host[close + 1] != ':')
				return false;
			port = host.substr(close + 2);
			if (port.empty())
				return false;
		}
		std::array<uint8_t, 16> v6{};
		if (name.find('%') != std::string::npos || !parse_ipv6(name, v6))
			return false;
	} else {
		const size_t colon = host.find(':');
		if (colon != std::string::npos) {
			if (host.find(':', colon + 1) != std::string::npos)
				return false; // bare IPv6 is not valid in a Host header
			name = host.substr(0, colon);
			port = host.substr(colon + 1);
			if (port.empty())
				return false;
		} else {
			name = host;
		}
		std::string lower = name;
		std::transform(lower.begin(), lower.end(), lower.begin(),
			       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		std::array<uint8_t, 4> v4{};
		if (lower != "localhost" && !parse_ipv4(name, v4))
			return false;
	}
	if (!port.empty()) {
		if (port.size() > 5 || !std::all_of(port.begin(), port.end(), [](unsigned char c) { return std::isdigit(c) != 0; }))
			return false;
	}
	return true;
}

} // namespace himitsukao
