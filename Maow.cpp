#include "common.h"
#include "audio.h"
#include "window.h"
#include "resource.h"
#include <atomic>
#include <chrono>
#include <new>
#include <tlhelp32.h>
#include <shobjidl.h>
#include <d2d1.h>
#include <dxgi.h>
#include <d3d11.h>
#include <wincodec.h>
#include <d2d1_1.h>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "windowscodecs.lib")

HINSTANCE g_instance = nullptr;
ULONG_PTR g_gdiplusToken = 0;
HWND g_controller = nullptr;
HWND g_tunnel = nullptr;
HWND g_d3dWindow = nullptr;
std::array<WindowState, App::Window::MaxCount> g_windows{};
BackgroundAudio g_audio{};
std::mt19937 g_rng{std::random_device{}()};
int g_windowCount = App::Window::BaseCount;
double g_mouseBoost = 1.0;
POINT g_lastMouse{0, 0};
volatile bool g_shouldExit = false;
volatile bool g_tunnelThreadRunning = false;
std::wstring g_keyBuffer;

ID3D11Device* g_d3dDevice = nullptr;
ID3D11DeviceContext* g_d3dContext = nullptr;
IDXGISwapChain* g_swapChain = nullptr;
ID3D11RenderTargetView* g_renderTargetView = nullptr;
ID2D1Factory1* g_d2dFactory = nullptr;
ID2D1RenderTarget* g_d2dRenderTarget = nullptr;

double RandomVelocity();
bool CreateWnd(WindowState& window, int windowIndex, HINSTANCE instance);

void RaiseGifWindowsAboveAll() {
    if (g_tunnel) {
        SetWindowPos(
            g_tunnel,
            HWND_TOPMOST,
            0,
            0,
            0,
            0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER
        );
    }

    for (int i = 0; i < g_windowCount; ++i) {
        auto& window = g_windows[i];
        if (!window.hwnd) {
            continue;
        }

        SetWindowPos(
            window.hwnd,
            HWND_TOPMOST,
            0,
            0,
            0,
            0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER
        );
    }
}

const std::array<int, 17> GifIds{
    IDR_CAT1_GIF, IDR_CAT2_GIF, IDR_CAT3_GIF, IDR_CAT4_GIF, IDR_CAT5_GIF, IDR_CAT6_GIF,
    IDR_CAT7_GIF, IDR_CAT8_GIF, IDR_CAT9_GIF, IDR_CAT10_GIF, IDR_CAT11_GIF, IDR_CAT12_GIF,
    IDR_CAT13_GIF, IDR_CAT15_GIF, IDR_CAT16_GIF, IDR_CAT17_GIF, IDR_CAT18_GIF
};

namespace {

std::atomic<bool> g_popupLimitUnlocked{false};
std::chrono::steady_clock::time_point g_rampStart{};
int g_targetWindowCount = App::Window::MaxCount;

LPCWSTR RandomPopupMessage() {
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<int> pick(0, 99);
    const int roll = pick(rng);

    if (roll < 50) {
        return L"Maow";
    }
    if (roll < 70) {
        return L"MEOWWWWWW";
    }
    return L"Nya~";
}

std::wstring GetTempPathString() {
    std::wstring path(MAX_PATH, L'\0');
    DWORD len = GetTempPathW(static_cast<DWORD>(path.size()), &path[0]);
    while (len > 0 && len >= path.size()) {
        path.resize(len + 1, L'\0');
        len = GetTempPathW(static_cast<DWORD>(path.size()), &path[0]);
    }
    if (len == 0) {
        return L"";
    }
    path.resize(len);
    return path;
}

std::wstring ExtractWallpaperResourceToTemp() {
    HRSRC resource = FindResource(g_instance, MAKEINTRESOURCE(IDR_CAT_GIRL_JPG), RT_RCDATA);
    if (!resource) {
        return L"";
    }

    const DWORD size = SizeofResource(g_instance, resource);
    HGLOBAL loaded = LoadResource(g_instance, resource);
    const void* source = LockResource(loaded);
    if (size == 0 || !loaded || !source) {
        return L"";
    }

    const std::wstring temp = GetTempPathString();
    if (temp.empty()) {
        return L"";
    }

    const std::wstring dir = temp + L"Maow";
    CreateDirectoryW(dir.c_str(), nullptr);

    const std::wstring output = dir + L"\\cat_girl.jpg";
    HANDLE file = CreateFileW(
        output.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (file == INVALID_HANDLE_VALUE) {
        return L"";
    }

    DWORD written = 0;
    const BOOL ok = WriteFile(file, source, size, &written, nullptr);
    CloseHandle(file);

    if (!ok || written != size) {
        DeleteFileW(output.c_str());
        return L"";
    }

    return output;
}

void KillProcessByName(const wchar_t* processName) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot, &entry)) {
        CloseHandle(snapshot);
        return;
    }

    do {
        if (_wcsicmp(entry.szExeFile, processName) != 0) {
            continue;
        }

        HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, entry.th32ProcessID);
        if (process) {
            TerminateProcess(process, 0);
            CloseHandle(process);
        }
    } while (Process32NextW(snapshot, &entry));

    CloseHandle(snapshot);
}

