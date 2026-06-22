// Minimal DirectSound render walk. DirectSound on Vista+ is layered on top of the
// audio engine (audioses / IAudioClient), so this exercises the same audio service
// RPC path as audio-sample, but through the older dsound.dll front end.

#include <windows.h>
#include <dsound.h>

#include <cstdio>
#include <cstring>

namespace
{
    bool report(const char* what, const HRESULT hr)
    {
        std::printf("%-28s 0x%08lx\n", what, static_cast<unsigned long>(hr));
        std::fflush(stdout);
        return SUCCEEDED(hr);
    }
}

#define STEP(expr, what)            \
    if (!report(what, (expr)))      \
    {                               \
        std::puts("DSOUND FAILED"); \
        return 1;                   \
    }

int main()
{
    STEP(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED), "CoInitializeEx");

    IDirectSound8* ds = nullptr;
    STEP(DirectSoundCreate8(nullptr, &ds, nullptr), "DirectSoundCreate8");

    STEP(ds->SetCooperativeLevel(GetDesktopWindow(), DSSCL_PRIORITY), "SetCooperativeLevel");

    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = 48000;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = static_cast<WORD>(fmt.nChannels * fmt.wBitsPerSample / 8);
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;

    DSBUFFERDESC desc{};
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DSBCAPS_GLOBALFOCUS;
    desc.dwBufferBytes = fmt.nAvgBytesPerSec; // one second of audio
    desc.lpwfxFormat = &fmt;

    IDirectSoundBuffer* buffer = nullptr;
    STEP(ds->CreateSoundBuffer(&desc, &buffer, nullptr), "CreateSoundBuffer");

    void* region = nullptr;
    DWORD region_bytes = 0;
    STEP(buffer->Lock(0, desc.dwBufferBytes, &region, &region_bytes, nullptr, nullptr, 0), "Lock");
    std::memset(region, 0, region_bytes); // silence
    STEP(buffer->Unlock(region, region_bytes, nullptr, 0), "Unlock");

    STEP(buffer->Play(0, 0, 0), "Play");
    Sleep(100);
    STEP(buffer->Stop(), "Stop");

    std::puts("DSOUND OK");
    return 0;
}
