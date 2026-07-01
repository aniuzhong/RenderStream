#include "lan_net.h"

#include <cctype>
#include <cstdio>

#include <algorithm>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <Windows.h>

#include "encoding.h"

#pragma comment(lib, "iphlpapi.lib")

namespace rs::utils {
namespace {

bool IsVirtualAdapter(const std::string& friendly, const std::string& desc) {
    std::string combined = friendly + "|" + desc;
    for (auto& c : combined) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const char* keywords[] = {
        "wsl", "docker", "vmware", "virtual", "vethernet",
        "hyper-v", "tap", "tun", "vpn", "virtualbox",
        "zerotier", "tailscale", "wireguard", "openvpn",
        "bluestacks", "pseudo", "loopback"
    };
    for (const auto* kw : keywords) {
        if (combined.find(kw) != std::string::npos) return true;
    }
    return false;
}

bool IsValidLanIp(int a, int b, int c, int d) {
    if (a == 127 || a == 0) return false;                           // loopback / zero
    if (a == 169 && b == 254) return false;                         // APIPA
    if (a >= 224) return false;                                     // multicast
    return true;
}

} // namespace

std::string GetPrimaryLanIp() {
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buf(size);
    auto* addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    ULONG rc = GetAdaptersAddresses(AF_INET, flags, nullptr, addrs, &size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        rc = GetAdaptersAddresses(AF_INET, flags, nullptr, addrs, &size);
    }
    if (rc != NO_ERROR) return {};

    std::string best;
    int best_score = -10000;
    for (auto* a = addrs; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK || a->IfType == IF_TYPE_TUNNEL) continue;

        bool is_virt = IsVirtualAdapter(wstring_to_utf8(std::wstring(a->FriendlyName)),
                                            wstring_to_utf8(std::wstring(a->Description)));

        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
            auto* sa = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
            char ip[INET_ADDRSTRLEN]{};
            inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip));
            int a_ = 0, b_ = 0, c_ = 0, d_ = 0;
            if (sscanf_s(ip, "%d.%d.%d.%d", &a_, &b_, &c_, &d_) != 4) continue;
            if (!IsValidLanIp(a_, b_, c_, d_)) continue;

            int score = 0;
            if (is_virt) score -= 1000;
            if (score > best_score) { best_score = score; best = ip; }
        }
    }
    return best;
}

std::vector<std::string> BuildDirectedBroadcasts() {
    std::vector<std::string> out;
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buf(size);
    auto* addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    ULONG rc = GetAdaptersAddresses(AF_INET, flags, nullptr, addrs, &size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        rc = GetAdaptersAddresses(AF_INET, flags, nullptr, addrs, &size);
    }
    if (rc != NO_ERROR) return out;

    std::vector<uint32_t> seen;
    for (auto* a = addrs; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK || a->IfType == IF_TYPE_TUNNEL) continue;
        if (IsVirtualAdapter(wstring_to_utf8(std::wstring(a->FriendlyName)), wstring_to_utf8(std::wstring(a->Description)))) continue;

        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
            auto* sa = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
            uint32_t ip = ntohl(sa->sin_addr.s_addr);
            ULONG prefix = u->OnLinkPrefixLength;
            if (prefix > 32 || prefix == 0) continue;

            uint32_t mask = 0xFFFFFFFFu << (32 - prefix);
            uint32_t bcast = (ip & mask) | (~mask);

            if (std::find(seen.begin(), seen.end(), bcast) != seen.end()) continue;
            seen.push_back(bcast);

            char buf_ip[INET_ADDRSTRLEN]{};
            struct in_addr addr;
            addr.s_addr = htonl(bcast);
            inet_ntop(AF_INET, &addr, buf_ip, sizeof(buf_ip));
            out.push_back(buf_ip);
        }
    }
    return out;
}

} // namespace rs::utils
