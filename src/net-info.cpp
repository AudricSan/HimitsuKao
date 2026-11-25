#include "net-info.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <vector>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#endif

std::vector<std::string> local_ipv4_addresses()
{
	std::vector<std::string> out;
#ifdef _WIN32
	ULONG size = 0;
	const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
	GetAdaptersAddresses(AF_INET, flags, nullptr, nullptr, &size);
	if (size == 0)
		return out;
	std::vector<unsigned char> buffer(size);
	auto *addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data());
	if (GetAdaptersAddresses(AF_INET, flags, nullptr, addrs, &size) != NO_ERROR)
		return out;
	for (auto *a = addrs; a; a = a->Next) {
		if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
			continue;
		for (auto *u = a->FirstUnicastAddress; u; u = u->Next) {
			if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET)
				continue;
			char text[INET_ADDRSTRLEN] = {};
			const auto *sin = reinterpret_cast<const sockaddr_in *>(u->Address.lpSockaddr);
			if (InetNtopA(AF_INET, const_cast<IN_ADDR *>(&sin->sin_addr), text, sizeof(text)))
				out.emplace_back(text);
		}
	}
#else
	ifaddrs *list = nullptr;
	if (getifaddrs(&list) != 0)
		return out;
	for (ifaddrs *a = list; a; a = a->ifa_next) {
		if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET || (a->ifa_flags & IFF_LOOPBACK) ||
		    !(a->ifa_flags & IFF_UP))
			continue;
		char text[INET_ADDRSTRLEN] = {};
		const auto *sin = reinterpret_cast<const sockaddr_in *>(a->ifa_addr);
		if (inet_ntop(AF_INET, &sin->sin_addr, text, sizeof(text)))
			out.emplace_back(text);
	}
	freeifaddrs(list);
#endif
	return out;
}
