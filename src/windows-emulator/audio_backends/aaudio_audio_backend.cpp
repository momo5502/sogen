#include "../std_include.hpp"
#include <platform/audio_backend.hpp>

#include <aaudio/AAudio.h>

#include <cstring>
#include <mutex>
#include <optional>
#include <vector>

namespace sogen
{
    namespace
    {
        bool is_supported_format(const audio_format& format)
        {
            if (format.channels == 0 || format.sample_rate == 0)
            {
                return false;
            }

            if (format.is_float)
            {
                return format.bits_per_sample == 32;
            }

            return format.bits_per_sample == 8 || format.bits_per_sample == 16 || format.bits_per_sample == 32;
        }

        template <typename T>
        T read_sample(const uint8_t* data)
        {
            T value{};
            std::memcpy(&value, data, sizeof(value));
            return value;
        }

        float to_float_sample(const audio_format& format, const uint8_t* data)
        {
            if (format.is_float)
            {
                return read_sample<float>(data);
            }

            switch (format.bits_per_sample)
            {
            case 8:
                return (static_cast<float>(*data) - 128.0f) / 128.0f;
            case 16:
                return static_cast<float>(read_sample<int16_t>(data)) / 32768.0f;
            default:
                return static_cast<float>(static_cast<double>(read_sample<int32_t>(data)) / 2147483648.0);
            }
        }

        class aaudio_audio_backend final : public audio_backend
        {
          public:
            ~aaudio_audio_backend() override
            {
                this->stop();
            }

            aaudio_audio_backend() = default;
            aaudio_audio_backend(const aaudio_audio_backend&) = delete;
            aaudio_audio_backend& operator=(const aaudio_audio_backend&) = delete;
            aaudio_audio_backend(aaudio_audio_backend&&) = delete;
            aaudio_audio_backend& operator=(aaudio_audio_backend&&) = delete;

            bool start(const audio_format& format) override
            {
                const std::lock_guard lock{this->mutex_};

                if (this->stream_ && this->format_ == format)
                {
                    return true;
                }

                this->close_stream();

                if (!is_supported_format(format))
                {
                    return false;
                }

                AAudioStreamBuilder* builder{};
                if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK)
                {
                    return false;
                }

                AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
                AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
                AAudioStreamBuilder_setChannelCount(builder, format.channels);
                AAudioStreamBuilder_setSampleRate(builder, static_cast<int32_t>(format.sample_rate));
                AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
                AAudioStreamBuilder_setDataCallback(builder, &aaudio_audio_backend::data_callback, this);

                AAudioStream* stream{};
                const auto result = AAudioStreamBuilder_openStream(builder, &stream);
                AAudioStreamBuilder_delete(builder);
                if (result != AAUDIO_OK)
                {
                    return false;
                }

                if (AAudioStream_requestStart(stream) != AAUDIO_OK)
                {
                    AAudioStream_close(stream);
                    return false;
                }

                this->stream_ = stream;
                this->format_ = format;
                return true;
            }

            void submit(const void* data, const size_t size) override
            {
                const std::lock_guard lock{this->mutex_};

                if (!this->stream_ || !data || !size)
                {
                    return;
                }

                const auto bytes_per_sample = static_cast<size_t>(this->format_.bits_per_sample / 8);
                const auto sample_count = size / bytes_per_sample;
                const auto* bytes = static_cast<const uint8_t*>(data);

                this->samples_.reserve(this->samples_.size() + sample_count);
                for (size_t i = 0; i < sample_count; ++i)
                {
                    this->samples_.push_back(to_float_sample(this->format_, bytes + (i * bytes_per_sample)));
                }
            }

            std::optional<uint64_t> queued_bytes() const override
            {
                const std::lock_guard lock{this->mutex_};

                if (!this->stream_)
                {
                    return std::nullopt;
                }

                const auto queued_samples = this->samples_.size() - this->read_offset_;
                return static_cast<uint64_t>(queued_samples) * (this->format_.bits_per_sample / 8);
            }

            void stop() override
            {
                const std::lock_guard lock{this->mutex_};
                this->close_stream();
            }

          private:
            static aaudio_data_callback_result_t data_callback(AAudioStream* stream, void* user_data, void* audio_data,
                                                               const int32_t num_frames)
            {
                auto& self = *static_cast<aaudio_audio_backend*>(user_data);
                const auto requested = static_cast<size_t>(num_frames) * static_cast<size_t>(AAudioStream_getChannelCount(stream));
                self.fill(static_cast<float*>(audio_data), requested);
                return AAUDIO_CALLBACK_RESULT_CONTINUE;
            }

            void fill(float* output, const size_t requested)
            {
                const std::lock_guard lock{this->mutex_};

                const auto available = this->samples_.size() - this->read_offset_;
                const auto count = std::min(requested, available);

                std::memcpy(output, this->samples_.data() + this->read_offset_, count * sizeof(float));
                std::fill(output + count, output + requested, 0.0f);
                this->read_offset_ += count;

                if (this->read_offset_ == this->samples_.size())
                {
                    this->samples_.clear();
                    this->read_offset_ = 0;
                }
                else if (this->read_offset_ > this->samples_.size() / 2)
                {
                    this->samples_.erase(this->samples_.begin(), this->samples_.begin() + static_cast<ptrdiff_t>(this->read_offset_));
                    this->read_offset_ = 0;
                }
            }

            // Called with mutex_ held. AAudioStream_close waits for an in-flight data callback, which takes
            // the same mutex, so the stream is detached first and closed after the lock is released.
            void close_stream()
            {
                auto* stream = std::exchange(this->stream_, nullptr);
                if (!stream)
                {
                    return;
                }

                this->format_ = {};
                this->samples_.clear();
                this->read_offset_ = 0;

                this->mutex_.unlock();
                AAudioStream_requestStop(stream);
                AAudioStream_close(stream);
                this->mutex_.lock();
            }

            mutable std::mutex mutex_{};
            AAudioStream* stream_{};
            audio_format format_{};
            std::vector<float> samples_{};
            size_t read_offset_{};
        };
    }

    std::unique_ptr<audio_backend> create_aaudio_audio_backend()
    {
        return std::make_unique<aaudio_audio_backend>();
    }

} // namespace sogen
