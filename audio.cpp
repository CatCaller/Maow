#include "audio.h"
#include "caramelldansen.h"

unsigned long long RandomSuffix() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<unsigned long long> dist;
    return dist(rng);
}

extern volatile bool g_shouldExit;

void BackgroundAudio::WriteMp3() {
    wchar_t dir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, dir);

    tempFilePath = std::wstring(dir) + L"maow_audio_" + std::to_wstring(RandomSuffix()) + L".mp3";

    HANDLE f = CreateFileW(tempFilePath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    DWORD wrote = 0;
    WriteFile(f, caramelldansen_data, (DWORD)caramelldansen_size, &wrote, nullptr);
    FlushFileBuffers(f);
    CloseHandle(f);
}

void BackgroundAudio::Start() {
    Stop();
    WriteMp3();
    Sleep(50);

    auto p = std::make_unique<MP3Player>();
    p->OpenFromFile(tempFilePath.c_str());
    p->Play();

    durationSeconds = p->GetDuration();
    player = std::move(p);
    active = true;
}

void BackgroundAudio::Update() {
    if (!active || !player) {
        return;
    }

    double pos = player->GetPosition();
    if (pos >= durationSeconds) {
        g_shouldExit = true;
        PostQuitMessage(0);
    }
}

void BackgroundAudio::Stop() {
    if (player) {
        player->Close();
        player.reset();
    }
    if (!tempFilePath.empty()) {
        DeleteFileW(tempFilePath.c_str());
        tempFilePath.clear();
    }
    durationSeconds = 0.0;
    active = false;
}
