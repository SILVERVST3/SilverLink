// SilverLink Engine - network audio core
// Copyright (C) 2026 Jorge Prata Alves Junior - GPLv3
#include "Engine.h"
#include "../common/TimeUtil.h"

#include <mmsystem.h>
#include <avrt.h>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace slink {

// ===========================================================================
// Config
// ===========================================================================
static std::string sanitizeName(const std::string& in) {
    std::string s;
    for (char c : in) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')
            s += c;
        else if (c == ' ')
            s += '-';
        if ((int)s.size() >= kNameLen - 1) break;
    }
    if (s.empty()) s = "SilverLink";
    return s;
}
static std::string sanitizeChan(const std::string& in) {
    std::string s;
    for (char c : in) { if (c >= 32 && c < 127 && c != '|' && c != '=') s += c; if ((int)s.size() >= kChanNameLen - 1) break; }
    return s;
}

std::wstring Config::path() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    std::wstring dir = (n > 0 && n < MAX_PATH) ? std::wstring(buf) : std::wstring(L".");
    dir += L"\\SilverLink";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\engine.cfg";
}

void Config::defaults() {
    std::random_device rd;
    deviceId = (uint64_t(rd()) << 32) ^ uint64_t(rd()) ^ uint64_t(nowNs());
    if (deviceId == 0) deviceId = 1;
    char host[MAX_COMPUTERNAME_LENGTH + 1] = {}; DWORD hl = sizeof(host);
    GetComputerNameA(host, &hl);
    name = sanitizeName(host);
    for (int i = 0; i < kMaxChannels; ++i) {
        txNames[i] = "TX " + std::to_string(i + 1);
        rxNames[i] = "RX " + std::to_string(i + 1);
    }
}

void Config::load() {
    defaults();
    std::ifstream f{std::filesystem::path(path())};
    if (!f) { save(); return; }
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        try {
            if (k == "deviceId") deviceId = std::stoull(v, nullptr, 16);
            else if (k == "name") name = sanitizeName(v);
            else if (k == "rate") rate = std::stoul(v);
            else if (k == "latencyUs") latencyUs = std::stoul(v);
            else if (k == "preferredLeader") preferredLeader = v == "1";
            else if (k == "iface") ifaceIp = parseIp(v.c_str());
            else if (k == "buffer") bufferPref = std::stoul(v);
            else if (k == "testTone") testTone = v == "1";
            else if (k == "txChannels") txChannels = std::stoi(v);
            else if (k == "rxChannels") rxChannels = std::stoi(v);
            else if (k.rfind("txname.", 0) == 0) { int i = std::stoi(k.substr(7)); if (i >= 0 && i < kMaxChannels) txNames[i] = sanitizeChan(v); }
            else if (k.rfind("rxname.", 0) == 0) { int i = std::stoi(k.substr(7)); if (i >= 0 && i < kMaxChannels) rxNames[i] = sanitizeChan(v); }
            else if (k.rfind("route.", 0) == 0) {
                int rx = std::stoi(k.substr(6));
                auto bar = v.find('|');
                if (bar != std::string::npos && rx >= 0 && rx < kMaxChannels) {
                    Route r; r.rxCh = uint8_t(rx); r.src = sanitizeName(v.substr(0, bar)); r.txCh = uint8_t(std::stoi(v.substr(bar + 1)));
                    routes[rx] = r;
                }
            }
        } catch (...) {}
    }
    bool okRate = false; for (auto r : kRates) okRate |= (r == rate); if (!okRate) rate = 44100;
    if (latencyUs < 500 || latencyUs > 40000) latencyUs = 4000;
    txChannels = std::max(1, std::min(kMaxChannels, txChannels));
    rxChannels = std::max(1, std::min(kMaxChannels, rxChannels));
    if (bufferPref < 32 || bufferPref > 2048) bufferPref = 256;
}

void Config::save() const {
    std::ofstream f{std::filesystem::path(path()), std::ios::trunc};
    if (!f) return;
    char id[32]; snprintf(id, sizeof(id), "%016llx", (unsigned long long)deviceId);
    f << "deviceId=" << id << "\n" << "name=" << name << "\n" << "rate=" << rate << "\n"
      << "latencyUs=" << latencyUs << "\n" << "preferredLeader=" << (preferredLeader ? 1 : 0) << "\n"
      << "iface=" << (ifaceIp ? ipToString(ifaceIp) : std::string()) << "\n"
      << "buffer=" << bufferPref << "\n" << "testTone=" << (testTone ? 1 : 0) << "\n"
      << "txChannels=" << txChannels << "\n" << "rxChannels=" << rxChannels << "\n";
    for (int i = 0; i < kMaxChannels; ++i) f << "txname." << i << "=" << txNames[i] << "\n";
    for (int i = 0; i < kMaxChannels; ++i) f << "rxname." << i << "=" << rxNames[i] << "\n";
    for (auto& kv : routes) f << "route." << kv.first << "=" << kv.second.src << "|" << int(kv.second.txCh) << "\n";
}

