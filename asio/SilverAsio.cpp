// SilverLink ASIO driver - virtual sound card that exchanges audio with SilverLink Engine
// Copyright (C) 2026 Jorge Prata Alves Junior - GPLv3
// Uses the Steinberg ASIO SDK (GPLv3 option). ASIO is a trademark of Steinberg Media Technologies GmbH.
#include "../common/Shm.h"
#include "../common/TimeUtil.h"

#include <windows.h>
#include <objbase.h>
#include <avrt.h>
#include <shellapi.h>
#include <cmath>
#include <cstdio>
#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "asiosys.h"
#include "asio.h"
#include "iasiodrv.h"

using namespace slink;

// {904D1B11-0B72-431C-ADE8-7C1BA2034072}
static const CLSID CLSID_SilverAsio = {0x904d1b11, 0x0b72, 0x431c, {0xad, 0xe8, 0x7c, 0x1b, 0xa2, 0x03, 0x40, 0x72}};
static const wchar_t* kClsidStr   = L"{904D1B11-0B72-431C-ADE8-7C1BA2034072}";
static const wchar_t* kDriverKey  = L"SOFTWARE\\ASIO\\SilverLink";
static const char*    kDriverName = "SilverLink";

static HINSTANCE g_hinst = nullptr;
static std::atomic<long> g_objects{0}, g_locks{0};

static std::wstring moduleDir() {
    wchar_t p[MAX_PATH]; GetModuleFileNameW(g_hinst, p, MAX_PATH);
    std::wstring s(p); return s.substr(0, s.find_last_of(L"\\/"));
}
static void launch(const std::wstring& exe) {
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exe + L"\"";
    if (CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }
}

