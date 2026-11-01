// SilverLink Controller - device discovery and control
// Copyright (C) 2026 Jorge Prata Alves Junior - GPLv3
#include "Discovery.h"
#include <algorithm>
#include <random>

namespace slink {

bool Discovery::start() {
    if (!netInit()) return false;
    std::random_device rd;
    myId_ = (uint64_t(rd()) << 32) ^ rd() ^ GetTickCount64();
    sock_ = udpSocket(kPortDiscovery);
    ctrl_ = udpSocket(0);
    if (sock_ == INVALID_SOCKET || ctrl_ == INVALID_SOCKET) return false;
    const uint32_t grp = parseIp(kGroupDiscovery);
    // Listen on every IPv4 interface so the controller works whatever the engine uses.
    bool any = false;
    for (auto& a : listAdapters()) any |= joinGroup(sock_, grp, a.ip);
    if (!any) joinGroup(sock_, grp, 0);
    DWORD tmo = 200;
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tmo), sizeof(tmo));
    running_ = true;
    th_ = std::thread(&Discovery::loop, this);
    return true;
}

void Discovery::stop() {
    running_ = false;
    if (th_.joinable()) th_.join();
    closeSock(sock_); closeSock(ctrl_);
}

void Discovery::loop() {
    std::vector<uint8_t> buf(8192);
    while (running_) {
        uint32_t ip; uint16_t port;
        int n = recvFrom(sock_, buf.data(), buf.size(), ip, port);
        if (n > 0) handle(buf.data(), n, ip);
        // drain acks (not needed for UI state, the next announce shows the result)
        u_long avail = 0;
        while (ioctlsocket(ctrl_, FIONREAD, &avail) == 0 && avail > 0) {
            uint8_t tmp[256]; recvFrom(ctrl_, tmp, sizeof(tmp), ip, port);
        }
        const int64_t now = int64_t(GetTickCount64());
        std::lock_guard<std::mutex> l(m_);
        for (auto it = devs_.begin(); it != devs_.end();)
            it = (now - it->second.lastSeen > 6000) ? devs_.erase(it) : std::next(it);
    }
}

void Discovery::handle(const uint8_t* p, int n, uint32_t ip) {
    Reader r(p, size_t(n));
    uint64_t id = 0;
    const uint8_t t = r.header(id);
    if (!t) return;
    const int64_t now = int64_t(GetTickCount64());
    std::lock_guard<std::mutex> l(m_);
    if (t == MSG_ANNOUNCE) {
        Announce a;
        if (!a.read(r)) return;
        DeviceView& d = devs_[id];
        d.id = id; d.ann = a; d.ip = a.ip ? a.ip : ip; d.lastSeen = now;
        return;
    }
    auto it = devs_.find(id);
    if (it == devs_.end()) return; // wait for announce first
    DeviceView& d = it->second;
    if (t == MSG_ROUTES) {
        std::vector<RouteStatusEntry> v;
        if (readRoutes(r, v)) d.routes = v;
    } else if (t == MSG_NAMES) {
        const int dir = r.u8(), first = r.u8(), cnt = r.u8();
        auto& names = dir ? d.rxNames : d.txNames;
        if (!r.ok()) return;
        if ((int)names.size() < first + cnt) names.resize(size_t(first + cnt));
        for (int i = 0; i < cnt && r.ok(); ++i) names[size_t(first + i)] = r.str(kChanNameLen);
    } else if (t == MSG_METERS) {
        int tx = r.u8(); std::vector<uint8_t> tv(static_cast<size_t>(tx)); for (auto& x : tv) x = r.u8();
        int rx = r.u8(); std::vector<uint8_t> rv(static_cast<size_t>(rx)); for (auto& x : rv) x = r.u8();
        if (r.ok()) { d.txMeters = tv; d.rxMeters = rv; d.lastMeters = now; }
    }
}

std::vector<DeviceView> Discovery::devices() {
    std::vector<DeviceView> v;
    {
        std::lock_guard<std::mutex> l(m_);
        for (auto& kv : devs_) v.push_back(kv.second);
    }
    std::sort(v.begin(), v.end(), [](const DeviceView& a, const DeviceView& b) {
        return a.ann.name != b.ann.name ? a.ann.name < b.ann.name : a.id < b.id;
    });
    return v;
}

void Discovery::sendControl(const DeviceView& d, uint8_t cmd, const std::vector<uint8_t>& args) {
    Writer w; w.header(MSG_CONTROL, myId_);
    w.u64(d.id); w.u32(reqId_++); w.u8(cmd);
    w.buf.insert(w.buf.end(), args.begin(), args.end());
    // send twice: UDP on a LAN rarely drops, but commands are idempotent
    sendTo(ctrl_, w.buf.data(), w.buf.size(), d.ip, kPortControl);
    sendTo(ctrl_, w.buf.data(), w.buf.size(), d.ip, kPortControl);
}

void Discovery::setRoute(const DeviceView& rxDev, int rxCh, const std::string& src, int txCh) {
    Writer a; a.u8(uint8_t(rxCh)); a.u8(uint8_t(txCh)); a.str(src, kNameLen);
    sendControl(rxDev, CMD_SET_ROUTE, a.buf);
    optimisticRoute(rxDev.id, rxCh, src, txCh);
}

void Discovery::optimisticRoute(uint64_t rxDev, int rxCh, const std::string& src, int txCh) {
    std::lock_guard<std::mutex> l(m_);
    auto it = devs_.find(rxDev);
    if (it == devs_.end()) return;
    auto& v = it->second.routes;
    v.erase(std::remove_if(v.begin(), v.end(), [&](const RouteStatusEntry& e) { return e.route.rxCh == rxCh; }), v.end());
    if (!src.empty()) {
        RouteStatusEntry e; e.route.rxCh = uint8_t(rxCh); e.route.txCh = uint8_t(txCh); e.route.src = src; e.status = RS_NO_AUDIO;
        v.push_back(e);
    }
}

} // namespace slink
