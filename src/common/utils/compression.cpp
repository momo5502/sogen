#include "compression.hpp"

#include <zstd.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <thread>

namespace sogen
{

    namespace utils::compression
    {
        namespace zstd
        {
            std::vector<std::byte> decompress(const std::span<const std::byte> data)
            {
                const auto decompressed_size = ZSTD_getFrameContentSize(data.data(), data.size());

                if (decompressed_size == ZSTD_CONTENTSIZE_ERROR || decompressed_size == ZSTD_CONTENTSIZE_UNKNOWN)
                {
                    return {};
                }

                std::vector<std::byte> buffer(static_cast<size_t>(decompressed_size));

                const auto result = ZSTD_decompress(buffer.data(), buffer.size(), data.data(), data.size());

                if (ZSTD_isError(result))
                {
                    return {};
                }

                return buffer;
            }

            std::vector<std::byte> decompress(const std::span<const std::byte> data, const size_t decompressed_size)
            {
                const std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> context{ZSTD_createDCtx(), &ZSTD_freeDCtx};
                if (!context)
                {
                    return {};
                }

                std::vector<std::byte> buffer(decompressed_size);
                ZSTD_inBuffer input{data.data(), data.size(), 0};
                ZSTD_outBuffer output{buffer.data(), buffer.size(), 0};
                size_t result = 1;
                while (result != 0 && output.pos < output.size && input.pos < input.size)
                {
                    result = ZSTD_decompressStream(context.get(), &output, &input);
                    if (ZSTD_isError(result))
                    {
                        return {};
                    }
                }

                // A streamed frame can still end with an empty block once all of its data has been produced.
                std::byte extra{};
                while (result != 0 && input.pos < input.size)
                {
                    ZSTD_outBuffer scratch{&extra, sizeof(extra), 0};
                    result = ZSTD_decompressStream(context.get(), &scratch, &input);
                    if (ZSTD_isError(result) || scratch.pos != 0)
                    {
                        return {};
                    }
                }

                if (result != 0 || input.pos != input.size || output.pos != buffer.size())
                {
                    return {};
                }

                return buffer;
            }

            void stream_compressor::context_deleter::operator()(void* context) const
            {
                ZSTD_freeCCtx(static_cast<ZSTD_CCtx*>(context));
            }

            stream_compressor::stream_compressor(sink output, const int compression_level, const unsigned workers)
                : output_(std::move(output)),
                  context_(ZSTD_createCCtx()),
                  out_buffer_(ZSTD_CStreamOutSize())
            {
                auto* context = static_cast<ZSTD_CCtx*>(this->context_.get());
                if (!context || ZSTD_isError(ZSTD_CCtx_setParameter(context, ZSTD_c_compressionLevel, compression_level)))
                {
                    throw std::runtime_error("Failed to create a zstd stream");
                }

                // Builds without multithreading reject workers and compress on the calling thread instead.
                (void)ZSTD_CCtx_setParameter(context, ZSTD_c_nbWorkers, static_cast<int>(workers));
            }

            stream_compressor::~stream_compressor() = default;

            void stream_compressor::write(const std::span<const std::byte> data)
            {
                this->compress(data, false);
            }

            void stream_compressor::finish()
            {
                this->compress({}, true);
            }

            void stream_compressor::compress(const std::span<const std::byte> data, const bool end)
            {
                auto* context = static_cast<ZSTD_CCtx*>(this->context_.get());
                ZSTD_inBuffer input{data.data(), data.size(), 0};
                const auto directive = end ? ZSTD_e_end : ZSTD_e_continue;

                while (true)
                {
                    ZSTD_outBuffer output{this->out_buffer_.data(), this->out_buffer_.size(), 0};
                    const auto remaining = ZSTD_compressStream2(context, &output, &input, directive);
                    if (ZSTD_isError(remaining))
                    {
                        throw std::runtime_error(std::string{"zstd compression failed: "} + ZSTD_getErrorName(remaining));
                    }

                    if (output.pos > 0)
                    {
                        this->output_(std::span(this->out_buffer_).first(output.pos));
                    }

                    if (end ? remaining == 0 : input.pos == input.size)
                    {
                        return;
                    }
                }
            }

            void stream_decompressor::context_deleter::operator()(void* context) const
            {
                ZSTD_freeDCtx(static_cast<ZSTD_DCtx*>(context));
            }

            stream_decompressor::stream_decompressor(source input)
                : input_(std::move(input)),
                  context_(ZSTD_createDCtx()),
                  in_buffer_(ZSTD_DStreamInSize())
            {
                if (!this->context_)
                {
                    throw std::runtime_error("Failed to create a zstd stream");
                }
            }

            stream_decompressor::~stream_decompressor() = default;

            void stream_decompressor::read(const std::span<std::byte> data)
            {
                ZSTD_outBuffer output{data.data(), data.size(), 0};
                while (output.pos < output.size)
                {
                    if (this->frame_complete_)
                    {
                        throw std::runtime_error("The compressed data ended early");
                    }

                    this->decompress(output);
                }
            }

            void stream_decompressor::finish()
            {
                std::byte extra{};
                while (!this->frame_complete_)
                {
                    ZSTD_outBuffer output{&extra, sizeof(extra), 0};
                    this->decompress(output);
                    if (output.pos != 0)
                    {
                        throw std::runtime_error("The compressed data holds more than expected");
                    }
                }

                if (this->fill_input())
                {
                    throw std::runtime_error("The compressed data continues after its frame");
                }
            }

            bool stream_decompressor::fill_input()
            {
                if (this->in_position_ == this->in_size_)
                {
                    this->in_size_ = this->input_(this->in_buffer_);
                    this->in_position_ = 0;
                }

                return this->in_position_ < this->in_size_;
            }

            void stream_decompressor::decompress(ZSTD_outBuffer& output)
            {
                if (!this->fill_input())
                {
                    throw std::runtime_error("The compressed data ended early");
                }

                ZSTD_inBuffer input{this->in_buffer_.data(), this->in_size_, this->in_position_};
                const auto result = ZSTD_decompressStream(static_cast<ZSTD_DCtx*>(this->context_.get()), &output, &input);
                this->in_position_ = input.pos;
                if (ZSTD_isError(result))
                {
                    throw std::runtime_error(std::string{"zstd decompression failed: "} + ZSTD_getErrorName(result));
                }

                this->frame_complete_ = result == 0;
            }

            std::vector<std::byte> compress(const std::span<const std::byte> data, const int compression_level)
            {
                const auto max_size = ZSTD_compressBound(data.size());
                std::vector<std::byte> result(max_size);

                const std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)> context{ZSTD_createCCtx(), &ZSTD_freeCCtx};
                if (!context || ZSTD_isError(ZSTD_CCtx_setParameter(context.get(), ZSTD_c_compressionLevel, compression_level)))
                {
                    return {};
                }

                // Builds without multithreading reject workers and compress on the calling thread instead.
                (void)ZSTD_CCtx_setParameter(context.get(), ZSTD_c_nbWorkers, static_cast<int>(std::thread::hardware_concurrency()));

                const auto compressed_size = ZSTD_compress2(context.get(), result.data(), max_size, data.data(), data.size());
                if (ZSTD_isError(compressed_size))
                {
                    return {};
                }

                result.resize(compressed_size);
                return result;
            }
        }
    }
} // namespace sogen
