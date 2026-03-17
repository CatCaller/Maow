#pragma once
#include "Mp3Player.h"
#include "common.h"
#include <chrono>


struct BackgroundAudio {
    std::unique_ptr<MP3Player> player;
    std::wstring tempFilePath;
    double durationSeconds = 0.0;
    bool active = false;

    void WriteMp3();
    void Start();
    void Stop();
    void Update();
};
