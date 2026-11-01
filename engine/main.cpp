// SilverLink Engine - tray application
// Copyright (C) 2026 Jorge Prata Alves Junior - GPLv3
#include "Engine.h"
#include <shellapi.h>
#include <string>

using namespace slink;

namespace {
constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT ID_OPEN = 1001, ID_AUTOSTART = 1002, ID_QUIT = 1003, ID_IFACE_BASE = 1100;
constexpr UINT_PTR TIMER_TIP = 1;
constexpr const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t* kRunVal = L"SilverLink Engine";

Engine* g_engine = nullptr;
NOTIFYICONDATAW g_nid{};
std::vector<Adapter> g_adapters;
UINT g_taskbarCreated = 0;

std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}
std::wstring exeDir() {
    wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s(p); return s.substr(0, s.find_last_of(L"\\/"));
}
bool autostartEnabled() {
    HKEY k; if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    bool ok = RegQueryValueExW(k, kRunVal, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(k); return ok;
}
void setAutostart(bool on) {
    HKEY k; if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr, p, MAX_PATH);
        std::wstring v = L"\"" + std::wstring(p) + L"\"";
        RegSetValueExW(k, kRunVal, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()), DWORD((v.size() + 1) * sizeof(wchar_t)));
    } else RegDeleteValueW(k, kRunVal);
    RegCloseKey(k);
}
void openController() {
    std::wstring p = exeDir() + L"\\SilverLink_Controller.exe";
    ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
const wchar_t* clockText(uint8_t s) { return s == CLK_LOCKED ? L"travado" : s == CLK_LOCKING ? L"sincronizando" : L"livre"; }

void updateTip() {
    EngineStatus s = g_engine->status();
    std::wstring tip = L"SilverLink - " + widen(s.name) + L"\n" + widen(s.ip) + L"  " + std::to_wstring(s.rate) + L" Hz\n" +
                       (s.isLeader ? L"Relógio: LÍDER" : L"Relógio: " + std::wstring(clockText(s.clockState))) +
                       (s.asioActive ? L"  |  ASIO ativo" : L"");
    wcsncpy(g_nid.szTip, tip.c_str(), 127); g_nid.szTip[127] = 0;
    g_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

void showMenu(HWND hwnd) {
    EngineStatus s = g_engine->status();
    HMENU m = CreatePopupMenu();
    std::wstring head = L"SilverLink  " + widen(s.name) + L"  (" + widen(s.ip) + L")";
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, head.c_str());
    std::wstring clk = s.isLeader ? L"Relógio: líder da rede" : (L"Relógio: " + std::wstring(clockText(s.clockState)) +
                       (s.leaderName.empty() ? L"" : L" (líder: " + widen(s.leaderName) + L")"));
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, clk.c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_OPEN, L"Abrir SilverLink Controller");
    HMENU sub = CreatePopupMenu();
    g_adapters = listAdapters();
    for (size_t i = 0; i < g_adapters.size() && i < 50; ++i) {
        auto& a = g_adapters[i];
        std::wstring t = widen(a.friendlyName) + L"  -  " + widen(ipToString(a.ip)) + (a.wired ? L"" : L"  (não cabeada)");
        AppendMenuW(sub, MF_STRING | (a.ip == g_engine->interfaceIp() ? MF_CHECKED : 0), ID_IFACE_BASE + UINT(i), t.c_str());
    }
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(sub), L"Interface de rede");
    AppendMenuW(m, MF_STRING | (autostartEnabled() ? MF_CHECKED : 0), ID_AUTOSTART, L"Iniciar com o Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_QUIT, L"Sair");
    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(m);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_taskbarCreated && msg == g_taskbarCreated) { // Explorer restarted
        g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        Shell_NotifyIconW(NIM_ADD, &g_nid);
        return 0;
    }
    switch (msg) {
    case WM_TRAY:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) showMenu(hwnd);
        else if (LOWORD(lp) == WM_LBUTTONDBLCLK) openController();
        return 0;
    case WM_TIMER:
        if (wp == TIMER_TIP) updateTip();
        return 0;
    case WM_COMMAND: {
        UINT id = LOWORD(wp);
        if (id == ID_OPEN) openController();
        else if (id == ID_AUTOSTART) setAutostart(!autostartEnabled());
        else if (id == ID_QUIT) DestroyWindow(hwnd);
        else if (id >= ID_IFACE_BASE && id < ID_IFACE_BASE + g_adapters.size()) g_engine->setInterface(g_adapters[id - ID_IFACE_BASE].ip);
        return 0; }
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
} // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    HANDLE single = CreateMutexW(nullptr, TRUE, kEngineMutex);
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    Engine engine;
    g_engine = &engine;
    std::string err;
    if (!engine.start(err)) {
        MessageBoxW(nullptr, (L"Não foi possível iniciar o SilverLink Engine:\n" + widen(err)).c_str(), L"SilverLink", MB_ICONERROR);
        return 1;
    }

    WNDCLASSW wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = hInst; wc.lpszClassName = L"SilverLinkEngineWnd";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"SilverLink Engine", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, hInst, nullptr);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd; g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    if (!g_nid.hIcon) g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy(g_nid.szTip, L"SilverLink");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    SetTimer(hwnd, TIMER_TIP, 2000, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    engine.stop();
    if (single) CloseHandle(single);
    return 0;
}