void KillWallpaperProcesses() {
    KillProcessByName(L"wallpaper64.exe");
    KillProcessByName(L"wallpaper32.exe");
}

void ApplyWallpaperToAllMonitors(const std::wstring& imagePath) {
    if (imagePath.empty()) {
        return;
    }

    IDesktopWallpaper* desktopWallpaper = nullptr;
    HRESULT hr = CoCreateInstance(
        CLSID_DesktopWallpaper,
        nullptr,
        CLSCTX_ALL,
        IID_PPV_ARGS(&desktopWallpaper)
    );

    if (SUCCEEDED(hr) && desktopWallpaper) {
        desktopWallpaper->SetPosition(DWPOS_FILL);
        UINT monitorCount = 0;
        if (SUCCEEDED(desktopWallpaper->GetMonitorDevicePathCount(&monitorCount))) {
            for (UINT i = 0; i < monitorCount; ++i) {
                LPWSTR monitorId = nullptr;
                if (SUCCEEDED(desktopWallpaper->GetMonitorDevicePathAt(i, &monitorId)) && monitorId) {
                    desktopWallpaper->SetWallpaper(monitorId, imagePath.c_str());
                    CoTaskMemFree(monitorId);
                }
            }
        }
        desktopWallpaper->Release();
        return;
    }

    SystemParametersInfoW(
        SPI_SETDESKWALLPAPER,
        0,
        const_cast<wchar_t*>(imagePath.c_str()),
        SPIF_UPDATEINIFILE | SPIF_SENDCHANGE
    );
}

void ShowDesktopNow() {
    INPUT inputs[4]{};

    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_LWIN;

    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = 'D';

    inputs[2].type = INPUT_KEYBOARD;
    inputs[2].ki.wVk = 'D';
    inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;

    inputs[3].type = INPUT_KEYBOARD;
    inputs[3].ki.wVk = VK_LWIN;
    inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;

    SendInput(static_cast<UINT>(std::size(inputs)), inputs, sizeof(INPUT));
}

bool SpawnNextWindow() {
    if (g_windowCount >= g_targetWindowCount || g_windowCount >= App::Window::MaxCount) {
        return false;
    }

    const int index = g_windowCount;
    auto& window = g_windows[index];
    window.vx = RandomVelocity();
    window.vy = RandomVelocity();

    if (!CreateWnd(window, index, g_instance)) {
        return false;
    }

    g_windowCount = index + 1;
    if (!AddGif(index)) {
        DestroyWindow(window.hwnd);
        window.hwnd = nullptr;
        g_windowCount = index;
        return false;
    }

    ShowWindow(window.hwnd, SW_SHOWNOACTIVATE);

    if (g_windowCount >= g_targetWindowCount) {
        g_popupLimitUnlocked.store(true, std::memory_order_relaxed);
    }
    return true;
}

struct TunnelState {
    HWND hwnd = nullptr;
    HDC effectDc = nullptr;
    HDC frameDc = nullptr;
    HBITMAP effectBmp = nullptr;
    HBITMAP frameBmp = nullptr;
    HGDIOBJ oldEffect = nullptr;
    HGDIOBJ oldFrame = nullptr;
    RECT bounds{};
    int width = 0;
    int height = 0;
    double zoomScale = cfg::TunnelMaxScale;
    bool zoomingOut = true;
    std::chrono::steady_clock::time_point rampStart{};
    bool timerRunning = false;
};

