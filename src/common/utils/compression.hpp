#pragma once

#include <functional>
#include <memory>
#include <span>
#include <vector>

struct ZSTD_outBuffer_s;

namespace sogen
{

    namespace utils::compression
    {
        namespace zstd
        {
            std::vector<std::byte> compress(std::span<const std::byte> data, int compression_level = 8);
            std::vector<std::byte> decompress(std::span<const std::byte> data);
            // For frames written by a stream_compressor, which do not record their decompressed size.
            std::vector<std::byte> decompress(std::span<const std::byte> data, size_t decompressed_size);

            class stream_compressor
            {
              public:
                using sink = std::function<void(std::span<const std::byte> compressed)>;

                stream_compressor(sink output, int compression_level, unsigned workers);
                ~stream_compressor();

                stream_compressor(const stream_compressor&) = delete;
                stream_compressor& operator=(const stream_compressor&) = delete;
                stream_compressor(stream_compressor&&) = delete;
                stream_compressor& operator=(stream_compressor&&) = delete;

                void write(std::span<const std::byte> data);
                void finish();

              private:
                struct context_deleter
                {
                    void operator()(void* context) const;
                };

                sink output_;
                std::unique_ptr<void, context_deleter> context_;
                std::vector<std::byte> out_buffer_;

                void compress(std::span<const std::byte> data, bool end);
            };

            class stream_decompressor
            {
              public:
                // Reads up to the size of the span from the compressed data and returns how much it read, 0 at its end.
                using source = std::function<size_t(std::span<std::byte> compressed)>;

                explicit stream_decompressor(source input);
                ~stream_decompressor();

                stream_decompressor(const stream_decompressor&) = delete;
                stream_decompressor& operator=(const stream_decompressor&) = delete;
                stream_decompressor(stream_decompressor&&) = delete;
                stream_decompressor& operator=(stream_decompressor&&) = delete;

                void read(std::span<std::byte> data);
                void finish();

              private:
                struct context_deleter
                {
                    void operator()(void* context) const;
                };

                source input_;
                std::unique_ptr<void, context_deleter> context_;
                std::vector<std::byte> in_buffer_;
                size_t in_position_{};
                size_t in_size_{};
                bool frame_complete_{};

                bool fill_input();
                void decompress(ZSTD_outBuffer_s& output);
            };
        }
    }
} // namespace sogen
