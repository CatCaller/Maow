#include "window.h"

constexpr UINT_PTR FrameTimer = 1;
constexpr int Margin = 24;
constexpr double SizeScale = 0.80;
constexpr UINT DefaultFrameMs = 100;
constexpr UINT MinFrameMs = 30;
constexpr PROPID DelayTag = 0x5100;

UINT ClampFrameMs(UINT ms) {
    if (ms < MinFrameMs) {
        return MinFrameMs;
    }
    return ms;
}

void ResizeWindow(WindowState &window) {
    const int maxSize = MaxGifSize();

    if (!window.gif.image) {
        const int fallback = static_cast<int>(maxSize * SizeScale);
        window.width = fallback;
        window.height = fallback;
        return;
    }

    const double boost = ScreenScale();
    const double widthScale = maxSize / static_cast<double>(window.gif.width);
    const double heightScale = maxSize / static_cast<double>(window.gif.height);
    const double scale = boost * std::min(1.0, std::min(widthScale, heightScale));

    window.width = (std::max)(1, static_cast<int>(window.gif.width * scale * SizeScale));
    window.height = (std::max)(1, static_cast<int>(window.gif.height * scale * SizeScale));
}

void PlaceWindow(WindowState &window) {
    if (window.positioned) {
        return;
    }

    const RECT bounds = GetBounds();
    std::uniform_int_distribution<int> xDist(bounds.left + Margin, bounds.right - Margin - window.width);
    std::uniform_int_distribution<int> yDist(bounds.top + Margin, bounds.bottom - Margin - window.height);

    window.x = xDist(g_rng);
    window.y = yDist(g_rng);
    window.positioned = true;
}

UINT FirstDelay(const GifState &gif) {
    if (gif.frameDelaysMs.empty()) {
        return DefaultFrameMs;
    }
    return gif.frameDelaysMs[0];
}

void LoadGifInfo(GifState &gif) {
    gif.width = gif.image->GetWidth();
    gif.height = gif.image->GetHeight();

    const UINT dimensionCount = gif.image->GetFrameDimensionsCount();
    if (dimensionCount > 0) {
        std::vector<GUID> dimensions(dimensionCount);
        if (gif.image->GetFrameDimensionsList(dimensions.data(), dimensionCount) == Gdiplus::Ok) {
            gif.frameGuid = dimensions[0];
            gif.frameCount = (std::max)(1U, gif.image->GetFrameCount(&gif.frameGuid));
        }
    }

    gif.frameDelaysMs.assign(gif.frameCount, DefaultFrameMs);

    const UINT propSize = gif.image->GetPropertyItemSize(DelayTag);
    if (propSize == 0) {
        return;
    }

    std::vector<BYTE> bytes(propSize);
    auto* prop = reinterpret_cast<Gdiplus::PropertyItem*>(bytes.data());
    if (gif.image->GetPropertyItem(DelayTag, propSize, prop) != Gdiplus::Ok) {
        return;
    }

    const UINT delayCount = (std::min)(gif.frameCount, static_cast<UINT>(prop->length / sizeof(UINT)));
    const auto* delayData = reinterpret_cast<const UINT*>(prop->value);
    for (UINT i = 0; i < delayCount; ++i) {
        gif.frameDelaysMs[i] = ClampFrameMs(delayData[i] * 10);
    }
}

bool IsGifUsed(int windowIndex, int resourceId) {
    for (int i = 0; i < g_windowCount; ++i) {
        if (i == windowIndex) {
            continue;
        }

        if (g_windows[i].resourceId == resourceId) {
            return true;
        }
    }

    return false;
}

void ApplyPos(const WindowState &window) {
    if (!window.hwnd) {
        return;
    }

    SetWindowPos(window.hwnd, nullptr, static_cast<int>(std::lround(window.x)), static_cast<int>(std::lround(window.y)), window.width,
                 window.height, SWP_NOACTIVATE | SWP_NOZORDER);
}

void ResizeAndPlaceWindow(WindowState &window) {
    if (!window.hwnd) {
        return;
    }

    ResizeWindow(window);
    PlaceWindow(window);
    ApplyPos(window);
}

