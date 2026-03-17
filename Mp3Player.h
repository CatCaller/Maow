#pragma once

#include "common.h"
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <evr.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mmdeviceapi.h>
#include <random>
#include <string>
#include <wrl/client.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "mmdevapi.lib")

using Microsoft::WRL::ComPtr;

class MP3Player {
private:
  ComPtr<IMFMediaSession> mediaSession;
  ComPtr<IMFMediaSource> mediaSource;
  std::wstring tempFilePath;
  double duration = 0.0;
  bool opened = false;
  bool initialized = false;

  static unsigned long long RandomSuffix() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<unsigned long long> dist;
    return dist(rng);
  }

  static std::wstring GetTempPathStr() {
    wchar_t tempDir[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, tempDir) == 0) {
      return L"";
    }
    return std::wstring(tempDir) + L"maow_" + std::to_wstring(RandomSuffix()) +
           L".mp3";
  }

  HRESULT EnsureInitialized() {
    if (initialized) {
      return S_OK;
    }

    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
      return hr;
    }

    hr = MFCreateMediaSession(nullptr, &mediaSession);
    if (FAILED(hr)) {
      MFShutdown();
      return hr;
    }

    initialized = true;
    return S_OK;
  }

  bool WriteTemp(const BYTE* data, DWORD size) {
    tempFilePath = GetTempPathStr();
    HANDLE file = CreateFileW(tempFilePath.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD written = 0;
    WriteFile(file, data, size, &written, nullptr);
    FlushFileBuffers(file);
    CloseHandle(file);
    return true;
  }

  HRESULT CreateTopology(IMFMediaSource* source, IMFMediaSession* session) {
    ComPtr<IMFTopology> topology;
    MFCreateTopology(&topology);

    ComPtr<IMFPresentationDescriptor> presentationDescriptor;
    source->CreatePresentationDescriptor(&presentationDescriptor);

    DWORD streamCount = 0;
    presentationDescriptor->GetStreamDescriptorCount(&streamCount);

    for (DWORD i = 0; i < streamCount; i++) {
      BOOL selected = FALSE;
      ComPtr<IMFStreamDescriptor> streamDescriptor;
      presentationDescriptor->GetStreamDescriptorByIndex(i, &selected,
                                                         &streamDescriptor);

      if (!selected) {
        presentationDescriptor->SelectStream(i);
      }

      ComPtr<IMFTopologyNode> sourceNode;
      MFCreateTopologyNode(MF_TOPOLOGY_SOURCESTREAM_NODE, &sourceNode);
      sourceNode->SetUnknown(MF_TOPONODE_SOURCE, source);
      sourceNode->SetUnknown(MF_TOPONODE_PRESENTATION_DESCRIPTOR,
                             presentationDescriptor.Get());
      sourceNode->SetUnknown(MF_TOPONODE_STREAM_DESCRIPTOR,
                             streamDescriptor.Get());
      topology->AddNode(sourceNode.Get());

      ComPtr<IMFActivate> sinkActivate;
      MFCreateAudioRendererActivate(&sinkActivate);

      ComPtr<IMFTopologyNode> sinkNode;
      MFCreateTopologyNode(MF_TOPOLOGY_OUTPUT_NODE, &sinkNode);
      sinkNode->SetObject(sinkActivate.Get());
      topology->AddNode(sinkNode.Get());

      sourceNode->ConnectOutput(0, sinkNode.Get(), 0);
    }

    session->SetTopology(MFSESSION_SETTOPOLOGY_IMMEDIATE, topology.Get());
    return S_OK;
  }

  HRESULT GetMediaSourceDuration(IMFMediaSource* source, double &outDuration) {
    ComPtr<IMFPresentationDescriptor> presentationDescriptor;
    source->CreatePresentationDescriptor(&presentationDescriptor);
    LONGLONG duration = 0;
    presentationDescriptor->GetUINT64(MF_PD_DURATION, (UINT64 *)&duration);
    outDuration = static_cast<double>(duration) / 10000000.0;
    return S_OK;
  }

public:
  MP3Player() = default;

  ~MP3Player() { Close(); }

  HRESULT OpenFromFile(const wchar_t* inputFileName) {
    Close();
    HRESULT hr = EnsureInitialized();
    if (FAILED(hr)) {
      return hr;
    }

    ComPtr<IMFSourceResolver> sourceResolver;
    MFCreateSourceResolver(&sourceResolver);

    MF_OBJECT_TYPE objectType = MF_OBJECT_INVALID;
    sourceResolver->CreateObjectFromURL(inputFileName,
                                        MF_RESOLUTION_MEDIASOURCE, nullptr,
                                        &objectType, (IUnknown **)&mediaSource);

    CreateTopology(mediaSource.Get(), mediaSession.Get());

    GetMediaSourceDuration(mediaSource.Get(), duration);
    opened = true;
    return S_OK;
  }

  HRESULT OpenFromMemory(BYTE* mp3InputBuffer, DWORD mp3InputBufferSize) {
    if (mp3InputBuffer == nullptr || mp3InputBufferSize == 0) {
      return E_INVALIDARG;
    }

    if (!WriteTemp(mp3InputBuffer, mp3InputBufferSize)) {
      return E_FAIL;
    }

    HRESULT hr = OpenFromFile(tempFilePath.c_str());
    if (FAILED(hr)) {
      if (!tempFilePath.empty()) {
        DeleteFileW(tempFilePath.c_str());
        tempFilePath.clear();
      }
    }

    return hr;
  }

  void Close() {
    if (mediaSession) {
      mediaSession->Stop();
    }

    mediaSource.Reset();
    mediaSession.Reset();

    if (!tempFilePath.empty()) {
      DeleteFileW(tempFilePath.c_str());
      tempFilePath.clear();
    }

    duration = 0.0;
    opened = false;
  }

  double GetDuration() { return duration; }

  double GetPosition() {
    if (!mediaSession) {
      return 0.0;
    }
    ComPtr<IMFClock> clock;
    mediaSession->GetClock(&clock);
    MFTIME position = 0;
    MFTIME systemTime = 0;
    clock->GetCorrelatedTime(0, &position, &systemTime);
    return static_cast<double>(position) / 10000000.0;
  }

  void Play() {
    if (!mediaSession) {
      return;
    }
    PROPVARIANT varStart;
    PropVariantInit(&varStart);
    mediaSession->Start(&GUID_NULL, &varStart);
    PropVariantClear(&varStart);

    SetSystemVolume(0.6f);
  }

  void SetSystemVolume(float volume) {
    ComPtr<IMMDeviceEnumerator> deviceEnumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&deviceEnumerator)))) {
      return;
    }

    ComPtr<IMMDevice> device;
    if (FAILED(deviceEnumerator->GetDefaultAudioEndpoint(eRender, eConsole,
                                                         &device))) {
      return;
    }

    ComPtr<IAudioEndpointVolume> endpointVolume;
    if (FAILED(device->Activate(__uuidof(IAudioEndpointVolume),
                                CLSCTX_INPROC_SERVER, nullptr,
                                (void **)&endpointVolume))) {
      return;
    }

    endpointVolume->SetMasterVolumeLevelScalar(volume, nullptr);
  }

  void SetVolume(float volume) { SetSystemVolume(volume); }
};