// ===========================================================================
// Engine lifecycle
// ===========================================================================
Engine::Engine() {
    for (auto& a : rxPeak_) a.store(0.f);
    for (auto& a : txPeak_) a.store(0.f);
    cfg_.load();
}
Engine::~Engine() { stop(); closeShm(); }

bool Engine::openShm(std::string& err) {
    if (shm_) return true;
    SECURITY_ATTRIBUTES sa{}; PSECURITY_DESCRIPTOR sd = nullptr;
    SECURITY_ATTRIBUTES* psa = makeOpenSa(sa, sd) ? &sa : nullptr;
    const DWORD size = DWORD(sizeof(ShmLayout));
    shmMap_ = CreateFileMappingW(INVALID_HANDLE_VALUE, psa, PAGE_READWRITE, 0, size, kShmName);
    if (!shmMap_) { err = "Falha ao criar memoria compartilhada"; if (sd) LocalFree(sd); return false; }
    shm_ = static_cast<ShmLayout*>(MapViewOfFile(shmMap_, FILE_MAP_ALL_ACCESS, 0, 0, size));
    blockEvt_ = CreateEventW(psa, FALSE, FALSE, kBlockEvtName);
    txEvt_    = CreateEventW(psa, FALSE, FALSE, kTxEvtName);
    if (sd) LocalFree(sd);
    if (!shm_ || !blockEvt_ || !txEvt_) { err = "Falha ao mapear memoria/eventos"; return false; }
    ShmHeader& h = shm_->hdr;
    h.magic = kShmMagic; h.version = kShmVersion;
    h.asioRunning = 0; h.asioBufferSize = 0; h.asioPid = 0; h.rateRequest = 0;
    h.rxReadPos = 0; h.txWriteEnd = 0; h.blockStart = 0; h.blockCounter = 0; h.engineConfigRev = 1;
    publishConfigToShm(false);
    return true;
}

void Engine::closeShm() {
    if (shm_) { UnmapViewOfFile(shm_); shm_ = nullptr; }
    if (shmMap_) { CloseHandle(shmMap_); shmMap_ = nullptr; }
    if (blockEvt_) { CloseHandle(blockEvt_); blockEvt_ = nullptr; }
    if (txEvt_) { CloseHandle(txEvt_); txEvt_ = nullptr; }
}

void Engine::publishConfigToShm(bool bumpRev) {
    if (!shm_) return;
    ShmHeader& h = shm_->hdr;
    std::lock_guard<std::mutex> l(cfgMutex_);
    h.sampleRate = cfg_.rate;
    h.txChannels = uint32_t(cfg_.txChannels);
    h.rxChannels = uint32_t(cfg_.rxChannels);
    h.latencySamples = int32_t(int64_t(cfg_.latencyUs) * cfg_.rate / 1000000);
    h.preferredBuffer = cfg_.bufferPref;
    for (int i = 0; i < kMaxChannels; ++i) {
        strncpy(h.txNames[i], cfg_.txNames[i].c_str(), kChanNameLen - 1); h.txNames[i][kChanNameLen - 1] = 0;
        strncpy(h.rxNames[i], cfg_.rxNames[i].c_str(), kChanNameLen - 1); h.rxNames[i][kChanNameLen - 1] = 0;
    }
    strncpy(h.deviceName, cfg_.name.c_str(), kNameLen - 1); h.deviceName[kNameLen - 1] = 0;
    if (bumpRev) h.engineConfigRev.fetch_add(1);
}

