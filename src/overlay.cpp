#include "overlay.h"
#include "features.h"
#include "render.h"
#include "ui.h"
#include "offsets.h"
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")

namespace overlay {

namespace {

HWND                    g_hwnd = nullptr;
ID3D11Device*           g_device = nullptr;
ID3D11DeviceContext*    g_context = nullptr;
IDXGISwapChain*         g_swap = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;

bool g_menu = true;
bool g_running = true;
bool g_mouseDown = false;
bool g_mouseClicked = false;
float g_mx = 0.0f, g_my = 0.0f;

// Menu toggle. F1 while the game has focus.
constexpr int kMenuKey = VK_F1;

void logFail(const char* what);

HHOOK g_kbHook = nullptr;
bool  g_keyHeld = false;

void resizeSwapchain(UINT w, UINT h) {
    if (!g_swap || w == 0 || h == 0) return;
    rnd::releaseTarget(); // the D2D target holds a backbuffer reference
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    g_context->OMSetRenderTargets(0, nullptr, nullptr);
    g_swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);

    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(g_swap->GetBuffer(0, IID_PPV_ARGS(&back))) && back) {
        g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

void setClickThrough(bool on) {
    LONG_PTR ex = GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE);
    if (on) ex |= WS_EX_TRANSPARENT;
    else    ex &= ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT);
    SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE, ex); 
}

void toggleMenu() { 
    g_menu = !g_menu;
    setClickThrough(!g_menu);
    if (g_menu) {
        SetForegroundWindow(g_hwnd);
    } else {
        // Hand keyboard focus back to the game, otherwise the overlay keeps it
        // as the foreground window and Roblox stops receiving input.
        HWND game = FindWindowW(off::WindowClass, nullptr);
        if (!game) game = FindWindowW(nullptr, L"Roblox");
        if (game) SetForegroundWindow(game);
    }
}

// Low-level hook: fires while Roblox owns the foreground, and unlike
// RegisterHotKey it does not steal F1 from everything else on the desktop.
LRESULT CALLBACK kbProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        auto* info = reinterpret_cast<KBDLLHOOKSTRUCT*>(lp);
        if (info->vkCode == kMenuKey) {
            if (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN) {
                if (!g_keyHeld) { g_keyHeld = true; toggleMenu(); }
            } else if (wp == WM_KEYUP || wp == WM_SYSKEYUP) {
                g_keyHeld = false;
            }
        }
    }
    return CallNextHookEx(g_kbHook, code, wp, lp);
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_HOTKEY:
            return 0;
        case WM_MOUSEMOVE:
            g_mx = static_cast<float>(GET_X_LPARAM(lp));
            g_my = static_cast<float>(GET_Y_LPARAM(lp));
            return 0;
        case WM_LBUTTONDOWN:
            g_mouseDown = true;
            g_mouseClicked = true;
            SetCapture(hwnd);
            return 0;
        case WM_LBUTTONUP:
            g_mouseDown = false;
            ReleaseCapture();
            return 0;
        case WM_SIZE:
            resizeSwapchain(LOWORD(lp), HIWORD(lp));
            return 0;
        case WM_SYSCOMMAND:
            if ((wp & 0xFFF0) == SC_KEYMENU) return 0;
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool initWindow() {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
    wc.lpszClassName = L"roblox_ext_overlay";
    if (!RegisterClassExW(&wc)) return false;

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);

    g_hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        wc.lpszClassName, L"ELYYYKKK",
        WS_POPUP, 0, 0, sw, sh,
        nullptr, nullptr, inst, nullptr);
    if (!g_hwnd) return false;

    SetLayeredWindowAttributes(g_hwnd, 0, 255, LWA_ALPHA);
    MARGINS margins{-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(g_hwnd, &margins);

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);
    return true;
}

bool initD3D() {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 0;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = g_hwnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        levels, 2, D3D11_SDK_VERSION, &sd,
        &g_swap, &g_device, &got, &g_context);
    if (FAILED(hr)) {
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels, 2, D3D11_SDK_VERSION, &sd,
            &g_swap, &g_device, &got, &g_context);
    }
    if (FAILED(hr)) return false;

    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(g_swap->GetBuffer(0, IID_PPV_ARGS(&back))) && back) {
        g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
    return g_rtv != nullptr;
}

void shutdown() {
    feat::shutdown();   // restore FOV / lighting before the process goes away
    if (g_kbHook) { UnhookWindowsHookEx(g_kbHook); g_kbHook = nullptr; }
    rnd::shutdown();
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    if (g_swap) { g_swap->Release(); g_swap = nullptr; }
    if (g_context) { g_context->Release(); g_context = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
    if (g_hwnd) { DestroyWindow(g_hwnd); g_hwnd = nullptr; }
}

void logFail(const char* what) {
    FILE* f = nullptr;
    fopen_s(&f, "overlay_log.txt", "a");
    if (f) { fprintf(f, "overlay: %s\n", what); fclose(f); }
}

} // namespace

bool menuOpen() { return g_menu; }

int run() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    if (!initWindow()) { logFail("window creation failed"); return 1; }
    if (!initD3D())    { logFail("D3D11 init failed"); shutdown(); return 1; }
    if (!rnd::init(g_device, g_context, g_swap)) {        logFail("Direct2D init failed");
        shutdown();
        return 1;
    }

    setClickThrough(!g_menu);
    SetForegroundWindow(g_hwnd); // menu is open at launch; make sure it is usable

    g_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, kbProc, GetModuleHandleW(nullptr), 0);
    if (!g_kbHook) logFail("keyboard hook failed; F1 only works with overlay focused");

    MSG msg{};
    while (g_running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) { g_running = false; break; }
        }
        if (!g_running) break;

        RECT rc{};
        GetClientRect(g_hwnd, &rc);
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;
        if (w <= 0 || h <= 0) { Sleep(10); continue; }

        ui::beginFrame(g_mx, g_my, g_mouseDown, g_mouseClicked);
        g_mouseClicked = false;

        rnd::begin(w, h);
        feat::update(w, h);
        feat::render(w, h);
        rnd::end();

        // Sync to vblank. Present(0,0) on a layered window tears against the
        // game underneath: thin ESP lines get composited from a half-updated
        // surface and visibly shimmer while the camera moves.
        HRESULT hr = g_swap->Present(1, 0);
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) break;
    }

    shutdown();
    return 0;
}

} // namespace overlay