void ReleaseTunnelBuffers(TunnelState& state) {
    if (state.effectDc && state.oldEffect) {
        SelectObject(state.effectDc, state.oldEffect);
    }
    if (state.frameDc && state.oldFrame) {
        SelectObject(state.frameDc, state.oldFrame);
    }
    if (state.effectBmp) {
        DeleteObject(state.effectBmp);
    }
    if (state.frameBmp) {
        DeleteObject(state.frameBmp);
    }
    if (state.effectDc) {
        DeleteDC(state.effectDc);
    }
    if (state.frameDc) {
        DeleteDC(state.frameDc);
    }

    state.effectDc = nullptr;
    state.frameDc = nullptr;
    state.effectBmp = nullptr;
    state.frameBmp = nullptr;
    state.oldEffect = nullptr;
    state.oldFrame = nullptr;
    state.width = 0;
    state.height = 0;
}

bool CreateTunnelBuffers(HWND hwnd, TunnelState& state, const RECT& bounds) {
    state.bounds = bounds;
    state.width = state.bounds.right - state.bounds.left;
    state.height = state.bounds.bottom - state.bounds.top;
    if (state.width <= 0 || state.height <= 0) {
        return false;
    }

    HDC windowDc = GetDC(hwnd);
    if (!windowDc) {
        return false;
    }

    state.effectDc = CreateCompatibleDC(windowDc);
    state.frameDc = CreateCompatibleDC(windowDc);
    
    if (!state.effectDc || !state.frameDc) {
        if (state.effectDc) DeleteDC(state.effectDc);
        if (state.frameDc) DeleteDC(state.frameDc);
        ReleaseDC(hwnd, windowDc);
        return false;
    }

    state.effectBmp = CreateCompatibleBitmap(windowDc, state.width, state.height);
    state.frameBmp = CreateCompatibleBitmap(windowDc, state.width, state.height);
    
    if (!state.effectBmp || !state.frameBmp) {
        if (state.effectBmp) DeleteObject(state.effectBmp);
        if (state.frameBmp) DeleteObject(state.frameBmp);
        DeleteDC(state.effectDc);
        DeleteDC(state.frameDc);
        ReleaseDC(hwnd, windowDc);
        return false;
    }

    state.oldEffect = SelectObject(state.effectDc, state.effectBmp);
    state.oldFrame = SelectObject(state.frameDc, state.frameBmp);

    HDC desktop = GetDC(nullptr);
    if (desktop) {
        BitBlt(
            state.effectDc,
            0, 0,
            state.width, state.height,
            desktop,
            state.bounds.left,
            state.bounds.top,
            SRCCOPY | CAPTUREBLT
        );
        ReleaseDC(nullptr, desktop);
    }
    BitBlt(state.frameDc, 0, 0, state.width, state.height, state.effectDc, 0, 0, SRCCOPY);
    ReleaseDC(hwnd, windowDc);
    return true;
}

bool TunnelGeometry(HWND hwnd, TunnelState& state) {
    const RECT currentBounds = GetBounds();
    if (EqualRect(&currentBounds, &state.bounds)) {
        return true;
    }

    SetWindowPos(
        hwnd,
        nullptr,
        currentBounds.left,
        currentBounds.top,
        currentBounds.right - currentBounds.left,
        currentBounds.bottom - currentBounds.top,
        SWP_NOACTIVATE | SWP_NOZORDER
    );

    ReleaseTunnelBuffers(state);
    return CreateTunnelBuffers(hwnd, state, currentBounds);
}

void DestroyTunnelState(TunnelState*& state) {
    if (!state) {
        return;
    }

    ReleaseTunnelBuffers(*state);

    delete state;
    state = nullptr;
}

bool InitTunnelState(HWND hwnd, TunnelState*& state) {
    state = new (std::nothrow) TunnelState();
    if (!state) {
        return false;
    }

    if (!CreateTunnelBuffers(hwnd, *state, GetBounds())) {
        DestroyTunnelState(state);
        return false;
    }
    return true;
}

void StepTunnel(TunnelState& state);
bool TunnelGeometry(HWND hwnd, TunnelState& state);