bool Engine::start(std::string& err) {
    if (running_) return true;
    if (!netInit()) { err = "Winsock indisponivel"; return false; }
    if (!openShm(err)) return false;

    auto adapters = listAdapters();
    bool found = false;
    for (auto& a : adapters) found |= (a.ip == cfg_.ifaceIp);
    if (!found) {
        cfg_.ifaceIp = pickDefaultAdapter(adapters);
        cfg_.save();
    }
    if (cfg_.ifaceIp == 0) { err = "Nenhuma interface de rede ativa encontrada"; return false; }
    const uint32_t ifc = cfg_.ifaceIp;

    discSock_    = udpSocket(kPortDiscovery);
    clockSock_   = udpSocket(kPortClock);
    ctrlSock_    = udpSocket(kPortControl);
    audioRxSock_ = udpSocket(kPortAudio, 8 << 20);
    audioTxSock_ = udpSocket(0);
    if (discSock_ == INVALID_SOCKET || clockSock_ == INVALID_SOCKET || ctrlSock_ == INVALID_SOCKET ||
        audioRxSock_ == INVALID_SOCKET || audioTxSock_ == INVALID_SOCKET) {
        err = "Falha ao abrir portas UDP 7700-7710 (outro programa usando?)";
        stop();
        return false;
    }
    joinGroup(discSock_, parseIp(kGroupDiscovery), ifc);
    joinGroup(clockSock_, parseIp(kGroupClock), ifc);
    setMulticastIf(discSock_, ifc);
    setMulticastIf(clockSock_, ifc);
    setMulticastIf(audioTxSock_, ifc);
    DWORD tmo = 100;
    setsockopt(audioRxSock_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tmo), sizeof(tmo));

    {
        std::lock_guard<std::mutex> l(routeMutex_);
        joinedGroups_.clear(); routeMap_.clear(); sources_.clear(); routeStatus_.clear();
    }
    {
        std::lock_guard<std::mutex> l(peersMutex_);
        peers_.clear();
    }
    leaderId_ = 0; leaderIp_ = 0;
    clock_.setFollower();
    startNs_ = nowNs();
    running_ = true;
    threads_.emplace_back(&Engine::blockThread, this);
    threads_.emplace_back(&Engine::txThread, this);
    threads_.emplace_back(&Engine::rxAudioThread, this);
    threads_.emplace_back(&Engine::netThread, this);
    threads_.emplace_back(&Engine::houseThread, this);
    return true;
}

void Engine::stop() {
    running_ = false;
    if (txEvt_) SetEvent(txEvt_);
    for (auto& t : threads_) if (t.joinable()) t.join();
    threads_.clear();
    closeSock(discSock_); closeSock(clockSock_); closeSock(ctrlSock_);
    closeSock(audioRxSock_); closeSock(audioTxSock_);
}

void Engine::setInterface(uint32_t ip) {
    { std::lock_guard<std::mutex> l(cfgMutex_); cfg_.ifaceIp = ip; cfg_.save(); }
    std::string err; restart(err);
}

EngineStatus Engine::status() {
    EngineStatus s;
    { std::lock_guard<std::mutex> l(cfgMutex_); s.name = cfg_.name; s.ip = ipToString(cfg_.ifaceIp); s.rate = cfg_.rate; }
    auto cs = clock_.status();
    s.isLeader = clock_.isLeader();
    s.clockState = cs.state;
    s.offsetNs = cs.lastErrorNs;
    s.asioActive = shm_ && shm_->hdr.asioRunning.load();
    std::lock_guard<std::mutex> l(peersMutex_);
    s.peers = int(peers_.size());
    auto it = peers_.find(leaderId_.load());
    if (it != peers_.end()) s.leaderName = it->second.ann.name;
    return s;
}

// ===========================================================================
// Block thread: drives the ASIO driver from the network clock (1 ms ticks)
// ===========================================================================
void Engine::blockThread() {
    timeBeginPeriod(1);
    DWORD taskIdx = 0;
    HANDLE mm = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIdx);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    LARGE_INTEGER due; due.QuadPart = -10000LL; // 1 ms
    SetWaitableTimer(timer, &due, 1, nullptr, nullptr, FALSE);

    ShmHeader& h = shm_->hdr;
    int64_t lastBlock = INT64_MIN;
    uint32_t lastSteps = clock_.steps();
    uint32_t lastRate = h.sampleRate.load();

    while (running_) {
        WaitForSingleObject(timer, 5);
        const int64_t local = nowNs();
        const uint32_t rate = h.sampleRate.load();
        const int64_t net = clock_.netAt(local);
        const int64_t S = nsToSamples(net, rate);
        h.engineHeartbeat.fetch_add(1, std::memory_order_relaxed);

        const uint32_t steps = clock_.steps();
        if (steps != lastSteps || rate != lastRate) { lastSteps = steps; lastRate = rate; lastBlock = INT64_MIN; }

        const uint32_t B = h.asioRunning.load() ? h.asioBufferSize.load() : 0;
        if (B >= 16) {
            const int64_t blk = S - (((S % B) + B) % B);
            if (lastBlock == INT64_MIN || blk < lastBlock || blk - lastBlock > int64_t(B) * 64)
                lastBlock = blk - B; // (re)start: signal now
            if (blk > lastBlock) {
                h.blockStart.store(blk, std::memory_order_relaxed);
                h.blockTimeNs.store(local - (samplesToNs(S - blk, rate)), std::memory_order_relaxed);
                h.blockCounter.fetch_add(1, std::memory_order_release);
                SetEvent(blockEvt_);
                lastBlock = blk;
            }
        } else {
            lastBlock = INT64_MIN;
            bool tone;
            { std::lock_guard<std::mutex> l(cfgMutex_); tone = cfg_.testTone; }
            if (tone) writeTestTone(S);
        }
    }
    CancelWaitableTimer(timer);
    CloseHandle(timer);
    if (mm) AvRevertMmThreadCharacteristics(mm);
    timeEndPeriod(1);
}

