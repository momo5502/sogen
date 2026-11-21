#include "../std_include.hpp"
#include <platform/audio_backend.hpp>

#include <mutex>
#include <optional>

#include <SDL3/SDL.h>

namespace sogen
{
    namespace
    {
        SDL_AudioFormat to_sdl_format(const audio_format& format)
        {
            if (format.is_float)
            {
                return format.bits_per_sample == 32 ? SDL_AUDIO_F32 : SDL_AUDIO_UNKNOWN;
            }

            switch (format.bits_per_sample)
            {
            case 8:
                return SDL_AUDIO_U8;
            case 16:
                return SDL_AUDIO_S16;
            case 32:
                return SDL_AUDIO_S32;
            default:
                return SDL_AUDIO_UNKNOWN;
            }
        }

        // The host playback device. SDL mixes every stream bound to it, converting each from its own format.
        class sdl_audio_device
        {
          public:
            explicit sdl_audio_device(const SDL_AudioDeviceID id)
                : id_(id)
            {
            }

            ~sdl_audio_device()
            {
                SDL_CloseAudioDevice(this->id_);
                SDL_QuitSubSystem(SDL_INIT_AUDIO);
            }

            sdl_audio_device(const sdl_audio_device&) = delete;
            sdl_audio_device& operator=(const sdl_audio_device&) = delete;
            sdl_audio_device(sdl_audio_device&&) = delete;
            sdl_audio_device& operator=(sdl_audio_device&&) = delete;

            SDL_AudioDeviceID id() const
            {
                return this->id_;
            }

          private:
            SDL_AudioDeviceID id_{};
        };

        class sdl_audio_output final : public audio_output
        {
          public:
            sdl_audio_output(std::shared_ptr<sdl_audio_device> device, SDL_AudioStream* stream)
                : device_(std::move(device)),
                  stream_(stream)
            {
            }

            ~sdl_audio_output() override
            {
                SDL_DestroyAudioStream(this->stream_);
            }

            sdl_audio_output(const sdl_audio_output&) = delete;
            sdl_audio_output& operator=(const sdl_audio_output&) = delete;
            sdl_audio_output(sdl_audio_output&&) = delete;
            sdl_audio_output& operator=(sdl_audio_output&&) = delete;

            void submit(const void* data, const size_t size) override
            {
                if (data && size)
                {
                    SDL_PutAudioStreamData(this->stream_, data, static_cast<int>(size));
                }
            }

            std::optional<uint64_t> queued_bytes() const override
            {
                const auto queued = SDL_GetAudioStreamQueued(this->stream_);
                return queued < 0 ? std::nullopt : std::optional{static_cast<uint64_t>(queued)};
            }

          private:
            std::shared_ptr<sdl_audio_device> device_{};
            SDL_AudioStream* stream_{};
        };

        class sdl_audio_backend final : public audio_backend
        {
          public:
            std::unique_ptr<audio_output> open_output(const audio_format& format) override
            {
                const auto sdl_format = to_sdl_format(format);
                if (sdl_format == SDL_AUDIO_UNKNOWN || format.channels == 0 || format.sample_rate == 0)
                {
                    return nullptr;
                }

                const auto device = this->device();
                if (!device)
                {
                    return nullptr;
                }

                const SDL_AudioSpec spec{
                    .format = sdl_format, .channels = static_cast<int>(format.channels), .freq = static_cast<int>(format.sample_rate)};
                auto* stream = SDL_CreateAudioStream(&spec, nullptr);
                if (!stream)
                {
                    return nullptr;
                }

                if (!SDL_BindAudioStream(device->id(), stream))
                {
                    SDL_DestroyAudioStream(stream);
                    return nullptr;
                }

                return std::make_unique<sdl_audio_output>(device, stream);
            }

          private:
            // Opened on first use and kept while this backend or any of its outputs exist.
            std::shared_ptr<sdl_audio_device> device()
            {
                const std::lock_guard lock{this->mutex_};
                if (this->device_)
                {
                    return this->device_;
                }

                if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
                {
                    return nullptr;
                }

                const auto id = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
                if (id == 0)
                {
                    SDL_QuitSubSystem(SDL_INIT_AUDIO);
                    return nullptr;
                }

                SDL_ResumeAudioDevice(id);
                this->device_ = std::make_shared<sdl_audio_device>(id);
                return this->device_;
            }

            std::mutex mutex_{};
            std::shared_ptr<sdl_audio_device> device_{};
        };
    }

    std::unique_ptr<audio_backend> create_sdl_audio_backend()
    {
        return std::make_unique<sdl_audio_backend>();
    }

} // namespace sogen