// ===========================================================================
class SilverAsio : public IASIO {
public:
    SilverAsio() { g_objects++; }
    virtual ~SilverAsio() { stop(); disposeBuffers(); closeShm(); g_objects--; }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, CLSID_SilverAsio)) {
            *ppv = static_cast<IASIO*>(this); AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ULONG(++ref_); }
    ULONG STDMETHODCALLTYPE Release() override { long r = --ref_; if (r == 0) delete this; return ULONG(r); }

    // IASIO
    ASIOBool init(void*) override {
        if (openShm()) return ASIOTrue;
        launch(moduleDir() + L"\\SilverLink.exe");
        for (int i = 0; i < 40; ++i) { Sleep(100); if (openShm()) return ASIOTrue; }
        setError("SilverLink Engine nao esta em execucao. Abra o SilverLink.exe.");
        return ASIOFalse;
    }
    void getDriverName(char* name) override { strcpy(name, kDriverName); }
    long getDriverVersion() override { return 1; }
    void getErrorMessage(char* s) override { strncpy(s, err_.c_str(), 123); s[123] = 0; }

    ASIOError start() override {
        if (!shm_) return ASE_NotPresent;
        if (!callbacks_ || bufSize_ == 0) return ASE_InvalidMode;
        if (running_) return ASE_OK;
        ShmHeader& h = shm_->hdr;
        // Engine alive?
        const uint32_t hb = h.engineHeartbeat.load();
        Sleep(20);
        if (h.engineHeartbeat.load() == hb) { setError("SilverLink Engine parado."); return ASE_HWMalfunction; }
        if (h.asioRunning.load() && h.asioPid.load() != GetCurrentProcessId()) {
            HANDLE ph = OpenProcess(SYNCHRONIZE, FALSE, h.asioPid.load());
            bool alive = ph && WaitForSingleObject(ph, 0) == WAIT_TIMEOUT;
            if (ph) CloseHandle(ph);
            if (alive) { setError("SilverLink ja esta em uso por outro programa."); return ASE_HWMalfunction; }
        }
        for (int c = 0; c < kMaxChannels; ++c) memset(shm_->rx[c], 0, sizeof(shm_->rx[c]));
        configRev_ = h.engineConfigRev.load();
        resetSent_ = false;
        h.asioBufferSize = uint32_t(bufSize_);
        h.asioPid = GetCurrentProcessId();
        h.asioRunning = 1;
        running_ = true;
        thread_ = std::thread(&SilverAsio::audioThread, this);
        return ASE_OK;
    }
    ASIOError stop() override {
        if (!running_) return ASE_OK;
        running_ = false;
        if (blockEvt_) SetEvent(blockEvt_);
        if (thread_.joinable()) thread_.join();
        if (shm_) { shm_->hdr.asioRunning = 0; shm_->hdr.asioPid = 0; }
        return ASE_OK;
    }
    ASIOError getChannels(long* in, long* out) override {
        if (!shm_) return ASE_NotPresent;
        *in = long(shm_->hdr.rxChannels.load()); *out = long(shm_->hdr.txChannels.load());
        return ASE_OK;
    }
    ASIOError getLatencies(long* in, long* out) override {
        if (!shm_) return ASE_NotPresent;
        *in = bufSize_ ? bufSize_ : long(shm_->hdr.preferredBuffer.load());
        *out = *in;
        return ASE_OK;
    }
    ASIOError getBufferSize(long* minS, long* maxS, long* pref, long* gran) override {
        if (!shm_) return ASE_NotPresent;
        *minS = 32; *maxS = 2048;
        long p = long(shm_->hdr.preferredBuffer.load());
        *pref = (p >= 32 && p <= 2048) ? p : 256;
        *gran = -1; // powers of two
        return ASE_OK;
    }
    ASIOError canSampleRate(ASIOSampleRate r) override {
        if (!shm_) return ASE_NotPresent;
        for (auto x : kRates) if (std::abs(double(x) - r) < 0.5) return ASE_OK;
        return ASE_NoClock;
    }
    ASIOError getSampleRate(ASIOSampleRate* r) override {
        if (!shm_) return ASE_NotPresent;
        *r = double(shm_->hdr.sampleRate.load());
        return ASE_OK;
    }
    ASIOError setSampleRate(ASIOSampleRate r) override {
        if (!shm_) return ASE_NotPresent;
        const uint32_t want = uint32_t(r + 0.5);
        if (want == shm_->hdr.sampleRate.load()) return ASE_OK;
        if (canSampleRate(r) != ASE_OK) return ASE_NoClock;
        if (running_) return ASE_InvalidMode;
        shm_->hdr.rateRequest = want;
        for (int i = 0; i < 40 && shm_->hdr.sampleRate.load() != want; ++i) Sleep(25);
        if (shm_->hdr.sampleRate.load() != want) return ASE_NoClock;
        configRev_ = shm_->hdr.engineConfigRev.load();
        return ASE_OK;
    }
    ASIOError getClockSources(ASIOClockSource* c, long* n) override {
        if (!c || !n) return ASE_InvalidParameter;
        c[0].index = 0; c[0].associatedChannel = -1; c[0].associatedGroup = -1; c[0].isCurrentSource = ASIOTrue;
        strcpy(c[0].name, "Rede SilverLink");
        *n = 1;
        return ASE_OK;
    }
    ASIOError setClockSource(long ref) override { return ref == 0 ? ASE_OK : ASE_InvalidParameter; }
    ASIOError getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) override {
        if (!running_) return ASE_SPNotAdvancing;
        const uint64_t pos = uint64_t(lastS_.load() - startS_);
        const uint64_t ts  = uint64_t(lastTimeNs_.load());
        sPos->hi = (unsigned long)(pos >> 32); sPos->lo = (unsigned long)(pos & 0xFFFFFFFFu);
        tStamp->hi = (unsigned long)(ts >> 32); tStamp->lo = (unsigned long)(ts & 0xFFFFFFFFu);
        return ASE_OK;
    }
    ASIOError getChannelInfo(ASIOChannelInfo* info) override {
        if (!shm_) return ASE_NotPresent;
        ShmHeader& h = shm_->hdr;
        const long n = info->isInput ? long(h.rxChannels.load()) : long(h.txChannels.load());
        if (info->channel < 0 || info->channel >= n) return ASE_InvalidParameter;
        info->type = ASIOSTFloat32LSB;
        info->channelGroup = 0;
        info->isActive = ASIOFalse;
        for (auto& b : bufs_) if (b.isInput == (info->isInput != 0) && b.ch == info->channel) info->isActive = ASIOTrue;
        const char* nm = info->isInput ? h.rxNames[info->channel] : h.txNames[info->channel];
        strncpy(info->name, nm, 31); info->name[31] = 0;
        if (!info->name[0]) snprintf(info->name, 32, "%s %ld", info->isInput ? "RX" : "TX", info->channel + 1);
        return ASE_OK;
    }
    ASIOError createBuffers(ASIOBufferInfo* infos, long num, long size, ASIOCallbacks* cb) override {
        if (!shm_) return ASE_NotPresent;
        if (running_) return ASE_InvalidMode;
        if (!cb || num <= 0 || size < 16 || size > 4096) return ASE_InvalidMode;
        disposeBuffers();
        ShmHeader& h = shm_->hdr;
        const long nin = long(h.rxChannels.load()), nout = long(h.txChannels.load());
        for (long i = 0; i < num; ++i) {
            const bool in = infos[i].isInput != 0;
            if (infos[i].channelNum < 0 || infos[i].channelNum >= (in ? nin : nout)) { disposeBuffers(); return ASE_InvalidParameter; }
            Buf b; b.isInput = in; b.ch = int(infos[i].channelNum);
            b.data[0].assign(size_t(size), 0.f); b.data[1].assign(size_t(size), 0.f);
            bufs_.push_back(std::move(b));
        }
        for (long i = 0; i < num; ++i) {
            infos[i].buffers[0] = bufs_[size_t(i)].data[0].data();
            infos[i].buffers[1] = bufs_[size_t(i)].data[1].data();
        }
        bufSize_ = size;
        callbacks_ = cb;
        hostResetSupported_ = cb->asioMessage && cb->asioMessage(kAsioSelectorSupported, kAsioResetRequest, nullptr, nullptr) == 1;
        return ASE_OK;
    }
    ASIOError disposeBuffers() override {
        stop();
        bufs_.clear(); bufSize_ = 0; callbacks_ = nullptr;
        return ASE_OK;
    }
    ASIOError controlPanel() override {
        std::wstring p = moduleDir() + L"\\SilverLink_Controller.exe";
        ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return ASE_OK;
    }
    ASIOError future(long selector, void*) override {
        switch (selector) {
        case kAsioCanReportOverload: return ASE_SUCCESS;
        default: return ASE_InvalidParameter;
        }
    }
    ASIOError outputReady() override { return ASE_NotPresent; }