void Engine::writeTestTone(int64_t S) {
    ShmHeader& h = shm_->hdr;
    const uint32_t rate = h.sampleRate.load();
    const int ch = int(h.txChannels.load());
    // keep the tone timeline continuous across short scheduler stalls
    if (toneEnd_ < S - 4096 || toneEnd_ > S + kRingFrames / 2) toneEnd_ = S + 256;
    const int64_t until = S + 1024;
    const float amp = 0.1f; // -20 dBFS
    for (int64_t i = toneEnd_; i < until; ++i) {
        // 1 kHz on odd channels, 500 Hz on even channels, phase derived from the timeline
        const double ph = double(((i % rate) + rate) % rate) / double(rate);
        const float a = amp * float(std::sin(2.0 * 3.14159265358979 * 1000.0 * ph));
        const float b = amp * float(std::sin(2.0 * 3.14159265358979 * 500.0 * ph));
        for (int c = 0; c < ch; ++c) shm_->tx[c][i & kRingMask] = (c & 1) ? b : a;
    }
    if (until > toneEnd_) {
        toneEnd_ = until;
        h.txWriteEnd.store(toneEnd_, std::memory_order_release);
        SetEvent(txEvt_);
    }
}

// ===========================================================================
// TX thread: tx ring -> L24 multicast packets
// ===========================================================================
static inline void putL24(uint8_t* p, float v) {
    if (v > 1.f) v = 1.f; else if (v < -1.f) v = -1.f;
    int32_t s = int32_t(std::lrint(v * 8388607.0f));
    p[0] = uint8_t(s >> 16); p[1] = uint8_t(s >> 8); p[2] = uint8_t(s);
}
static inline float getL24(const uint8_t* p) {
    int32_t s = (int32_t(p[0]) << 24 | int32_t(p[1]) << 16 | int32_t(p[2]) << 8) >> 8;
    return float(s) * (1.0f / 8388608.0f);
}

void Engine::txThread() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    ShmHeader& h = shm_->hdr;
    std::vector<uint8_t> pkt(2048);
    uint32_t seq = 0;
    const uint32_t group = audioGroupFor(cfg_.deviceId);
    txSent_ = 0;
    float peaks[kMaxChannels] = {};

    while (running_) {
        WaitForSingleObject(txEvt_, 5);
        if (!running_) break;
        bool tone; { std::lock_guard<std::mutex> l(cfgMutex_); tone = cfg_.testTone; }
        if (!h.asioRunning.load() && !tone) continue;
        const int64_t end = h.txWriteEnd.load(std::memory_order_acquire);
        const int ch = int(h.txChannels.load());
        const uint32_t rate = h.sampleRate.load();
        const int fpp = framesPerPacket(ch);
        if (txSent_ > end || end - txSent_ > kRingFrames / 2) txSent_ = end;

        while (end - txSent_ >= fpp) {
            Writer w; w.buf.swap(pkt); w.buf.clear();
            w.header(MSG_AUDIO, cfg_.deviceId);
            w.i64(txSent_); w.u32(seq++); w.u32(rate); w.u16(uint16_t(ch)); w.u16(uint16_t(fpp));
            size_t off = w.buf.size();
            w.buf.resize(off + size_t(fpp) * ch * 3);
            uint8_t* p = w.buf.data() + off;
            for (int f = 0; f < fpp; ++f) {
                const int idx = int((txSent_ + f) & kRingMask);
                for (int c = 0; c < ch; ++c, p += 3) {
                    const float v = shm_->tx[c][idx];
                    putL24(p, v);
                    const float a = std::fabs(v);
                    if (a > peaks[c]) peaks[c] = a;
                }
            }
            sendTo(audioTxSock_, w.buf.data(), w.buf.size(), group, kPortAudio);
            pkt.swap(w.buf);
            txSent_ += fpp;
            txPackets_.fetch_add(1, std::memory_order_relaxed);
        }
        for (int c = 0; c < ch; ++c) {
            float cur = txPeak_[c].load(std::memory_order_relaxed);
            if (peaks[c] > cur) txPeak_[c].store(peaks[c], std::memory_order_relaxed);
            peaks[c] = 0;
        }
    }
}