DWORD WINAPI TunnelRenderThreadProc(LPVOID param) {
    TunnelState* state = reinterpret_cast<TunnelState*>(param);
    if (!state) {
        return 1;
    }

    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    auto frameStart = std::chrono::steady_clock::now();

    while (!g_shouldExit && state->hwnd) {
        if (!g_tunnelThreadRunning) {
            Sleep(10);
            continue;
        }

        if (!TunnelGeometry(state->hwnd, *state)) {
            break;
        }
        StepTunnel(*state);
        InvalidateRect(state->hwnd, nullptr, FALSE);
        UpdateWindow(state->hwnd);

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - frameStart);
        if (elapsed.count() < cfg::TunnelFrameMs) {
            Sleep(static_cast<DWORD>(cfg::TunnelFrameMs - elapsed.count()));
        }
        frameStart = std::chrono::steady_clock::now();
    }

    return 0;
}

void StepTunnel(TunnelState& state)
{
    if (!state.effectDc || !state.frameDc) {
        return;
    }

    static int frameCounter = 0;

    if ((frameCounter++ % 48) == 0) {
        HDC desktop = GetDC(nullptr);
        if (desktop) {
            BitBlt(
                state.effectDc,
                0, 0,
                state.width, state.height,
                desktop,
                state.bounds.left,
                state.bounds.top,
                SRCCOPY | CAPTUREBLT
            );
            ReleaseDC(nullptr, desktop);
        }
    }

    BitBlt(
        state.frameDc,
        0, 0,
        state.width, state.height,
        state.effectDc,
        0, 0,
        SRCCOPY
    );

    const int tunnelWidth = (std::max)(1, (int)std::lround(state.width * state.zoomScale));
    const int tunnelHeight = (std::max)(1, (int)std::lround(state.height * state.zoomScale));
    const int x = (state.width - tunnelWidth) / 2;
    const int y = (state.height - tunnelHeight) / 2;

    SetStretchBltMode(state.frameDc, HALFTONE);
    SetBrushOrgEx(state.frameDc, 0, 0, nullptr);

    StretchBlt(
        state.frameDc,
        x, y,
        tunnelWidth, tunnelHeight,
        state.effectDc,
        0, 0,
        state.width, state.height,
        SRCCOPY
    );

    BitBlt(
        state.effectDc,
        0, 0,
        state.width, state.height,
        state.frameDc,
        0, 0,
        SRCCOPY
    );

    const double step = 0.0025;

    if (state.zoomingOut) {
        state.zoomScale -= step;
        if (state.zoomScale <= cfg::TunnelMinScale) {
            state.zoomScale = cfg::TunnelMinScale;
            state.zoomingOut = false;
        }
    } else {
        state.zoomScale += step;
        if (state.zoomScale >= cfg::TunnelMaxScale) {
            state.zoomScale = cfg::TunnelMaxScale;
            state.zoomingOut = true;
        }
    }
}

LRESULT CALLBACK TunnelWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<TunnelState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (message) {
        case WM_CREATE: {
            TunnelState* created = nullptr;
            if (!InitTunnelState(hwnd, created)) {
                return -1;
            }
            created->hwnd = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(created));
            
            HANDLE renderThread = CreateThread(nullptr, 0, TunnelRenderThreadProc, created, 0, nullptr);
            if (renderThread) {
                CloseHandle(renderThread);
            }
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            return 0;
        }

        case WM_SHOWWINDOW:
            if (wParam) {
                g_tunnelThreadRunning = true;
                if (state) {
                    StepTunnel(*state);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            } else {
                g_tunnelThreadRunning = false;
            }
            return 0;

        case WM_DISPLAYCHANGE:
        case WM_SETTINGCHANGE:
            if (state) {
                TunnelGeometry(hwnd, *state);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(hwnd, &paint);
            if (state) {
                BitBlt(dc, 0, 0, state->width, state->height, state->frameDc, 0, 0, SRCCOPY);
            }
            EndPaint(hwnd, &paint);
            return 0;
        }

        case WM_DESTROY:
            g_tunnelThreadRunning = false;
            Sleep(100);
            DestroyTunnelState(state);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

}

RECT GetBounds() {
    int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    return {x, y, x + GetSystemMetrics(SM_CXVIRTUALSCREEN), y + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
}

double ScreenScale() {
    const int dim = (std::min)(GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));

    if (dim >= 2160) {
        return 1.8;
    }
    if (dim >= 1440) {
        return 1.5;
    }
    return 1.0;
}

