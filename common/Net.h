// SilverLink - small Winsock helpers
// Copyright (C) 2026 Jorge Prata Alves Junior - GPLv3
#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdint>
#include <string>
#include <vector>

namespace slink {

struct Adapter {
    std::string  friendlyName;
    std::string  description;
    uint32_t     ip = 0;        // host order
    bool         wired = false;
    bool         hasGateway = false;
    uint64_t     speedBps = 0;
};

bool netInit();
std::vector<Adapter> listAdapters();          // up, IPv4, non-loopback
uint32_t pickDefaultAdapter(const std::vector<Adapter>& a);

uint32_t parseIp(const char* s);              // host order, 0 on error

// UDP socket bound to INADDR_ANY:port with SO_REUSEADDR (port 0 = ephemeral).
SOCKET udpSocket(uint16_t port, int rcvBuf = 1 << 20);
bool joinGroup(SOCKET s, uint32_t group, uint32_t ifaceIp);
bool leaveGroup(SOCKET s, uint32_t group, uint32_t ifaceIp);
bool setMulticastIf(SOCKET s, uint32_t ifaceIp, int ttl = 4, bool loop = true);
int  sendTo(SOCKET s, const void* data, size_t len, uint32_t ip, uint16_t port);
// Returns bytes received or -1. srcIp in host order.
int  recvFrom(SOCKET s, void* buf, size_t len, uint32_t& srcIp, uint16_t& srcPort);
void closeSock(SOCKET& s);

} // namespace slink