// ===========================================================================
// Audio RX thread: packets -> routed rx ring (presentation = ts + latency)
// ===========================================================================
void Engine::rxAudioThread() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    ShmHeader& h = shm_->hdr;
    std::vector<uint8_t> buf(65536);
    while (running_) {
        uint32_t sip; uint16_t sport;
        int n = recvFrom(audioRxSock_, buf.data(), buf.size(), sip, sport);
        if (n <= 0) continue;
        Reader r(buf.data(), size_t(n));
        uint64_t src = 0;
        if (r.header(src) != MSG_AUDIO) continue;
        const int64_t ts = r.i64(); const uint32_t seq = r.u32(); const uint32_t rate = r.u32();
        const int ch = r.u16(); const int frames = r.u16();
        if (!r.ok() || ch <= 0 || ch > kMaxChannels || frames <= 0 || r.remaining() < size_t(ch) * frames * 3) continue;
        const uint8_t* payload = r.raw(size_t(ch) * frames * 3);

        std::lock_guard<std::mutex> l(routeMutex_);
        auto& ss = sources_[src];
        ss.lastPacket = nowNs();
        if (ss.haveSeq && seq != ss.lastSeq + 1) {
            uint32_t gap = seq - ss.lastSeq - 1;
            if (gap < 10000) rxLost_.fetch_add(gap, std::memory_order_relaxed);
        }
        ss.haveSeq = true; ss.lastSeq = seq;
        auto it = routeMap_.find(src);
        if (it == routeMap_.end()) continue;
        if (rate != h.sampleRate.load()) { ss.rateMismatch = true; continue; }
        ss.rateMismatch = false;
        rxPackets_.fetch_add(1, std::memory_order_relaxed);

        const bool asio = h.asioRunning.load() != 0;
        const int64_t readPos = h.rxReadPos.load(std::memory_order_acquire);
        const int64_t P0 = ts + h.latencySamples.load();
        bool late = false;
        for (auto& pr : it->second.txToRx) {
            const int txc = pr.first, rxc = pr.second;
            if (txc >= ch) continue;
            float peak = 0;
            for (int f = 0; f < frames; ++f) {
                const float v = getL24(payload + (size_t(f) * ch + txc) * 3);
                const float a = std::fabs(v); if (a > peak) peak = a;
                if (!asio) continue;
                const int64_t P = P0 + f;
                if (P < readPos) { late = true; continue; }
                if (P >= readPos + kRingFrames - 2048) continue; // too early: clock not settled
                shm_->rx[rxc][P & kRingMask] = v;
            }
            float cur = rxPeak_[rxc].load(std::memory_order_relaxed);
            if (peak > cur) rxPeak_[rxc].store(peak, std::memory_order_relaxed);
        }
        // Race check: if the driver consumed this window while we were writing, it was late too.
        if (asio && !late && h.rxReadPos.load(std::memory_order_acquire) > P0) late = true;
        if (late) rxLate_.fetch_add(1, std::memory_order_relaxed);
    }
}

// ===========================================================================
// Net thread: discovery, clock, control
// ===========================================================================
void Engine::netThread() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    std::vector<uint8_t> buf(8192);
    while (running_) {
        fd_set rs; FD_ZERO(&rs);
        FD_SET(discSock_, &rs); FD_SET(clockSock_, &rs); FD_SET(ctrlSock_, &rs);
        timeval tv{0, 50000};
        int k = select(0, &rs, nullptr, nullptr, &tv);
        if (k <= 0) continue;
        uint32_t sip; uint16_t sport;
        if (FD_ISSET(clockSock_, &rs)) {
            int n = recvFrom(clockSock_, buf.data(), buf.size(), sip, sport);
            const int64_t t = nowNs();
            if (n > 0) handleClock(buf.data(), n, sip, t);
        }
        if (FD_ISSET(discSock_, &rs)) {
            int n = recvFrom(discSock_, buf.data(), buf.size(), sip, sport);
            if (n > 0) handleDiscovery(buf.data(), n, sip);
        }
        if (FD_ISSET(ctrlSock_, &rs)) {
            int n = recvFrom(ctrlSock_, buf.data(), buf.size(), sip, sport);
            if (n > 0) handleControl(buf.data(), n, sip, sport);
        }
    }
}

void Engine::handleDiscovery(const uint8_t* p, int n, uint32_t srcIp) {
    Reader r(p, size_t(n));
    uint64_t id = 0;
    uint8_t t = r.header(id);
    if (t != MSG_ANNOUNCE || id == cfg_.deviceId) return;
    Announce a;
    if (!a.read(r)) return;
    std::lock_guard<std::mutex> l(peersMutex_);
    Peer& pe = peers_[id];
    pe.ann = a; pe.srcIp = srcIp; pe.lastSeen = nowNs();
}