int MaxGifSize() {
    const int dim = (std::min)(GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));

    if (dim >= 2160) {
        return 570;
    }
    if (dim >= 1440) {
        return 330;
    }
    return 300;
}

double RandomVelocity() {
    std::uniform_real_distribution<double> speed(cfg::MinSpeed, cfg::MaxSpeed);
    int sign = std::uniform_int_distribution<int>(0, 1)(g_rng);
    return (sign == 0 ? -1.0 : 1.0) * (speed(g_rng) / 60.0);
}

void UpdateBoost() {
    POINT curPos{};
    if (!GetCursorPos(&curPos)) {
        g_mouseBoost = 1.0;
        return;
    }

    const double dx = static_cast<double>(curPos.x - g_lastMouse.x);
    const double dy = static_cast<double>(curPos.y - g_lastMouse.y);
    g_lastMouse = curPos;
    
    const double boost = (std::min)(1.0 + std::hypot(dx, dy) / 15.4, cfg::MaxMouseBoost);
    if (boost > g_mouseBoost) {
        g_mouseBoost = boost;
    } else {
        g_mouseBoost = (std::max)(1.0, g_mouseBoost - 0.05);
    }
}

void ResolveCollision(WindowState& a, WindowState& b) {
    const double left = (std::max)(a.x, b.x);
    const double right = (std::min)(a.x + a.width, b.x + b.width);
    const double top = (std::max)(a.y, b.y);
    const double bottom = (std::min)(a.y + a.height, b.y + b.height);
    if (right <= left || bottom <= top) {
        return;
    }

    const bool resolveX = (right - left) < (bottom - top);
    if (resolveX) {
        const bool aLeft = a.x < b.x;
        const double axDirection = aLeft ? -1.0 : 1.0;
        const double bxDirection = -axDirection;

        a.x += axDirection;
        a.vx = axDirection * std::abs(a.vx);
        b.vx = bxDirection * std::abs(b.vx);
    } else {
        const bool aAbove = a.y < b.y;
        const double ayDirection = aAbove ? -1.0 : 1.0;
        const double byDirection = -ayDirection;

        a.y += ayDirection;
        a.vy = ayDirection * std::abs(a.vy);
        b.vy = byDirection * std::abs(b.vy);
    }
}

void MoveWindows() {
    UpdateBoost();

    const RECT bounds = GetBounds();

    const double margin = cfg::Margin;
    const double left = bounds.left + margin;
    const double right = bounds.right - margin;
    const double top = bounds.top + margin;
    const double bottom = bounds.bottom - margin;

    for (auto& window : g_windows) {
        if (!window.hwnd || !window.positioned) {
            continue;
        }
        window.x += window.vx * g_mouseBoost;
        window.y += window.vy * g_mouseBoost;

        if (window.x <= left) {
            window.x = left;
            window.vx = std::abs(window.vx);
        }
        if (window.x >= right - window.width) {
            window.x = right - window.width;
            window.vx = -std::abs(window.vx);
        }
        if (window.y <= top) {
            window.y = top;
            window.vy = std::abs(window.vy);
        }
        if (window.y >= bottom - window.height) {
            window.y = bottom - window.height;
            window.vy = -std::abs(window.vy);
        }
    }

    for (int i = 0; i < g_windowCount; ++i) {
        auto& a = g_windows[i];
        if (!a.hwnd || !a.positioned) {
            continue;
        }
        for (int j = i + 1; j < g_windowCount; ++j) {
            auto& b = g_windows[j];
            if (!b.hwnd || !b.positioned) {
                continue;
            }
            ResolveCollision(a, b);
        }
    }

    static ULONGLONG lastUpdate = 0;
    ULONGLONG now = GetTickCount64();
    if (now - lastUpdate >= 8) {
        lastUpdate = now;
        
        HDWP dwp = BeginDeferWindowPos(g_windowCount);
        if (dwp) {
            for (auto& window : g_windows) {
                if (window.hwnd && window.positioned) {
                    dwp = DeferWindowPos(
                        dwp, window.hwnd, nullptr,
                        static_cast<int>(std::lround(window.x)),
                        static_cast<int>(std::lround(window.y)),
                        window.width, window.height,
                        SWP_NOACTIVATE | SWP_NOZORDER
                    );
                }
            }
            EndDeferWindowPos(dwp);
        }
    }
}

