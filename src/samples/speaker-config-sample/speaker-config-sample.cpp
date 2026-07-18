// Probes the exact host-audio calls Miles Sound System (mss32.dll) makes while opening its digital driver
// for MW3, so we can see which one fails in the emulator. Miles' path (reversed from mss32!FUN_2112ea10 /
// FUN_2112ee40 / FUN_2112fbf0):
//   1. get_system_speaker_configuration: DirectSoundCreate(NULL) + IDirectSound::GetSpeakerConfig.
//   2. actual output, gated by a Miles global: either waveOutOpen(WAVE_MAPPER, wfx) OR the DirectSound path
//      (DirectSoundCreate + SetCooperativeLevel(PRIORITY) + a PRIMARY DSBUFFER + a secondary buffer).
// The plain dsound-sample only makes a DSSCL_NORMAL secondary buffer, so it never exercises the primary
// buffer / waveOut paths that Miles depends on.

#include <windows.h>
#include <dsound.h>
#include <mmsystem.h>

#include <cstdio>

namespace
{
    void report(const char* what, const HRESULT hr)
    {
        std::printf("%-32s 0x%08lx %s\n", what, static_cast<unsigned long>(hr), SUCCEEDED(hr) ? "OK" : "FAIL");
        std::fflush(stdout);
    }

    WAVEFORMATEX pcm_44100_16_stereo()
    {
        WAVEFORMATEX wfx{};
        wfx.wFormatTag = WAVE_FORMAT_PCM;
        wfx.nChannels = 2;
        wfx.nSamplesPerSec = 44100;
        wfx.wBitsPerSample = 16;
        wfx.nBlockAlign = static_cast<WORD>(wfx.nChannels * wfx.wBitsPerSample / 8);
        wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
        return wfx;
    }
}

int main()
{
    report("CoInitializeEx", CoInitializeEx(nullptr, COINIT_MULTITHREADED));

    // 1. Speaker-config query (Miles get_system_speaker_configuration).
    {
        LPDIRECTSOUND ds = nullptr;
        const HRESULT hr = DirectSoundCreate(nullptr, &ds, nullptr);
        report("DirectSoundCreate(speaker)", hr);
        if (SUCCEEDED(hr))
        {
            DWORD cfg = 0;
            report("GetSpeakerConfig", ds->GetSpeakerConfig(&cfg));
            std::printf("  speakerConfig=0x%08lx\n", cfg);
            ds->Release();
        }
    }

    // 2a. waveOut output path (Miles FUN_2112ee40 when its global selects winmm).
    {
        WAVEFORMATEX wfx = pcm_44100_16_stereo();
        HWAVEOUT hwo = nullptr;
        const MMRESULT mr = waveOutOpen(&hwo, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL);
        report("waveOutOpen", static_cast<HRESULT>(mr));
        if (mr == MMSYSERR_NOERROR)
        {
            waveOutClose(hwo);
        }
    }

    // 2b. DirectSound output path (Miles FUN_2112e4a0): PRIORITY level + a PRIMARY buffer, then a secondary.
    {
        LPDIRECTSOUND ds = nullptr;
        HRESULT hr = DirectSoundCreate(nullptr, &ds, nullptr);
        report("DirectSoundCreate(output)", hr);
        if (SUCCEEDED(hr))
        {
            hr = ds->SetCooperativeLevel(GetDesktopWindow(), DSSCL_PRIORITY);
            report("SetCooperativeLevel(PRIORITY)", hr);

            DSBUFFERDESC primary{};
            primary.dwSize = sizeof(primary);
            primary.dwFlags = DSBCAPS_PRIMARYBUFFER;
            LPDIRECTSOUNDBUFFER primary_buf = nullptr;
            hr = ds->CreateSoundBuffer(&primary, &primary_buf, nullptr);
            report("CreateSoundBuffer(PRIMARY)", hr);
            if (SUCCEEDED(hr))
            {
                WAVEFORMATEX wfx = pcm_44100_16_stereo();
                report("primary SetFormat", primary_buf->SetFormat(&wfx));
                primary_buf->Release();
            }

            WAVEFORMATEX wfx = pcm_44100_16_stereo();
            DSBUFFERDESC secondary{};
            secondary.dwSize = sizeof(secondary);
            secondary.dwFlags = DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_GLOBALFOCUS;
            secondary.dwBufferBytes = wfx.nAvgBytesPerSec;
            secondary.lpwfxFormat = &wfx;
            LPDIRECTSOUNDBUFFER secondary_buf = nullptr;
            hr = ds->CreateSoundBuffer(&secondary, &secondary_buf, nullptr);
            report("CreateSoundBuffer(secondary)", hr);
            if (SUCCEEDED(hr))
            {
                report("secondary Play", secondary_buf->Play(0, 0, DSBPLAY_LOOPING));
                DWORD play = 0;
                DWORD write = 0;
                report("GetCurrentPosition", secondary_buf->GetCurrentPosition(&play, &write));
                std::printf("  play=%lu write=%lu\n", play, write);
                secondary_buf->Release();
            }
            ds->Release();
        }
    }

    std::puts("PROBE DONE");
    return 0;
}