void Engine::handleClock(const uint8_t* p, int n, uint32_t srcIp, int64_t rxLocal) {
    Reader r(p, size_t(n));
    uint64_t id = 0;
    uint8_t t = r.header(id);
    if (id == cfg_.deviceId) return;
    if (t == MSG_SYNC) {
        r.u32(); const int64_t t1 = r.i64();
        if (!r.ok() || id != leaderId_.load() || clock_.isLeader()) return;
        clock_.onSync(t1, rxLocal);
    } else if (t == MSG_DELAY_REQ) {
        const uint32_t seq = r.u32();
        if (!r.ok() || !clock_.isLeader()) return;
        const int64_t t4 = clock_.netAt(rxLocal);
        Writer w; w.header(MSG_DELAY_RESP, cfg_.deviceId);
        w.u32(seq); w.i64(t4); w.u64(id);
        sendTo(clockSock_, w.buf.data(), w.buf.size(), srcIp, kPortClock);
    } else if (t == MSG_DELAY_RESP) {
        const uint32_t seq = r.u32(); const int64_t t4 = r.i64(); const uint64_t req = r.u64();
        if (!r.ok() || req != cfg_.deviceId || id != leaderId_.load()) return;
        int64_t t3 = 0;
        {
            std::lock_guard<std::mutex> l(delayMutex_);
            auto it = pendingDelay_.find(seq);
            if (it == pendingDelay_.end()) return;
            t3 = it->second; pendingDelay_.erase(it);
        }
        clock_.onDelayResp(t3, t4);
    }
}

void Engine::handleControl(const uint8_t* p, int n, uint32_t srcIp, uint16_t srcPort) {
    Reader r(p, size_t(n));
    uint64_t id = 0;
    if (r.header(id) != MSG_CONTROL) return;
    const uint64_t target = r.u64();
    const uint32_t reqId = r.u32();
    const uint8_t cmd = r.u8();
    if (!r.ok() || target != cfg_.deviceId) return;
    uint8_t status = 0;
    bool shmChanged = false, rateChange = false; uint32_t newRate = 0;
    {
        std::lock_guard<std::mutex> l(cfgMutex_);
        switch (cmd) {
        case CMD_SET_ROUTE: {
            uint8_t rx = r.u8(), tx = r.u8(); std::string src = r.str(kNameLen);
            if (!r.ok() || rx >= kMaxChannels || tx >= kMaxChannels) { status = 1; break; }
            if (src.empty()) cfg_.routes.erase(rx);
            else { Route ro; ro.rxCh = rx; ro.txCh = tx; ro.src = sanitizeName(src); cfg_.routes[rx] = ro; }
            break; }
        case CMD_SET_NAME: { std::string nm = r.str(kNameLen); if (r.ok()) { cfg_.name = sanitizeName(nm); shmChanged = true; } else status = 1; break; }
        case CMD_SET_LATENCY: { uint32_t us = r.u32(); if (r.ok() && us >= 500 && us <= 40000) { cfg_.latencyUs = us; shmChanged = true; } else status = 1; break; }
        case CMD_SET_RATE: { uint32_t rt = r.u32(); bool ok = false; for (auto x : kRates) ok |= x == rt;
                             if (r.ok() && ok) { rateChange = true; newRate = rt; } else status = 1; break; }
        case CMD_SET_PRIORITY: { uint8_t v = r.u8(); if (r.ok()) cfg_.preferredLeader = v != 0; break; }
        case CMD_SET_CHNAME: { uint8_t dir = r.u8(), ch = r.u8(); std::string nm = r.str(kChanNameLen);
                               if (r.ok() && ch < kMaxChannels) { (dir ? cfg_.rxNames : cfg_.txNames)[ch] = sanitizeChan(nm); shmChanged = true; } else status = 1; break; }
        case CMD_CLEAR_ROUTES: cfg_.routes.clear(); break;
        case CMD_SET_TESTTONE: { uint8_t v = r.u8(); if (r.ok()) cfg_.testTone = v != 0; break; }
        case CMD_SET_CHCOUNT: { int tx = r.u16(), rx = r.u16();
                                if (r.ok() && tx >= 1 && tx <= kMaxChannels && rx >= 1 && rx <= kMaxChannels) {
                                    cfg_.txChannels = tx; cfg_.rxChannels = rx;
                                    for (auto it = cfg_.routes.begin(); it != cfg_.routes.end();) it = it->first >= rx ? cfg_.routes.erase(it) : std::next(it);
                                    shmChanged = true;
                                } else status = 1; break; }
        default: status = 2;
        }
        cfg_.save();
    }
    if (rateChange) applyRate(newRate);
    else if (shmChanged) publishConfigToShm(cmd == CMD_SET_CHCOUNT);
    rebuildRouting(nowNs());
    Writer w; w.header(MSG_CONTROL_ACK, cfg_.deviceId); w.u32(reqId); w.u8(status);
    sendTo(ctrlSock_, w.buf.data(), w.buf.size(), srcIp, srcPort);
}

void Engine::applyRate(uint32_t rate) {
    {
        std::lock_guard<std::mutex> l(cfgMutex_);
        cfg_.rate = rate;
        cfg_.save();
    }
    publishConfigToShm(true); // DLL sees configRev change and asks the host for a reset
}

