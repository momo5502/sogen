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

        // One AAudio output stream. Android mixes every stream an app has open.
        class aaudio_audio_output final : public audio_output
        {
          public:
            explicit aaudio_audio_output(const audio_format& format)
                : format_(format)
            {
            }

            ~aaudio_audio_output() override
            {
                if (this->stream_)
                {
                    // AAudioStream_close waits for an in-flight data callback, which takes mutex_, so it is not held here.
                    AAudioStream_requestStop(this->stream_);
                    AAudioStream_close(this->stream_);
                }
            }

            aaudio_audio_output(const aaudio_audio_output&) = delete;
            aaudio_audio_output& operator=(const aaudio_audio_output&) = delete;
            aaudio_audio_output(aaudio_audio_output&&) = delete;
            aaudio_audio_output& operator=(aaudio_audio_output&&) = delete;

            bool open()
            {
                AAudioStreamBuilder* builder{};
                if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK)
                {
                    return false;
                }

                AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
                AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
                AAudioStreamBuilder_setChannelCount(builder, this->format_.channels);
                AAudioStreamBuilder_setSampleRate(builder, static_cast<int32_t>(this->format_.sample_rate));
                AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
                AAudioStreamBuilder_setDataCallback(builder, &aaudio_audio_output::data_callback, this);

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
                return true;
            }

            void submit(const void* data, const size_t size) override
            {
                if (!data || !size)
                {
                    return;
                }

                const auto bytes_per_sample = static_cast<size_t>(this->format_.bits_per_sample / 8);
                const auto* bytes = static_cast<const uint8_t*>(data);
                std::vector<float> converted(size / bytes_per_sample);
                for (size_t i = 0; i < converted.size(); ++i)
                {
                    converted[i] = to_float_sample(this->format_, bytes + (i * bytes_per_sample));
                }

                const std::lock_guard lock{this->mutex_};
                this->samples_.insert(this->samples_.end(), converted.begin(), converted.end());
            }

            std::optional<uint64_t> queued_bytes() const override
            {
                const std::lock_guard lock{this->mutex_};
                const auto queued_samples = this->samples_.size() - this->read_offset_;
                return static_cast<uint64_t>(queued_samples) * (this->format_.bits_per_sample / 8);
            }

          private:
            static aaudio_data_callback_result_t data_callback(AAudioStream* stream, void* user_data, void* audio_data,
                                                               const int32_t num_frames)
            {
                auto& self = *static_cast<aaudio_audio_output*>(user_data);
                const auto requested = static_cast<size_t>(num_frames) * static_cast<size_t>(AAudioStream_getChannelCount(stream));
                self.fill(static_cast<float*>(audio_data), requested);
                return AAUDIO_CALLBACK_RESULT_CONTINUE;
            }

            void fill(float* output, const size_t requested)
            {
                const std::lock_guard lock{this->mutex_};

                const auto available = this->samples_.size() - this->read_offset_;
                const auto count = std::min(requested, available);

                if (count)
                {
                    std::memcpy(output, this->samples_.data() + this->read_offset_, count * sizeof(float));
                }
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

            const audio_format format_{};
            mutable std::mutex mutex_{};
            AAudioStream* stream_{};
            std::vector<float> samples_{};
            size_t read_offset_{};
        };

        class aaudio_audio_backend final : public audio_backend
        {
          public:
            std::unique_ptr<audio_output> open_output(const audio_format& format) override
            {
                if (!is_supported_format(format))
                {
                    return nullptr;
                }

                auto output = std::make_unique<aaudio_audio_output>(format);
                if (!output->open())
                {
                    return nullptr;
                }
                return output;
            }
        };
    }

    std::unique_ptr<audio_backend> create_aaudio_audio_backend()
    {
        return std::make_unique<aaudio_audio_backend>();
    }

} // namespace sogen
