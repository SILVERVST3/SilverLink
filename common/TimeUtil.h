// SilverLink - time helpers
// Copyright (C) 2026 Jorge Prata Alves Junior - GPLv3
#pragma once
#include <cstdint>
#include <windows.h>

namespace slink {

inline int64_t qpcFreq() {
    static int64_t f = [] { LARGE_INTEGER li; QueryPerformanceFrequency(&li); return li.QuadPart; }();
    return f;
}

// Monotonic local time in nanoseconds (overflow safe).
inline int64_t nowNs() {
    LARGE_INTEGER li; QueryPerformanceCounter(&li);
    const int64_t f = qpcFreq();
    const int64_t q = li.QuadPart;
    return (q / f) * 1000000000LL + ((q % f) * 1000000000LL) / f;
}

// Network ns -> media samples (floor), overflow safe.
inline int64_t nsToSamples(int64_t ns, uint32_t rate) {
    if (ns < 0) return -nsToSamples(-ns, rate) - 1;
    return (ns / 1000000000LL) * rate + ((ns % 1000000000LL) * rate) / 1000000000LL;
}
// Media samples -> network ns.
inline int64_t samplesToNs(int64_t s, uint32_t rate) {
    if (rate == 0) return 0;
    return (s / rate) * 1000000000LL + ((s % rate) * 1000000000LL) / rate;
}

} // namespace slink