bool LoadGif(int resourceId, GifState &outState) {
    HRSRC resource = FindResource(g_instance, MAKEINTRESOURCE(resourceId), RT_RCDATA);
    if (!resource) {
        return false;
    }

    const DWORD size = SizeofResource(g_instance, resource);
    HGLOBAL loaded = LoadResource(g_instance, resource);
    void* source = LockResource(loaded);
    if (size == 0 || !loaded || !source) {
        return false;
    }

    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!memory) {
        return false;
    }

    void* dest = GlobalLock(memory);
    if (!dest) {
        GlobalFree(memory);
        return false;
    }

    memcpy(dest, source, size);
    GlobalUnlock(memory);

    IStream* stream = nullptr;
    if (CreateStreamOnHGlobal(memory, TRUE, &stream) != S_OK) {
        GlobalFree(memory);
        return false;
    }

    auto image = std::unique_ptr<Gdiplus::Image>(Gdiplus::Image::FromStream(stream, FALSE));
    if (!image || image->GetLastStatus() != Gdiplus::Ok) {
        stream->Release();
        return false;
    }

    GifState gif;
    gif.image = std::move(image);
    gif.stream = stream;
    LoadGifInfo(gif);

    if (gif.frameCount > 1) {
        gif.image->SelectActiveFrame(&gif.frameGuid, 0);
    }

    outState = std::move(gif);
    return true;
}

bool SetWindowGif(WindowState &window, int resourceId) {
    GifState gif;
    if (!LoadGif(resourceId, gif)) {
        return false;
    }

    window.gif = std::move(gif);
    window.resourceId = resourceId;

    ResizeAndPlaceWindow(window);
    InvalidateRect(window.hwnd, nullptr, TRUE);
    SetTimer(window.hwnd, FrameTimer, FirstDelay(window.gif), nullptr);
    return true;
}

bool AddGif(int windowIndex) {
    std::vector<int> available;

    for (int resourceId : GifIds) {
        if (!IsGifUsed(windowIndex, resourceId)) {
            available.push_back(resourceId);
        }
    }

    if (available.empty()) {
        return false;
    }

    std::shuffle(available.begin(), available.end(), g_rng);
    for (int resourceId : available) {
        if (SetWindowGif(g_windows[windowIndex], resourceId)) {
            return true;
        }
    }

    return false;
}

void AdvanceFrame(WindowState &window) {
    if (!window.gif.image || window.gif.frameCount <= 1) {
        SetTimer(window.hwnd, FrameTimer, DefaultFrameMs, nullptr);
        return;
    }

    window.gif.frameIndex = (window.gif.frameIndex + 1) % window.gif.frameCount;
    window.gif.image->SelectActiveFrame(&window.gif.frameGuid, window.gif.frameIndex);
    InvalidateRect(window.hwnd, nullptr, TRUE);

    const UINT frameMs = window.gif.frameDelaysMs[window.gif.frameIndex % window.gif.frameDelaysMs.size()];
    SetTimer(window.hwnd, FrameTimer, ClampFrameMs(frameMs), nullptr);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return TRUE;
    }

    const int windowIndex = static_cast<int>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (windowIndex < 0 || windowIndex >= g_windowCount) {
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    WindowState &window = g_windows[windowIndex];

    switch (message) {
    case WM_ERASEBKGND:
        return 1;

    case WM_TIMER:
        if (wParam == FrameTimer) {
            AdvanceFrame(window);
            return 0;
        }
        break;

    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);

        RECT rect{};
        GetClientRect(hwnd, &rect);

        HBRUSH bg = CreateSolidBrush(RGB(255, 0, 255));
        FillRect(dc, &rect, bg);
        DeleteObject(bg);

        Gdiplus::Graphics graphics(dc);
        graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighSpeed);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
        if (window.gif.image) {
            graphics.DrawImage(window.gif.image.get(), Gdiplus::Rect(0, 0, rect.right, rect.bottom));
        }

        EndPaint(hwnd, &paint);
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hwnd, FrameTimer);
        return 0;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}
