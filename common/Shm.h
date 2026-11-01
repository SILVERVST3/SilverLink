// SilverLink - shared memory between Engine (SilverLink.exe) and the ASIO driver DLL
// Copyright (C) 2026 Jorge Prata Alves Junior - GPLv3
#pragma once
#include <atomic>
#include <cstdint>
#include <windows.h>
#include <sddl.h>
#include "Protocol.h"

namespace slink {

constexpr uint32_t kShmMagic   = 0x53484D31; // 'SHM1'
constexpr uint32_t kShmVersion = 1;
constexpr int      kRingFrames = 8192;       // power of two (~186 ms @ 44.1k)
constexpr int      kRingMask   = kRingFrames - 1;

constexpr const wchar_t* kShmName      = L"Local\\SilverLink_SHM_v1";
constexpr const wchar_t* kBlockEvtName = L"Local\\SilverLink_BlockEvt_v1";
constexpr const wchar_t* kTxEvtName    = L"Local\\SilverLink_TxEvt_v1";
constexpr const wchar_t* kEngineMutex  = L"Local\\SilverLink_Engine_Single";

// Timeline: "media samples" = network time (ns) * rate / 1e9. All devices with the
// same sample rate share the same timeline, so ASIO blocks are aligned across PCs.
//
// Engine -> DLL: at media time S (multiple of buffer size B) the engine publishes
//   blockStart=S and signals the block event.
// DLL: input  = rx ring [S-B, S)   (presentation timeline, already delayed by latency)
//      output = written to tx ring [S+B, S+2B) and txWriteEnd = S+2B
//      (stamped one buffer ahead: the host has one buffer period to render it)
struct alignas(64) ShmHeader {
    uint32_t magic;
    uint32_t version;
    std::atomic<uint32_t> sampleRate;
    std::atomic<uint32_t> txChannels;     // driver OUTPUTS  -> network
    std::atomic<uint32_t> rxChannels;     // network -> driver INPUTS
    std::atomic<int32_t>  latencySamples;
    std::atomic<uint32_t> preferredBuffer;
    std::atomic<uint32_t> engineHeartbeat; // increments every ms
    std::atomic<uint32_t> clockState;
    std::atomic<uint32_t> engineConfigRev; // bumps on rate/channel change -> DLL requests reset

    // DLL -> engine
    std::atomic<uint32_t> asioRunning;
    std::atomic<uint32_t> asioBufferSize;
    std::atomic<uint32_t> asioPid;
    std::atomic<uint32_t> rateRequest;     // 0 = none
    std::atomic<int64_t>  rxReadPos;       // presentation index consumed up to
    std::atomic<int64_t>  txWriteEnd;      // tx ring valid up to (exclusive)

    // Engine -> DLL
    std::atomic<int64_t>  blockStart;
    std::atomic<int64_t>  blockTimeNs;     // local QPC-ns when block was signalled
    std::atomic<uint32_t> blockCounter;

    char txNames[kMaxChannels][kChanNameLen];
    char rxNames[kMaxChannels][kChanNameLen];
    char deviceName[kNameLen];
};

struct ShmLayout {
    ShmHeader hdr;
    alignas(64) float rx[kMaxChannels][kRingFrames];
    alignas(64) float tx[kMaxChannels][kRingFrames];
};

// Security descriptor: everyone + low integrity, so an elevated host and a normal
// engine (or vice versa) can still share the objects.
inline bool makeOpenSa(SECURITY_ATTRIBUTES& sa, PSECURITY_DESCRIPTOR& sd) {
    sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(A;;GA;;;WD)(A;;GA;;;AN)(A;;GA;;;SY)S:(ML;;NW;;;LW)", SDDL_REVISION_1, &sd, nullptr))
        return false;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = sd;
    sa.bInheritHandle = FALSE;
    return true;
}

} // namespace slink
