// SilverLink - small Winsock helpers
// Copyright (C) 2026 Jorge Prata Alves Junior - GPLv3
#include "Net.h"
#include <iphlpapi.h>
#include <cstring>

namespace slink {

bool netInit() {
    static bool done = false;
    if (done) return true;
    WSADATA wd;
    done = WSAStartup(MAKEWORD(2, 2), &wd) == 0;
    return done;
}

static std::string narrow(const wchar_t* w) {
    if (!w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

std::vector<Adapter> listAdapters() {
    std::vector<Adapter> out;
    ULONG size = 32 * 1024;
    std::vector<uint8_t> buf;
    ULONG rc = ERROR_BUFFER_OVERFLOW;
    for (int tries = 0; tries < 3 && rc == ERROR_BUFFER_OVERFLOW; ++tries) {
        buf.resize(size);
        rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_DNS_SERVER |
                                  GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_ANYCAST,
                                  nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    }
    if (rc != NO_ERROR) return out;
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
            auto* sin = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
            Adapter ad;
            ad.ip = ntohl(sin->sin_addr.s_addr);
            if ((ad.ip >> 16) == 0xA9FE && a->FirstGatewayAddress == nullptr) {
                // link-local (169.254) is fine for direct PC<->PC cables, keep it
            }
            ad.friendlyName = narrow(a->FriendlyName);
            ad.description  = narrow(a->Description);
            ad.wired        = a->IfType == IF_TYPE_ETHERNET_CSMACD;
            ad.hasGateway   = a->FirstGatewayAddress != nullptr;
            ad.speedBps     = a->TransmitLinkSpeed;
            out.push_back(ad);
        }
    }
    return out;
}

uint32_t pickDefaultAdapter(const std::vector<Adapter>& a) {
    int best = -1, bestScore = -1;
    for (int i = 0; i < (int)a.size(); ++i) {
        int score = (a[i].wired ? 4 : 0) + (a[i].hasGateway ? 2 : 0) + (a[i].speedBps >= 1000000000ULL ? 1 : 0);
        if (score > bestScore) { bestScore = score; best = i; }
    }
    return best >= 0 ? a[best].ip : 0;
}

uint32_t parseIp(const char* s) {
    in_addr a;
    if (inet_pton(AF_INET, s, &a) != 1) return 0;
    return ntohl(a.s_addr);
}

SOCKET udpSocket(uint16_t port, int rcvBuf) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return s;
    BOOL yes = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvBuf), sizeof(rcvBuf));
    int sndBuf = 1 << 20;
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sndBuf), sizeof(sndBuf));
    sockaddr_in sa{}; sa.sin_family = AF_INET; sa.sin_port = htons(port); sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) { closesocket(s); return INVALID_SOCKET; }
    return s;
}

bool joinGroup(SOCKET s, uint32_t group, uint32_t ifaceIp) {
    ip_mreq m{}; m.imr_multiaddr.s_addr = htonl(group); m.imr_interface.s_addr = htonl(ifaceIp);
    return setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&m), sizeof(m)) == 0;
}
bool leaveGroup(SOCKET s, uint32_t group, uint32_t ifaceIp) {
    ip_mreq m{}; m.imr_multiaddr.s_addr = htonl(group); m.imr_interface.s_addr = htonl(ifaceIp);
    return setsockopt(s, IPPROTO_IP, IP_DROP_MEMBERSHIP, reinterpret_cast<const char*>(&m), sizeof(m)) == 0;
}
bool setMulticastIf(SOCKET s, uint32_t ifaceIp, int ttl, bool loop) {
    in_addr a; a.s_addr = htonl(ifaceIp);
    bool ok = setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&a), sizeof(a)) == 0;
    DWORD t = DWORD(ttl), l = loop ? 1 : 0;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&t), sizeof(t));
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP, reinterpret_cast<const char*>(&l), sizeof(l));
    return ok;
}
int sendTo(SOCKET s, const void* data, size_t len, uint32_t ip, uint16_t port) {
    sockaddr_in sa{}; sa.sin_family = AF_INET; sa.sin_port = htons(port); sa.sin_addr.s_addr = htonl(ip);
    return sendto(s, reinterpret_cast<const char*>(data), int(len), 0, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
}
int recvFrom(SOCKET s, void* buf, size_t len, uint32_t& srcIp, uint16_t& srcPort) {
    sockaddr_in sa{}; int sl = sizeof(sa);
    int n = recvfrom(s, reinterpret_cast<char*>(buf), int(len), 0, reinterpret_cast<sockaddr*>(&sa), &sl);
    if (n < 0) return -1;
    srcIp = ntohl(sa.sin_addr.s_addr); srcPort = ntohs(sa.sin_port);
    return n;
}
void closeSock(SOCKET& s) { if (s != INVALID_SOCKET) { closesocket(s); s = INVALID_SOCKET; } }

} // namespace slink