// ===========================================================================
// House-keeping: sync/delay/announce/meters, leader election, routing
// ===========================================================================
Announce Engine::buildAnnounce() {
    Announce a;
    {
        std::lock_guard<std::mutex> l(cfgMutex_);
        a.name = cfg_.name; a.ip = cfg_.ifaceIp; a.sampleRate = cfg_.rate;
        a.txChannels = uint16_t(cfg_.txChannels); a.rxChannels = uint16_t(cfg_.rxChannels);
        a.latencyUs = cfg_.latencyUs; a.preferredLeader = cfg_.preferredLeader ? 1 : 0;
        a.testTone = cfg_.testTone ? 1 : 0;
    }
    a.audioGroup = audioGroupFor(cfg_.deviceId);
    auto cs = clock_.status();
    a.isLeader = clock_.isLeader() ? 1 : 0;
    a.clockState = cs.state;
    a.offsetNs = int32_t(std::max<int64_t>(INT32_MIN, std::min<int64_t>(INT32_MAX, cs.lastErrorNs)));
    a.leaderId = leaderId_.load();
    a.asioActive = shm_->hdr.asioRunning.load() ? 1 : 0;
    a.asioBuffer = a.asioActive ? shm_->hdr.asioBufferSize.load() : 0;
    a.rxPackets = rxPackets_.load(); a.rxLate = rxLate_.load(); a.rxLost = rxLost_.load(); a.txPackets = txPackets_.load();
    a.uptimeS = uint32_t((nowNs() - startNs_) / 1000000000LL);
    a.version = kVersionString;
    return a;
}

void Engine::sendAnnounce() {
    const uint32_t grp = parseIp(kGroupDiscovery);
    Announce a = buildAnnounce();
    {   // keep ourselves in the peer table (self routing works even without multicast loopback)
        std::lock_guard<std::mutex> l(peersMutex_);
        Peer& me = peers_[cfg_.deviceId];
        me.ann = a; me.srcIp = cfg_.ifaceIp; me.lastSeen = nowNs();
    }
    Writer w; w.header(MSG_ANNOUNCE, cfg_.deviceId); a.write(w);
    sendTo(discSock_, w.buf.data(), w.buf.size(), grp, kPortDiscovery);

    Writer wr; wr.header(MSG_ROUTES, cfg_.deviceId);
    { std::lock_guard<std::mutex> l(routeMutex_); writeRoutes(wr, routeStatus_); }
    sendTo(discSock_, wr.buf.data(), wr.buf.size(), grp, kPortDiscovery);

    for (int dir = 0; dir < 2; ++dir) {
        Writer wn; wn.header(MSG_NAMES, cfg_.deviceId);
        std::lock_guard<std::mutex> l(cfgMutex_);
        const int cnt = dir ? cfg_.rxChannels : cfg_.txChannels;
        wn.u8(uint8_t(dir)); wn.u8(0); wn.u8(uint8_t(cnt));
        for (int i = 0; i < cnt; ++i) wn.str(dir ? cfg_.rxNames[i] : cfg_.txNames[i], kChanNameLen);
        sendTo(discSock_, wn.buf.data(), wn.buf.size(), grp, kPortDiscovery);
    }
}

void Engine::sendMeters() {
    int tx, rx;
    { std::lock_guard<std::mutex> l(cfgMutex_); tx = cfg_.txChannels; rx = cfg_.rxChannels; }
    Writer w; w.header(MSG_METERS, cfg_.deviceId);
    w.u8(uint8_t(tx)); for (int i = 0; i < tx; ++i) w.u8(peakToMeter(txPeak_[i].exchange(0.f)));
    w.u8(uint8_t(rx)); for (int i = 0; i < rx; ++i) w.u8(peakToMeter(rxPeak_[i].exchange(0.f)));
    sendTo(discSock_, w.buf.data(), w.buf.size(), parseIp(kGroupDiscovery), kPortDiscovery);
}

void Engine::electLeader(int64_t now) {
    if (now - startNs_ < 2500000000LL) return; // listen first
    uint64_t bestId = cfg_.deviceId;
    bool bestPref;
    { std::lock_guard<std::mutex> l(cfgMutex_); bestPref = cfg_.preferredLeader; }
    uint32_t bestIp = cfg_.ifaceIp;
    {
        std::lock_guard<std::mutex> l(peersMutex_);
        for (auto& kv : peers_) {
            if (kv.first == cfg_.deviceId) continue;
            if (now - kv.second.lastSeen > 3000000000LL) continue;
            const bool pref = kv.second.ann.preferredLeader != 0;
            if ((pref && !bestPref) || (pref == bestPref && kv.first < bestId)) {
                bestId = kv.first; bestPref = pref; bestIp = kv.second.srcIp;
            }
        }
    }
    if (bestId == cfg_.deviceId) {
        if (!clock_.isLeader()) clock_.setLeader(now);
    } else {
        clock_.setFollower();
    }
    leaderId_ = bestId; leaderIp_ = bestIp;
}

