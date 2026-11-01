// SilverLink Controller - device discovery and control
// Copyright (C) 2026 Jorge Prata Alves Junior - GPLv3
#pragma once
#include "../common/Net.h"
#include "../common/Protocol.h"
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace slink {

struct DeviceView {
    uint64_t id = 0;
    Announce ann;
    uint32_t ip = 0;
    std::vector<RouteStatusEntry> routes;
    std::vector<std::string> txNames, rxNames;
    std::vector<uint8_t> txMeters, rxMeters;
    int64_t lastSeen = 0;      // ms (GetTickCount64)
    int64_t lastMeters = 0;

    const RouteStatusEntry* routeFor(int rx) const {
        for (auto& r : routes) if (r.route.rxCh == rx) return &r;
        return nullptr;
    }
    std::string txName(int i) const { return i < (int)txNames.size() && !txNames[i].empty() ? txNames[i] : "TX " + std::to_string(i + 1); }
    std::string rxName(int i) const { return i < (int)rxNames.size() && !rxNames[i].empty() ? rxNames[i] : "RX " + std::to_string(i + 1); }
};

class Discovery {
public:
    bool start();
    void stop();
    // Snapshot of online devices (sorted by name).
    std::vector<DeviceView> devices();
    void sendControl(const DeviceView& d, uint8_t cmd, const std::vector<uint8_t>& args);

    // convenience
    void setRoute(const DeviceView& rxDev, int rxCh, const std::string& src, int txCh);
    void clearRoute(const DeviceView& rxDev, int rxCh) { setRoute(rxDev, rxCh, "", 0); }
    void optimisticRoute(uint64_t rxDev, int rxCh, const std::string& src, int txCh);

private:
    void loop();
    void handle(const uint8_t* p, int n, uint32_t ip);

    SOCKET sock_ = INVALID_SOCKET, ctrl_ = INVALID_SOCKET;
    std::atomic<bool> running_{false};
    std::thread th_;
    std::mutex m_;
    std::map<uint64_t, DeviceView> devs_;
    uint64_t myId_ = 0;
    uint32_t reqId_ = 1;
};

} // namespace slink
