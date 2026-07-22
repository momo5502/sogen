// Minimal WASAPI render-init walk. Exercises the audio service RPC path
// (GetDefaultAudioEndpoint -> Activate(IAudioClient) -> GetMixFormat -> Initialize -> Start)
// in isolation, so the emulator's audio support can be iterated without a full game.

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#include <cmath>
#include <numbers>
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

#define STEP(expr, what)           \
    if (!report(what, (expr)))     \
    {                              \
        std::puts("AUDIO FAILED"); \
        return 1;                  \
    }

int main()
{
    STEP(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "CoInitializeEx");

    IMMDeviceEnumerator* enumerator = nullptr;
    STEP(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                          reinterpret_cast<void**>(&enumerator)),
         "CoCreateInstance");

    IMMDevice* device = nullptr;
    STEP(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device), "GetDefaultAudioEndpoint");

    IAudioClient* client = nullptr;
    STEP(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client)), "Activate");

    WAVEFORMATEX* format = nullptr;
    STEP(client->GetMixFormat(&format), "GetMixFormat");
    std::printf("  format tag=%u ch=%u rate=%lu bits=%u cbSize=%u\n", format->wFormatTag, format->nChannels,
                static_cast<unsigned long>(format->nSamplesPerSec), format->wBitsPerSample, format->cbSize);

    STEP(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, format, nullptr), "Initialize");

    UINT32 buffer_frames = 0;
    STEP(client->GetBufferSize(&buffer_frames), "GetBufferSize");
    std::printf("  bufferFrames=%u\n", buffer_frames);

    IAudioRenderClient* render = nullptr;
    STEP(client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render)), "GetService");

    BYTE* data = nullptr;
    STEP(render->GetBuffer(buffer_frames, &data), "GetBuffer");

    const auto is_float = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                          (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                           reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    const double frequency = 440.0;
    const double step = 2.0 * std::numbers::pi * frequency / format->nSamplesPerSec;
    for (UINT32 frame = 0; frame < buffer_frames; ++frame)
    {
        const double sample = 0.25 * std::sin(step * frame);
        for (unsigned channel = 0; channel < format->nChannels; ++channel)
        {
            BYTE* slot = data + (static_cast<size_t>(frame) * format->nBlockAlign) + (channel * (format->wBitsPerSample / 8));
            if (is_float)
            {
                const auto value = static_cast<float>(sample);
                std::memcpy(slot, &value, sizeof(value));
            }
            else
            {
                const auto value = static_cast<int16_t>(sample * 32767.0);
                std::memcpy(slot, &value, sizeof(value));
            }
        }
    }

    STEP(render->ReleaseBuffer(buffer_frames, 0), "ReleaseBuffer");

    STEP(client->Start(), "Start");
    Sleep(1000);
    STEP(client->Stop(), "Stop");

    std::puts("AUDIO OK");
    return 0;
}