private:
    struct Buf { bool isInput = false; int ch = 0; std::vector<float> data[2]; };

    void setError(const char* e) { err_ = e; }

    bool openShm() {
        if (shm_) return true;
        HANDLE m = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, kShmName);
        if (!m) return false;
        auto* p = static_cast<ShmLayout*>(MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ShmLayout)));
        if (!p || p->hdr.magic != kShmMagic || p->hdr.version != kShmVersion) {
            if (p) UnmapViewOfFile(p);
            CloseHandle(m);
            setError("Versao do SilverLink Engine incompativel com o driver.");
            return false;
        }
        HANDLE be = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, kBlockEvtName);
        HANDLE te = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, kTxEvtName);
        if (!be || !te) {
            if (be) CloseHandle(be);
            if (te) CloseHandle(te);
            UnmapViewOfFile(p); CloseHandle(m); return false;
        }
        map_ = m; shm_ = p; blockEvt_ = be; txEvt_ = te;
        return true;
    }
    void closeShm() {
        if (shm_) { UnmapViewOfFile(shm_); shm_ = nullptr; }
        if (map_) { CloseHandle(map_); map_ = nullptr; }
        if (blockEvt_) { CloseHandle(blockEvt_); blockEvt_ = nullptr; }
        if (txEvt_) { CloseHandle(txEvt_); txEvt_ = nullptr; }
    }

    void audioThread() {
        DWORD idx = 0;
        HANDLE mm = AvSetMmThreadCharacteristicsW(L"Pro Audio", &idx);
        if (mm) AvSetMmThreadPriority(mm, AVRT_PRIORITY_CRITICAL);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

        ShmHeader& h = shm_->hdr;
        const int64_t B = bufSize_;
        int64_t lastS = INT64_MIN;
        long half = 0;
        bool first = true;

        while (running_) {
            DWORD w = WaitForSingleObject(blockEvt_, 100);
            if (!running_) break;
            if (w != WAIT_OBJECT_0) continue;

            if (h.engineConfigRev.load() != configRev_ && !resetSent_) {
                resetSent_ = true;
                if (hostResetSupported_) callbacks_->asioMessage(kAsioResetRequest, 0, nullptr, nullptr);
            }

            h.blockCounter.load(std::memory_order_acquire);
            const int64_t S = h.blockStart.load(std::memory_order_relaxed);
            if (S == lastS) continue;
            const int nRx = int(h.rxChannels.load()), nTx = int(h.txChannels.load());

            if (lastS == INT64_MIN || S < lastS || S - lastS > 8 * B) {
                // discontinuity: start over
                for (int c = 0; c < nRx; ++c) memset(shm_->rx[c], 0, sizeof(shm_->rx[c]));
                if (first) { startS_ = S; first = false; }
            } else if (S - lastS > B) {
                // missed block(s): clear what we skipped on both sides
                for (int64_t i = lastS; i < S - B; ++i)
                    for (int c = 0; c < nRx; ++c) shm_->rx[c][i & kRingMask] = 0.f;
                for (int64_t i = lastS + 2 * B; i < S + B; ++i)
                    for (int c = 0; c < nTx; ++c) shm_->tx[c][i & kRingMask] = 0.f;
            }

            // ---- inputs: presentation window [S-B, S)
            for (auto& b : bufs_) {
                if (!b.isInput || b.ch >= nRx) continue;
                float* dst = b.data[half].data();
                float* ring = shm_->rx[b.ch];
                for (int64_t i = 0; i < B; ++i) {
                    const int64_t k = (S - B + i) & kRingMask;
                    dst[i] = ring[k];
                    ring[k] = 0.f;
                }
            }
            h.rxReadPos.store(S, std::memory_order_release);
            lastS_.store(S); lastTimeNs_.store(h.blockTimeNs.load());

            callbacks_->bufferSwitch(half, ASIOTrue);

            // ---- outputs: stamped [S+B, S+2B)
            for (int c = 0; c < nTx; ++c) {
                const float* src = nullptr;
                for (auto& b : bufs_) if (!b.isInput && b.ch == c) { src = b.data[half].data(); break; }
                float* ring = shm_->tx[c];
                for (int64_t i = 0; i < B; ++i) ring[(S + B + i) & kRingMask] = src ? src[i] : 0.f;
            }
            h.txWriteEnd.store(S + 2 * B, std::memory_order_release);
            SetEvent(txEvt_);

            half ^= 1;
            lastS = S;
        }
        if (mm) AvRevertMmThreadCharacteristics(mm);
    }

    std::atomic<long> ref_{1};
    std::string err_ = "";
    HANDLE map_ = nullptr, blockEvt_ = nullptr, txEvt_ = nullptr;
    ShmLayout* shm_ = nullptr;
    std::vector<Buf> bufs_;
    long bufSize_ = 0;
    ASIOCallbacks* callbacks_ = nullptr;
    bool hostResetSupported_ = false;
    std::atomic<bool> running_{false};
    std::thread thread_;
    uint32_t configRev_ = 0;
    std::atomic<bool> resetSent_{false};
    int64_t startS_ = 0;
    std::atomic<int64_t> lastS_{0}, lastTimeNs_{0};
};