void SwapGifs() {
    if (g_windowCount <= 0) {
        return;
    }

    std::uniform_int_distribution<int> d(0, g_windowCount - 1);
    int swapCount = cfg::SwapCount;
    if (g_windowCount > App::Window::BaseCount) {
        swapCount += (g_windowCount - App::Window::BaseCount + 1) / 2;
    }
    if (g_windowCount >= g_targetWindowCount) {
        swapCount = (std::max)(swapCount, g_windowCount / 2);
    }

    for (int i = 0; i < swapCount && i < g_windowCount; ++i) {
        AddGif(d(g_rng));
    }
}

LRESULT CALLBACK ControllerWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_CREATE) {
        g_rampStart = std::chrono::steady_clock::now();
        SetTimer(hwnd, cfg::SwapTimer, cfg::SwapMs, nullptr);
        SetTimer(hwnd, cfg::MoveTimer, cfg::MoveMs, nullptr);
        SetTimer(hwnd, cfg::AudioTimer, cfg::AudioPollMs, nullptr);
        SetTimer(hwnd, cfg::RampTimer, cfg::RampMs, nullptr);
        SetTimer(hwnd, cfg::TunnelStartTimer, cfg::TunnelStartDelayMs, nullptr);
        return 0;
    }
    if (message == WM_TIMER) {
        if (wParam == cfg::SwapTimer) {
            SwapGifs();
        } else if (wParam == cfg::MoveTimer) {
            MoveWindows();
        } else if (wParam == cfg::AudioTimer) {
            g_audio.Update();
        } else if (wParam == cfg::RampTimer) {
            const auto elapsed = std::chrono::steady_clock::now() - g_rampStart;
            if (elapsed >= std::chrono::seconds(cfg::RampStartDelaySec)) {
                SpawnNextWindow();
                if (g_windowCount >= g_targetWindowCount) {
                    g_popupLimitUnlocked.store(true, std::memory_order_relaxed);
                    KillTimer(hwnd, cfg::RampTimer);
                }
            }
        } else if (wParam == cfg::TunnelStartTimer) {
            if (g_tunnel) {
                ShowWindow(g_tunnel, SW_SHOWNOACTIVATE);
            }
            KillTimer(hwnd, cfg::TunnelStartTimer);
        }
        return 0;
    }
    if (message == WM_DESTROY) {
        KillTimer(hwnd, cfg::SwapTimer);
        KillTimer(hwnd, cfg::MoveTimer);
        KillTimer(hwnd, cfg::AudioTimer);
        KillTimer(hwnd, cfg::RampTimer);
        KillTimer(hwnd, cfg::TunnelStartTimer);
        g_audio.Stop();
        if (g_tunnel) {
            DestroyWindow(g_tunnel);
            g_tunnel = nullptr;
        }
        for (auto& window : g_windows) {
            if (window.hwnd) {
                DestroyWindow(window.hwnd);
            }
            window.hwnd = nullptr;
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool RegisterWndClass() {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(WNDCLASSEXW);
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = g_instance;
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = App::Class;
    if (!RegisterClassExW(&windowClass)) {
        return false;
    }

    WNDCLASSEXW controllerClass{};
    controllerClass.cbSize = sizeof(WNDCLASSEXW);
    controllerClass.lpfnWndProc = ControllerWndProc;
    controllerClass.hInstance = g_instance;
    controllerClass.lpszClassName = App::Controller;
    if (!RegisterClassExW(&controllerClass)) {
        return false;
    }

    WNDCLASSEXW tunnelClass{};
    tunnelClass.cbSize = sizeof(WNDCLASSEXW);
    tunnelClass.lpfnWndProc = TunnelWndProc;
    tunnelClass.hInstance = g_instance;
    tunnelClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    tunnelClass.hbrBackground = nullptr;
    tunnelClass.lpszClassName = cfg::TunnelClass;
    return RegisterClassExW(&tunnelClass) != 0;
}

bool CreateWnd(WindowState& window, int windowIndex, HINSTANCE instance) {
    window.hwnd = CreateWindowExW(
        cfg::WindowExStyle,
        App::Class,
        L"",
        cfg::WindowStyle,
        cfg::WindowInitialPos,
        cfg::WindowInitialPos,
        App::Window::Size,
        App::Window::Size,
        nullptr,
        nullptr,
        instance,
        reinterpret_cast<LPVOID>(static_cast<INT_PTR>(windowIndex)));
        
    if (window.hwnd) {
        SetLayeredWindowAttributes(window.hwnd, RGB(255, 0, 255), 255, LWA_COLORKEY | LWA_ALPHA);
    }
    return window.hwnd != nullptr;
}

thread_local HHOOK g_boxHook = nullptr;

LRESULT CALLBACK CBTHook(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HCBT_ACTIVATE) {
        HWND hwnd = (HWND)wParam;
        
        RECT rect;
        if (GetWindowRect(hwnd, &rect)) {
            int boxWidth = rect.right - rect.left;
            int boxHeight = rect.bottom - rect.top;
            int screenWidth = GetSystemMetrics(SM_CXSCREEN);
            int screenHeight = GetSystemMetrics(SM_CYSCREEN);
            
            int maxX = (std::max)(0, screenWidth - boxWidth);
            int maxY = (std::max)(0, screenHeight - boxHeight);
            
            static thread_local std::mt19937 rng{std::random_device{}()};
            std::uniform_int_distribution<int> distX(0, maxX);
            std::uniform_int_distribution<int> distY(0, maxY);
            
            SetWindowPos(hwnd, HWND_TOPMOST, distX(rng), distY(rng), 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
            SetForegroundWindow(hwnd);
            BringWindowToTop(hwnd);
        }

        UnhookWindowsHookEx(g_boxHook);
    }
    return CallNextHookEx(g_boxHook, code, wParam, lParam);
}

DWORD WINAPI ShowMsgBoxThread(LPVOID) {
    g_boxHook = SetWindowsHookExW(WH_CBT, CBTHook, nullptr, GetCurrentThreadId());
    const LPCWSTR text = RandomPopupMessage();
    
    MSGBOXPARAMSW msg{sizeof(MSGBOXPARAMSW), nullptr, g_instance, text, L"Maow", 
                      MB_OK | MB_USERICON | MB_TOPMOST | MB_SETFOREGROUND | MB_SYSTEMMODAL, MAKEINTRESOURCEW(IDI_CAT), 0, nullptr, 0};
    MessageBoxIndirectW(&msg);
    
    return 0;
}

DWORD WINAPI MsgBoxThread(LPVOID) {
    auto nextSpeedUpTime = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    DWORD intervalMs = 2000;
    
    while (!g_shouldExit) {
        CreateThread(nullptr, 0, ShowMsgBoxThread, nullptr, 0, nullptr);
        
        auto now = std::chrono::steady_clock::now();
        if (now >= nextSpeedUpTime) {
            const DWORD minInterval = g_popupLimitUnlocked.load(std::memory_order_relaxed)
                ? cfg::PopupMinMsUnlocked
                : cfg::PopupMinMsLocked;
            intervalMs = (std::max)(minInterval, intervalMs / 2);
            nextSpeedUpTime = now + std::chrono::seconds(10);
        }
        
        Sleep(intervalMs);
    }
    
    return 0;
}

LRESULT CALLBACK KeyboardHook(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && wParam == WM_KEYDOWN) {
        PKBDLLHOOKSTRUCT keyInfo = (PKBDLLHOOKSTRUCT)lParam;
        DWORD keyCode = keyInfo->vkCode;
        
        if (keyCode >= 'A' && keyCode <= 'Z') {
            wchar_t c = L'a' + (keyCode - 'A');
            g_keyBuffer += c;
            
            if (g_keyBuffer.length() > 10) {
                g_keyBuffer = g_keyBuffer.substr(g_keyBuffer.length() - 10);
            }
            
            if (g_keyBuffer.length() >= 4) {
                if (g_keyBuffer.substr(g_keyBuffer.length() - 4) == L"maow") {
                    g_shouldExit = true;
                    ExitProcess(0);
                }
            }
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

DWORD WINAPI KeyboardThreadProc(LPVOID) {
    HHOOK hHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardHook, GetModuleHandleW(nullptr), 0);
    if (!hHook) {
        return 1;
    }
    
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0) && !g_shouldExit) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    
    UnhookWindowsHookEx(hHook);
    return 0;
}

DWORD WINAPI MouseThread(LPVOID) {
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<int> jitterDist(-15, 15);
    
    auto jitterSpeedUp = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    DWORD jitterMs = 80;
    auto lastJitter = std::chrono::steady_clock::now();
    
    while (!g_shouldExit) {
        auto now = std::chrono::steady_clock::now();
        
        if (now >= jitterSpeedUp) {
            jitterMs = (std::max)(static_cast<DWORD>(20), static_cast<DWORD>(jitterMs * 0.75));
            jitterSpeedUp = now + std::chrono::seconds(5);
        }
        
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastJitter).count() >= static_cast<int>(jitterMs)) {
            INPUT input{};
            input.type = INPUT_MOUSE;
            input.mi.dx = jitterDist(rng);
            input.mi.dy = jitterDist(rng);
            input.mi.dwFlags = MOUSEEVENTF_MOVE;
            SendInput(1, &input, sizeof(INPUT));
            lastJitter = now;
        }
        
        Sleep(10);
    }
    
    return 0;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    g_instance = inst;
    g_windowCount = GetSystemMetrics(SM_CMONITORS) > 1 ? App::Window::MultiCount : App::Window::BaseCount;
    g_targetWindowCount = App::Window::MaxCount;
    g_popupLimitUnlocked.store(g_windowCount >= g_targetWindowCount, std::memory_order_relaxed);

    const LPCWSTR startupText = RandomPopupMessage();
    MSGBOXPARAMSW msg{sizeof(MSGBOXPARAMSW), nullptr, inst, startupText, L"Maow", 
                      MB_OK | MB_USERICON, MAKEINTRESOURCEW(IDI_CAT), 0, nullptr, 0};
    MessageBoxIndirectW(&msg);

    KillWallpaperProcesses();
    ApplyWallpaperToAllMonitors(ExtractWallpaperResourceToTemp());
    ShowDesktopNow();

    CreateThread(nullptr, 0, MsgBoxThread, nullptr, 0, nullptr);

    Gdiplus::GdiplusStartupInput gsi;
    if (Gdiplus::GdiplusStartup(&g_gdiplusToken, &gsi, nullptr) != Gdiplus::Ok) {
        return 1;
    }

    if (!RegisterWndClass()) {
        Gdiplus::GdiplusShutdown(g_gdiplusToken);
        return 1;
    }

    const RECT bounds = GetBounds();
    g_tunnel = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
        cfg::TunnelClass,
        L"",
        WS_POPUP,
        bounds.left,
        bounds.top,
        bounds.right - bounds.left,
        bounds.bottom - bounds.top,
        nullptr,
        nullptr,
        inst,
        nullptr
    );
    if (!g_tunnel) {
        Gdiplus::GdiplusShutdown(g_gdiplusToken);
        return 1;
    }
    SetLayeredWindowAttributes(g_tunnel, 0, 255, LWA_ALPHA);

    g_controller = CreateWindowExW(0, App::Controller, L"Ctrl", WS_OVERLAPPED,
                                   0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    if (!g_controller) {
        DestroyWindow(g_tunnel);
        g_tunnel = nullptr;
        Gdiplus::GdiplusShutdown(g_gdiplusToken);
        return 1;
    }

    g_audio.Start();

    CreateThread(nullptr, 0, KeyboardThreadProc, nullptr, 0, nullptr);
    CreateThread(nullptr, 0, MouseThread, nullptr, 0, nullptr);
    
    std::vector<int> ids(GifIds.begin(), GifIds.end());
    std::shuffle(ids.begin(), ids.end(), g_rng);
    
    for (int i = 0; i < g_windowCount; ++i) {
        auto& window = g_windows[i];
        window.vx = RandomVelocity();
        window.vy = RandomVelocity();
        if (!CreateWnd(window, i, inst)) {
            continue;
        }

        if (SetWindowGif(window, ids[i])) {
            ShowWindow(window.hwnd, SW_SHOWNOACTIVATE);
        } else {
            DestroyWindow(window.hwnd);
            window.hwnd = nullptr;
        }
    }

    MSG m{};
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    if (g_controller) {
        DestroyWindow(g_controller);
        g_controller = nullptr;
    }
    Gdiplus::GdiplusShutdown(g_gdiplusToken);
    CoUninitialize();
    return static_cast<int>(m.wParam);
}
