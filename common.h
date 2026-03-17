#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <gdiplus.h>
#include <array>
#include <memory>
#include <random>
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>

extern volatile bool g_shouldExit;

namespace App {

constexpr wchar_t Class[] = L"Wnd";
constexpr wchar_t Controller[] = L"Controller";

struct Window {
    static constexpr int BaseCount = 8;
    static constexpr int MultiCount = 12;
    static constexpr int MaxCount = 12;
    static constexpr int Size = 220;
};

}

namespace cfg {
constexpr DWORD WindowExStyle = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
constexpr DWORD WindowStyle = WS_POPUP | WS_VISIBLE;
constexpr int WindowInitialPos = CW_USEDEFAULT;

constexpr UINT_PTR SwapTimer = 2;
constexpr UINT_PTR MoveTimer = 3;
constexpr UINT_PTR AudioTimer = 4;
constexpr UINT_PTR RampTimer = 6;
constexpr UINT_PTR TunnelTimer = 5;
constexpr UINT_PTR TunnelStartTimer = 7;
constexpr UINT_PTR ZOrderTimer = 8;
constexpr UINT ZOrderTimerMs = 100;

constexpr UINT SwapMs = 2000;
constexpr UINT MoveMs = 8;
constexpr UINT AudioPollMs = 200;
constexpr UINT RampMs = 5000;
constexpr UINT TunnelFrameMs = 25;
constexpr UINT TunnelStartDelayMs = 5000;

constexpr int SwapCount = 1;
constexpr int Margin = 24;
constexpr int RampStartDelaySec = 0;

constexpr double MinSpeed = 325.0;
constexpr double MaxSpeed = 585.0;
constexpr double MaxMouseBoost = 3.0;
constexpr double TunnelMinScale = 0.03;
constexpr double TunnelMaxScale = 1.0;

constexpr DWORD PopupMinMsLocked = 500;
constexpr DWORD PopupMinMsUnlocked = 1;

constexpr wchar_t TunnelClass[] = L"Tunnel";
}

struct GifState {
    std::unique_ptr<Gdiplus::Image> image;
    IStream* stream = nullptr;
    GUID frameGuid{};
    UINT frameCount = 1;
    UINT frameIndex = 0;
    UINT width = App::Window::Size;
    UINT height = App::Window::Size;
    std::vector<UINT> frameDelaysMs;

    GifState() = default;
    GifState(const GifState&) = delete;
    GifState& operator=(const GifState&) = delete;
    GifState(GifState&& other) noexcept { *this = std::move(other); }
    GifState& operator=(GifState&& other) noexcept {
        if (this != &other) {
            Release();
            image = std::move(other.image);
            stream = other.stream;
            frameGuid = other.frameGuid;
            frameCount = other.frameCount;
            frameIndex = other.frameIndex;
            width = other.width;
            height = other.height;
            frameDelaysMs = std::move(other.frameDelaysMs);
            other.stream = nullptr;
            other.frameCount = 1;
            other.frameIndex = 0;
        }
        return *this;
    }
    ~GifState() { Release(); }
    void Release() {
        image.reset();
        if (stream) {
            stream->Release();
            stream = nullptr;
        }
    }
};

struct WindowState {
    HWND hwnd = nullptr;
    GifState gif;
    int resourceId = 0;
    int width = App::Window::Size;
    int height = App::Window::Size;
    double x = 0.0;
    double y = 0.0;
    double vx = 0.0;
    double vy = 0.0;
    bool positioned = false;
};

extern HINSTANCE g_instance;
extern ULONG_PTR g_gdiplusToken;
extern std::array<WindowState, App::Window::MaxCount> g_windows;
extern std::mt19937 g_rng;
extern int g_windowCount;

extern const std::array<int, 17> GifIds;

RECT GetBounds();
double ScreenScale();
int MaxGifSize();