// ===========================================================================
// COM plumbing
// ===========================================================================
class Factory : public IClassFactory {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory)) { *ppv = this; AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { g_locks++; return 2; }
    ULONG STDMETHODCALLTYPE Release() override { g_locks--; return 1; }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (outer) return CLASS_E_NOAGGREGATION;
        auto* d = new (std::nothrow) SilverAsio();
        if (!d) return E_OUTOFMEMORY;
        HRESULT hr = d->QueryInterface(riid, ppv);
        d->Release();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override { if (lock) g_locks++; else g_locks--; return S_OK; }
};
static Factory g_factory;

extern "C" {

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { g_hinst = h; DisableThreadLibraryCalls(h); }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* ppv) {
    if (!IsEqualCLSID(clsid, CLSID_SilverAsio)) return CLASS_E_CLASSNOTAVAILABLE;
    return g_factory.QueryInterface(riid, ppv);
}

STDAPI DllCanUnloadNow() { return (g_objects == 0 && g_locks == 0) ? S_OK : S_FALSE; }

static bool setStr(HKEY root, const std::wstring& key, const wchar_t* name, const std::wstring& val) {
    HKEY k;
    if (RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return false;
    LONG r = RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(val.c_str()), DWORD((val.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

STDAPI DllRegisterServer() {
    wchar_t path[MAX_PATH]; GetModuleFileNameW(g_hinst, path, MAX_PATH);
    const std::wstring clsidKey = std::wstring(L"CLSID\\") + kClsidStr;
    bool ok = setStr(HKEY_CLASSES_ROOT, clsidKey, nullptr, L"SilverLink ASIO Driver") &&
              setStr(HKEY_CLASSES_ROOT, clsidKey + L"\\InprocServer32", nullptr, path) &&
              setStr(HKEY_CLASSES_ROOT, clsidKey + L"\\InprocServer32", L"ThreadingModel", L"Apartment") &&
              setStr(HKEY_LOCAL_MACHINE, kDriverKey, L"CLSID", kClsidStr) &&
              setStr(HKEY_LOCAL_MACHINE, kDriverKey, L"Description", L"SilverLink");
    return ok ? S_OK : E_FAIL;
}

STDAPI DllUnregisterServer() {
    const std::wstring clsidKey = std::wstring(L"CLSID\\") + kClsidStr;
    RegDeleteTreeW(HKEY_CLASSES_ROOT, clsidKey.c_str());
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, kDriverKey);
    return S_OK;
}

} // extern "C"
