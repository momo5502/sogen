#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace sogen
{
    struct audio_format
    {
        uint32_t sample_rate{};
        uint16_t channels{};
        uint16_t bits_per_sample{};
        bool is_float{};

        bool operator==(const audio_format& o) const
        {
            return sample_rate == o.sample_rate && channels == o.channels && bits_per_sample == o.bits_per_sample && is_float == o.is_float;
        }
    };

    // One stream on the host output device. Every open output plays at once, mixed by the host, each in the format
    // it was opened with.
    class audio_output
    {
      public:
        virtual ~audio_output() = default;

        // Enqueue interleaved PCM in the output's format. The host plays it back at the device rate, buffering as
        // needed.
        virtual void submit(const void* data, size_t size) = 0;

        // Bytes handed to submit() that the host has not consumed yet, when the backend can report it. Callers use it
        // to derive how much audio actually reached the speakers instead of estimating from wall clock, which lets
        // them apply real backpressure and keep latency bounded. std::nullopt = not supported.
        virtual std::optional<uint64_t> queued_bytes() const
        {
            return std::nullopt;
        }
    };

    class audio_backend
    {
      public:
        virtual ~audio_backend() = default;

        // Opens an output in the given format. Returns null when no host audio device is available or the format is
        // not supported. An output may outlive the backend that opened it.
        virtual std::unique_ptr<audio_output> open_output(const audio_format& format) = 0;
    };

    class null_audio_backend final : public audio_backend
    {
      public:
        std::unique_ptr<audio_output> open_output(const audio_format& /*format*/) override
        {
            return nullptr;
        }
    };

    std::unique_ptr<audio_backend> create_default_audio_backend();
    std::unique_ptr<audio_backend> create_sdl_audio_backend();
    std::unique_ptr<audio_backend> create_aaudio_audio_backend();

} // namespace sogen
