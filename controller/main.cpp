// SilverLink Controller - window, Direct3D 11 and Dear ImGui setup
// Copyright (C) 2026 Jorge Prata Alves Junior - GPLv3
#include "Discovery.h"
#include "Ui.h"

#include <d3d11.h>
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static ID3D11Device* g_dev = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;
static IDXGISwapChain* g_swap = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static UINT g_resizeW = 0, g_resizeH = 0;
static bool g_occluded = false;

static void createRT() {
    ID3D11Texture2D* back = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) { g_dev->CreateRenderTargetView(back, nullptr, &g_rtv); back->Release(); }
}
static void cleanupRT() { if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; } }

static bool createDevice(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60; sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                               &sd, &g_swap, &g_dev, &got, &g_ctx);
    if (hr == DXGI_ERROR_UNSUPPORTED)
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                           &sd, &g_swap, &g_dev, &got, &g_ctx);
    if (hr != S_OK) return false;
    createRT();
    return true;
}
static void cleanupDevice() {
    cleanupRT();
    if (g_swap) { g_swap->Release(); g_swap = nullptr; }
    if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
    if (g_dev) { g_dev->Release(); g_dev = nullptr; }
}

static LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;
    switch (msg) {
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) return 0;
        g_resizeW = LOWORD(lp); g_resizeH = HIWORD(lp);
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
        mm->ptMinTrackSize.x = 800; mm->ptMinTrackSize.y = 500;
        return 0; }
    case WM_SYSCOMMAND:
        if ((wp & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\SilverLink_Controller_Single");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(L"SilverLinkControllerWnd", nullptr);
        if (other) { ShowWindow(other, SW_RESTORE); SetForegroundWindow(other); }
        return 0;
    }
    ImGui_ImplWin32_EnableDpiAwareness();
    const float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));

    WNDCLASSEXW wc{ sizeof(wc), CS_CLASSDC, WndProc, 0, 0, hInst, LoadIconW(hInst, MAKEINTRESOURCEW(1)), LoadCursorW(nullptr, IDC_ARROW),
                    nullptr, nullptr, L"SilverLinkControllerWnd", nullptr };
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"SilverLink Controller", WS_OVERLAPPEDWINDOW, 100, 80,
                              int(1280 * scale), int(800 * scale), nullptr, nullptr, hInst, nullptr);
    if (!createDevice(hwnd)) {
        cleanupDevice();
        MessageBoxW(nullptr, L"Não foi possível iniciar o Direct3D 11.", L"SilverLink Controller", MB_ICONERROR);
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);
    slink::ui::setupStyle(scale);

    slink::Discovery disc;
    if (!disc.start())
        MessageBoxW(hwnd, L"Não foi possível abrir a porta UDP 7700 para descobrir dispositivos.", L"SilverLink Controller", MB_ICONWARNING);

    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;
        if (g_occluded && g_swap->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) { Sleep(20); continue; }
        g_occluded = false;
        if (g_resizeW && g_resizeH) {
            cleanupRT();
            g_swap->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
            g_resizeW = g_resizeH = 0;
            createRT();
        }
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        slink::ui::drawFrame(disc);
        ImGui::Render();
        const float clear[4] = { 0.85f, 0.86f, 0.88f, 1.f };
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        HRESULT hr = g_swap->Present(1, 0);
        g_occluded = (hr == DXGI_STATUS_OCCLUDED);
    }
    disc.stop();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    cleanupDevice();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, hInst);
    if (single) CloseHandle(single);
    return 0;
}
