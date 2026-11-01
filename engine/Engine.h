// SilverLink Engine - network audio core
// Copyright (C) 2026 Jorge Prata Alves Junior - GPLv3
#pragma once
#include "../common/Net.h"
#include "../common/Shm.h"
#include "../common/Protocol.h"
#include "NetClock.h"

#include <array>
#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace slink {

struct Config {
    uint64_t deviceId = 0;
    std::string name;
    uint32_t rate = 44100;
    uint32_t latencyUs = 4000;
    bool preferredLeader = false;
    uint32_t ifaceIp = 0;
    uint32_t bufferPref = 256;
    bool testTone = false;
    int txChannels = kDefaultChans;
    int rxChannels = kDefaultChans;
    std::string txNames[kMaxChannels];
    std::string rxNames[kMaxChannels];
    std::map<int, Route> routes;   // key = rx channel

    static std::wstring path();
    void load();
    void save() const;
    void defaults();
};

struct EngineStatus {
    std::string name;
    std::string ip;
    bool isLeader = false;
    uint8_t clockState = 0;
    std::string leaderName;
    int64_t offsetNs = 0;
    int peers = 0;
    bool asioActive = false;
    uint32_t rate = 0;
};

class Engine {
public:
    Engine();
    ~Engine();
    bool start(std::string& err);
    void stop();
    bool restart(std::string& err) { stop(); return start(err); }

    EngineStatus status();
    uint32_t interfaceIp() const { return cfg_.ifaceIp; }
    void setInterface(uint32_t ip);

private:
    struct Peer {
        Announce ann;
        uint32_t srcIp = 0;
        int64_t  lastSeen = 0;
    };
    struct SourceState {
        uint32_t lastSeq = 0;
        bool     haveSeq = false;
        bool     rateMismatch = false;
        int64_t  lastPacket = 0;
    };
    struct RouteMap { std::vector<std::pair<int, int>> txToRx; }; // (txCh, rxCh)

    // threads
    void blockThread();
    void txThread();
    void rxAudioThread();
    void netThread();
    void houseThread();

    // helpers
    bool openShm(std::string& err);
    void closeShm();
    void publishConfigToShm(bool bumpRev);
    void handleDiscovery(const uint8_t* p, int n, uint32_t srcIp);
    void handleClock(const uint8_t* p, int n, uint32_t srcIp, int64_t rxLocal);
    void handleControl(const uint8_t* p, int n, uint32_t srcIp, uint16_t srcPort);
    void sendAnnounce();
    void sendMeters();
    void electLeader(int64_t now);
    void rebuildRouting(int64_t now);
    Announce buildAnnounce();
    void applyRate(uint32_t rate);
    void writeTestTone(int64_t S);

    Config cfg_;
    std::mutex cfgMutex_;

    // shared memory
    HANDLE shmMap_ = nullptr, blockEvt_ = nullptr, txEvt_ = nullptr;
    ShmLayout* shm_ = nullptr;

    // sockets
    SOCKET discSock_ = INVALID_SOCKET, clockSock_ = INVALID_SOCKET, ctrlSock_ = INVALID_SOCKET;
    SOCKET audioRxSock_ = INVALID_SOCKET, audioTxSock_ = INVALID_SOCKET;

    std::atomic<bool> running_{false};
    std::vector<std::thread> threads_;

    NetClock clock_;
    int64_t startNs_ = 0;

    // peers & routing
    std::mutex peersMutex_;
    std::map<uint64_t, Peer> peers_;
    std::atomic<uint64_t> leaderId_{0};
    std::atomic<uint32_t> leaderIp_{0};

    std::mutex routeMutex_;                       // guards routeMap_, sources_, routeStatus_
    std::map<uint64_t, RouteMap> routeMap_;       // source device id -> pairs
    std::map<uint64_t, SourceState> sources_;
    std::vector<RouteStatusEntry> routeStatus_;
    std::set<uint32_t> joinedGroups_;

    // stats & meters
    std::atomic<uint32_t> rxPackets_{0}, rxLate_{0}, rxLost_{0}, txPackets_{0};
    std::array<std::atomic<float>, kMaxChannels> rxPeak_{}, txPeak_{};

    // clock messaging
    uint32_t syncSeq_ = 0, delaySeq_ = 0;
    std::mutex delayMutex_;
    std::map<uint32_t, int64_t> pendingDelay_;    // seq -> t3 local

    // tx state
    int64_t txSent_ = 0;
    int64_t toneEnd_ = 0;
};

} // namespace slink
