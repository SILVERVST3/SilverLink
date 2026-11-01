// SilverLink - network clock (simplified PTP: Sync + Delay_Req/Resp, software timestamps)
// Copyright (C) 2026 Jorge Prata Alves Junior - GPLv3
//
// Every engine keeps a "network time" in ns derived from its local monotonic clock:
//     net(local) = baseNet + (local - baseLocal) * (1 + freqAdj)
// The leader never adjusts. Followers steer freqAdj with a PI servo so that their
// network time follows the leader's. The mapping is always continuous (re-based on
// every change) except on a "step" (initial lock or a large error), which is counted
// so the audio side can resynchronise.
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <vector>

namespace slink {

class NetClock {
public:
    struct Status {
        uint8_t state = 0;        // ClockState
        int64_t lastErrorNs = 0;  // last measured offset to leader
        int64_t pathDelayNs = 0;
        double  freqPpm = 0;
        uint32_t steps = 0;
    };

    int64_t netAt(int64_t local) const {
        std::lock_guard<std::mutex> l(m_);
        return netAtLocked(local);
    }

    // Leader: free running, continuous from wherever we were.
    void setLeader(int64_t nowLocal) {
        std::lock_guard<std::mutex> l(m_);
        rebase(nowLocal);
        freqAdj_ = 0; integ_ = 0;
        leader_ = true; state_ = 2; errEma_ = 0; goodCount_ = 0;
        syncWin_.clear(); msWin_.clear(); smWin_.clear(); pathDelay_ = -1;
    }
    void setFollower() {
        std::lock_guard<std::mutex> l(m_);
        if (leader_) { leader_ = false; state_ = 1; goodCount_ = 0; syncWin_.clear(); msWin_.clear(); smWin_.clear(); pathDelay_ = -1; }
    }
    bool isLeader() const { std::lock_guard<std::mutex> l(m_); return leader_; }

    // Follower: Sync received. t1 = leader net time at send, t2 = local time at receive.
    void onSync(int64_t t1, int64_t t2) {
        std::lock_guard<std::mutex> l(m_);
        if (leader_) return;
        haveSync_ = true;
        pushWin(msWin_, netAtLocked(t2) - t1);
        syncWin_.push_back({t1, t2});
        if (syncWin_.size() < kWindow && state_ != 0) return;
        // Pick the sample with the smallest apparent transit (least scheduling delay).
        auto best = *std::min_element(syncWin_.begin(), syncWin_.end(), [&](const Samp& a, const Samp& b) {
            return (netAtLocked(a.t2) - a.t1) < (netAtLocked(b.t2) - b.t1);
        });
        syncWin_.clear();
        process(best.t1, best.t2);
    }

    // Follower: we sent Delay_Req at local t3, leader received it at net time t4.
    void onDelayResp(int64_t t3, int64_t t4) {
        std::lock_guard<std::mutex> l(m_);
        if (leader_ || !haveSync_) return;
        const int64_t sm = t4 - netAtLocked(t3);           // d + e + jitter
        if (std::llabs(sm) > 50000000) return;              // nonsense on a LAN
        pushWin(smWin_, sm);
        if (msWin_.empty()) return;
        // Minimum of each direction removes most of the (always positive) OS jitter.
        const int64_t ms = *std::min_element(msWin_.begin(), msWin_.end()); // d - e
        const int64_t mn = *std::min_element(smWin_.begin(), smWin_.end()); // d + e
        int64_t d = (ms + mn) / 2;
        pathDelay_ = d < 0 ? 0 : d;
    }

    Status status() const {
        std::lock_guard<std::mutex> l(m_);
        Status s; s.state = uint8_t(state_); s.lastErrorNs = lastErr_; s.pathDelayNs = pathDelay_ < 0 ? 0 : pathDelay_;
        s.freqPpm = freqAdj_ * 1e6; s.steps = steps_;
        return s;
    }
    uint32_t steps() const { return stepsAtomic_.load(); }

    // Gains (exposed for the unit test)
    double kp = 0.30, ki = 0.04;

private:
    struct Samp { int64_t t1, t2; };
    static constexpr size_t kWindow = 4;
    static constexpr int64_t kStepThreshold = 1000000;   // 1 ms
    static constexpr double  kMaxFreq = 500e-6;           // +-500 ppm

    int64_t netAtLocked(int64_t local) const {
        const int64_t dl = local - baseLocal_;
        return baseNet_ + dl + int64_t(std::llround(double(dl) * freqAdj_));
    }
    void rebase(int64_t nowLocal) {
        baseNet_ = netAtLocked(nowLocal);
        baseLocal_ = nowLocal;
    }

    void process(int64_t t1, int64_t t2) {
        const int64_t d = pathDelay_ < 0 ? 0 : pathDelay_;
        const int64_t target = t1 + d;
        const int64_t e = target - netAtLocked(t2);
        lastErr_ = e;
        if (state_ == 0 || std::llabs(e) > kStepThreshold) {
            // Step: jump so that net(t2) == target.
            baseNet_ = target; baseLocal_ = t2;
            integ_ = (state_ == 0) ? 0 : integ_;
            ++steps_; stepsAtomic_.store(steps_);
            msWin_.clear(); smWin_.clear();
            state_ = 1; goodCount_ = 0; errEma_ = 0; lastProcLocal_ = t2;
            return;
        }
        double T = double(t2 - lastProcLocal_);
        if (T < 1e7) T = 1e7;         // >= 10 ms
        lastProcLocal_ = t2;
        const double r = double(e) / T;
        integ_ += ki * r;
        integ_ = std::clamp(integ_, -kMaxFreq, kMaxFreq);
        double f = std::clamp(integ_ + kp * r, -kMaxFreq, kMaxFreq);
        rebase(t2);
        freqAdj_ = f;
        errEma_ = 0.8 * errEma_ + 0.2 * std::fabs(double(e));
        if (errEma_ < 150000.0) { if (++goodCount_ >= 8) state_ = 2; }
        else if (std::llabs(e) > 500000) { state_ = 1; goodCount_ = 0; }
    }

    mutable std::mutex m_;
    int64_t baseLocal_ = 0, baseNet_ = 0;
    double  freqAdj_ = 0, integ_ = 0, errEma_ = 0;
    bool    leader_ = false;
    int     state_ = 0;   // 0 free, 1 locking, 2 locked
    int     goodCount_ = 0;
    int64_t lastErr_ = 0, pathDelay_ = -1, lastProcLocal_ = 0;
    bool haveSync_ = false;
    uint32_t steps_ = 0;
    std::atomic<uint32_t> stepsAtomic_{0};
    std::vector<Samp> syncWin_;
    std::vector<int64_t> msWin_, smWin_;
    static void pushWin(std::vector<int64_t>& w, int64_t v) { w.push_back(v); if (w.size() > 32) w.erase(w.begin()); }
};

} // namespace slink
