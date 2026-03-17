#pragma once
#include "common.h"

void ApplyPos(const WindowState &window);
void ResizeAndPlaceWindow(WindowState &window);
bool LoadGif(int resourceId, GifState &outState);
bool SetWindowGif(WindowState &window, int resourceId);
bool AddGif(int windowIndex);
void AdvanceFrame(WindowState &window);
LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
