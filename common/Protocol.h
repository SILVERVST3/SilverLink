// SilverLink - audio over IP (PC <-> PC)
// Copyright (C) 2026 Jorge Prata Alves Junior
// Licensed under GPLv3 (see LICENSE)
//
// Wire protocol shared by Engine and Controller.
// All multi-byte integers on the wire are big-endian (network order).
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace slink {

constexpr uint32_t kMagic        = 0x534C4E4B; // 'SLNK'
constexpr uint8_t  kProtoVersion = 1;

// ---- Network ports / groups ------------------------------------------
constexpr uint16_t kPortDiscovery = 7700;   // multicast: announce / routes / names / meters
constexpr uint16_t kPortControl   = 7701;   // unicast: controller -> engine commands
constexpr uint16_t kPortClock     = 7702;   // multicast sync + unicast delay req/resp
constexpr uint16_t kPortAudio     = 7710;   // multicast audio flows

constexpr const char* kGroupDiscovery = "239.255.77.77";
constexpr const char* kGroupClock     = "239.255.77.78";
// Audio flows: 239.77.X.Y derived from device id.

// ---- Limits -------------------------------------------------------------
constexpr int kMaxChannels   = 64;   // hard limit of the shared memory layout
constexpr int kDefaultChans  = 32;   // 32x32 as requested
constexpr int kNameLen       = 32;   // device name (incl. NUL)
constexpr int kChanNameLen   = 16;   // channel label (incl. NUL)
constexpr int kMaxUdpPayload = 1400; // stay below Ethernet MTU

// ---- Message types --------------------------------------------------------
enum MsgType : uint8_t {
    MSG_ANNOUNCE   = 1,
    MSG_ROUTES     = 2,
    MSG_NAMES      = 3,
    MSG_METERS     = 4,
    MSG_SYNC       = 10,
    MSG_DELAY_REQ  = 11,
    MSG_DELAY_RESP = 12,
    MSG_CONTROL    = 20,
    MSG_CONTROL_ACK= 21,
    MSG_AUDIO      = 30,
};

enum ControlCmd : uint8_t {
    CMD_SET_ROUTE    = 1,  // rxCh, txCh, srcName  (empty srcName = remove)
    CMD_SET_NAME     = 2,  // name
    CMD_SET_LATENCY  = 3,  // u32 microseconds
    CMD_SET_RATE     = 4,  // u32 Hz
    CMD_SET_PRIORITY = 5,  // u8 (1 = preferred leader)
    CMD_SET_CHNAME   = 6,  // u8 dir (0=tx,1=rx), u8 ch, name
    CMD_CLEAR_ROUTES = 7,
    CMD_SET_TESTTONE = 8,  // u8 on/off
    CMD_SET_CHCOUNT  = 9,  // u16 tx, u16 rx
};

enum ClockState : uint8_t { CLK_FREE = 0, CLK_LOCKING = 1, CLK_LOCKED = 2 };

constexpr uint32_t kRates[] = { 44100, 48000, 88200, 96000 };
constexpr uint32_t kLatenciesUs[] = { 1000, 2000, 4000, 6000, 10000, 20000 };

// ---- Byte serializer ------------------------------------------------------
class Writer {
public:
    std::vector<uint8_t> buf;
    void u8(uint8_t v)  { buf.push_back(v); }
    void u16(uint16_t v){ u8(uint8_t(v >> 8)); u8(uint8_t(v)); }
    void u32(uint32_t v){ u16(uint16_t(v >> 16)); u16(uint16_t(v)); }
    void u64(uint64_t v){ u32(uint32_t(v >> 32)); u32(uint32_t(v)); }
    void i32(int32_t v) { u32(uint32_t(v)); }
    void i64(int64_t v) { u64(uint64_t(v)); }
    void str(const std::string& s, int fixedLen) {
        for (int i = 0; i < fixedLen; ++i)
            u8(i < fixedLen - 1 && i < (int)s.size() ? uint8_t(s[i]) : 0);
    }
    void header(MsgType t, uint64_t deviceId) {
        u32(kMagic); u8(kProtoVersion); u8(t); u16(0); u64(deviceId);
    }
};

class Reader {
public:
    Reader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    bool ok() const { return ok_; }
    size_t remaining() const { return ok_ ? n_ - pos_ : 0; }
    uint8_t  u8()  { if (!need(1)) return 0; return p_[pos_++]; }
    uint16_t u16() { uint16_t a = u8(); return uint16_t((a << 8) | u8()); }
    uint32_t u32() { uint32_t a = u16(); return (a << 16) | u16(); }
    uint64_t u64() { uint64_t a = u32(); return (a << 32) | u32(); }
    int32_t  i32() { return int32_t(u32()); }
    int64_t  i64() { return int64_t(u64()); }
    std::string str(int fixedLen) {
        if (!need(fixedLen)) return {};
        std::string s(reinterpret_cast<const char*>(p_ + pos_), strnlen(reinterpret_cast<const char*>(p_ + pos_), fixedLen));
        pos_ += fixedLen;
        return s;
    }
    const uint8_t* raw(size_t n) { if (!need(n)) return nullptr; const uint8_t* r = p_ + pos_; pos_ += n; return r; }
    // Returns message type, fills deviceId; 0 on invalid.
    uint8_t header(uint64_t& deviceId) {
        if (u32() != kMagic) { ok_ = false; return 0; }
        if (u8() != kProtoVersion) { ok_ = false; return 0; }
        uint8_t t = u8(); u16(); deviceId = u64();
        return ok_ ? t : 0;
    }
private:
    bool need(size_t k) { if (!ok_ || pos_ + k > n_) { ok_ = false; return false; } return true; }
    const uint8_t* p_; size_t n_; size_t pos_ = 0; bool ok_ = true;
};

constexpr size_t kHeaderSize = 4 + 1 + 1 + 2 + 8; // 16

// ---- Message payloads -----------------------------------------------------
struct Announce {
    std::string name;
    uint32_t ip = 0;            // host order
    uint32_t audioGroup = 0;    // host order
    uint32_t sampleRate = 44100;
    uint16_t txChannels = kDefaultChans, rxChannels = kDefaultChans;
    uint32_t latencyUs = 4000;
    uint8_t  isLeader = 0, clockState = CLK_FREE, preferredLeader = 0, asioActive = 0;
    int32_t  offsetNs = 0;
    uint64_t leaderId = 0;
    uint32_t rxPackets = 0, rxLate = 0, rxLost = 0, txPackets = 0;
    uint32_t asioBuffer = 0;
    uint8_t  testTone = 0;
    uint32_t uptimeS = 0;
    std::string version;

    void write(Writer& w) const {
        w.str(name, kNameLen); w.u32(ip); w.u32(audioGroup); w.u32(sampleRate);
        w.u16(txChannels); w.u16(rxChannels); w.u32(latencyUs);
        w.u8(isLeader); w.u8(clockState); w.u8(preferredLeader); w.u8(asioActive);
        w.i32(offsetNs); w.u64(leaderId);
        w.u32(rxPackets); w.u32(rxLate); w.u32(rxLost); w.u32(txPackets);
        w.u32(asioBuffer); w.u8(testTone); w.u32(uptimeS); w.str(version, 16);
    }
    bool read(Reader& r) {
        name = r.str(kNameLen); ip = r.u32(); audioGroup = r.u32(); sampleRate = r.u32();
        txChannels = r.u16(); rxChannels = r.u16(); latencyUs = r.u32();
        isLeader = r.u8(); clockState = r.u8(); preferredLeader = r.u8(); asioActive = r.u8();
        offsetNs = r.i32(); leaderId = r.u64();
        rxPackets = r.u32(); rxLate = r.u32(); rxLost = r.u32(); txPackets = r.u32();
        asioBuffer = r.u32(); testTone = r.u8(); uptimeS = r.u32(); version = r.str(16);
        return r.ok();
    }
};

struct Route {           // rx channel <- srcName:txCh
    uint8_t rxCh = 0;
    uint8_t txCh = 0;
    std::string src;     // source device name
};

struct RouteStatusEntry {
    Route route;
    uint8_t status = 0;  // 0=ok, 1=source not found, 2=rate mismatch, 3=no audio
};
enum RouteStatus : uint8_t { RS_OK = 0, RS_NO_SOURCE = 1, RS_RATE_MISMATCH = 2, RS_NO_AUDIO = 3 };

inline void writeRoutes(Writer& w, const std::vector<RouteStatusEntry>& v) {
    w.u8(uint8_t(v.size()));
    for (auto& e : v) { w.u8(e.route.rxCh); w.u8(e.route.txCh); w.u8(e.status); w.str(e.route.src, kNameLen); }
}
inline bool readRoutes(Reader& r, std::vector<RouteStatusEntry>& v) {
    v.clear(); int n = r.u8();
    for (int i = 0; i < n && r.ok(); ++i) {
        RouteStatusEntry e; e.route.rxCh = r.u8(); e.route.txCh = r.u8(); e.status = r.u8(); e.route.src = r.str(kNameLen);
        v.push_back(e);
    }
    return r.ok();
}

// NAMES: dir u8, first u8, count u8, then count * kChanNameLen bytes.
// Meters: tx u8 count + values, rx u8 count + values (0..255 -> -60..0 dBFS, 0 = silent)
inline uint8_t peakToMeter(float peak) {
    if (peak <= 0.001f) return 0; // -60 dB
    float db = 20.0f * std::log10(peak);
    if (db > 0) db = 0;
    return uint8_t(1 + (db + 60.0f) / 60.0f * 254.0f);
}
inline float meterToDb(uint8_t m) { return m == 0 ? -100.f : -60.0f + (m - 1) / 254.0f * 60.0f; }

// ---- Audio packet ---------------------------------------------------------
// header(16) + i64 timestamp(first frame, media samples) + u32 seq + u32 rate
// + u16 channels + u16 frames + payload (L24 big-endian interleaved)
constexpr size_t kAudioHdr = kHeaderSize + 8 + 4 + 4 + 2 + 2; // 36
inline int framesPerPacket(int channels) {
    if (channels <= 0) return 0;
    int f = int((kMaxUdpPayload - kAudioHdr) / (size_t(channels) * 3));
    return f < 1 ? 1 : (f > 64 ? 64 : f);
}

inline uint32_t audioGroupFor(uint64_t id) {
    uint32_t h = uint32_t(id ^ (id >> 21) ^ (id >> 42));
    uint32_t x = (h >> 8) & 0xFF, y = h & 0xFF;
    if (y == 0) y = 1;
    return (239u << 24) | (77u << 16) | (x << 8) | y;
}

inline std::string ipToString(uint32_t ip) {
    return std::to_string((ip >> 24) & 255) + "." + std::to_string((ip >> 16) & 255) + "." +
           std::to_string((ip >> 8) & 255) + "." + std::to_string(ip & 255);
}

constexpr const char* kVersionString = "1.0.0";

} // namespace slink