void Engine::rebuildRouting(int64_t now) {
    std::map<int, Route> routes; uint32_t myRate;
    { std::lock_guard<std::mutex> l(cfgMutex_); routes = cfg_.routes; myRate = cfg_.rate; }
    std::map<std::string, std::pair<uint64_t, Announce>> byName;
    {
        std::lock_guard<std::mutex> l(peersMutex_);
        for (auto it = peers_.begin(); it != peers_.end();) {
            if (it->first != cfg_.deviceId && now - it->second.lastSeen > 5000000000LL) it = peers_.erase(it);
            else { byName[it->second.ann.name] = {it->first, it->second.ann}; ++it; }
        }
    }
    std::map<uint64_t, RouteMap> rm;
    std::vector<RouteStatusEntry> st;
    std::set<uint32_t> groups;
    std::lock_guard<std::mutex> l(routeMutex_);
    for (auto& kv : routes) {
        RouteStatusEntry e; e.route = kv.second;
        auto f = byName.find(kv.second.src);
        if (f == byName.end()) e.status = RS_NO_SOURCE;
        else {
            const uint64_t sid = f->second.first;
            const Announce& a = f->second.second;
            if (a.sampleRate != myRate) e.status = RS_RATE_MISMATCH;
            else {
                groups.insert(a.audioGroup);
                rm[sid].txToRx.push_back({kv.second.txCh, kv.first});
                auto s = sources_.find(sid);
                e.status = (s == sources_.end() || now - s->second.lastPacket > 1500000000LL) ? RS_NO_AUDIO : RS_OK;
            }
        }
        st.push_back(e);
    }
    routeMap_.swap(rm);
    routeStatus_.swap(st);
    const uint32_t ifc = cfg_.ifaceIp;
    for (uint32_t g : joinedGroups_) if (!groups.count(g)) leaveGroup(audioRxSock_, g, ifc);
    for (uint32_t g : groups) if (!joinedGroups_.count(g)) joinGroup(audioRxSock_, g, ifc);
    joinedGroups_.swap(groups);
    for (auto it = sources_.begin(); it != sources_.end();)
        it = (now - it->second.lastPacket > 10000000000LL) ? sources_.erase(it) : std::next(it);
}

void Engine::houseThread() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    ShmHeader& h = shm_->hdr;
    int64_t nextSync = 0, nextDelay = 0, nextAnn = 0, nextMeter = 0, nextElect = 0;
    while (running_) {
        Sleep(5);
        const int64_t now = nowNs();
        const bool leader = clock_.isLeader();

        if (leader && now >= nextSync) {
            nextSync = now + 125000000LL;
            Writer w; w.header(MSG_SYNC, cfg_.deviceId); w.u32(syncSeq_++);
            const int64_t t1 = clock_.netAt(nowNs());
            w.i64(t1);
            sendTo(clockSock_, w.buf.data(), w.buf.size(), parseIp(kGroupClock), kPortClock);
        }
        if (!leader && leaderId_.load() != 0 && now >= nextDelay) {
            nextDelay = now + 500000000LL;
            const uint32_t seq = delaySeq_++;
            Writer w; w.header(MSG_DELAY_REQ, cfg_.deviceId); w.u32(seq);
            const int64_t t3 = nowNs();
            {
                std::lock_guard<std::mutex> l(delayMutex_);
                pendingDelay_[seq] = t3;
                while (pendingDelay_.size() > 8) pendingDelay_.erase(pendingDelay_.begin());
            }
            sendTo(clockSock_, w.buf.data(), w.buf.size(), leaderIp_.load(), kPortClock);
        }
        if (now >= nextMeter) { nextMeter = now + 100000000LL; sendMeters(); }
        if (now >= nextElect) {
            nextElect = now + 500000000LL;
            electLeader(now);
            rebuildRouting(now);
            h.clockState = clock_.status().state;
            // Host process gone without stopping the driver (crash)?
            const uint32_t pid = h.asioPid.load();
            if (h.asioRunning.load() && pid) {
                HANDLE ph = OpenProcess(SYNCHRONIZE, FALSE, pid);
                bool alive = ph && WaitForSingleObject(ph, 0) == WAIT_TIMEOUT;
                if (ph) CloseHandle(ph);
                if (!alive) { h.asioRunning = 0; h.asioPid = 0; }
            }
            const uint32_t req = h.rateRequest.exchange(0);
            if (req) {
                bool ok = false; for (auto x : kRates) ok |= x == req;
                if (ok && req != h.sampleRate.load()) applyRate(req);
            }
        }
        if (now >= nextAnn) { nextAnn = now + 1000000000LL; sendAnnounce(); }
    }
}

} // namespace slink
