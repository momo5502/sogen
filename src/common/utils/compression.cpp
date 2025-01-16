#include "compression.hpp"

#include <zlib.h>
#include <cstring>

namespace utils::compression
{
    namespace zlib
    {
        namespace
        {
            class zlib_stream
            {
              public:
                zlib_stream()
                {
                    memset(&stream_, 0, sizeof(stream_));
                    valid_ = inflateInit(&stream_) == Z_OK;
                }

                zlib_stream(zlib_stream&&) = delete;
                zlib_stream(const zlib_stream&) = delete;
                zlib_stream& operator=(zlib_stream&&) = delete;
                zlib_stream& operator=(const zlib_stream&) = delete;

                ~zlib_stream()
                {
                    if (valid_)
                    {
                        inflateEnd(&stream_);
                    }
                }

                z_stream& get()
                {
                    return stream_; //
                }

                bool is_valid() const
                {
                    return valid_;
                }

              private:
                bool valid_{false};
                z_stream stream_{};
            };
        }

        std::vector<std::uint8_t> decompress(const std::vector<std::uint8_t>& data)
        {
            std::vector<std::uint8_t> buffer{};
            zlib_stream stream_container{};
            if (!stream_container.is_valid())
            {
                return {};
            }

            int ret{};
            size_t offset = 0;
            static thread_local uint8_t dest[ZCHUNK_SIZE] = {0};
            auto& stream = stream_container.get();

            do
            {
                const auto input_size = std::min(sizeof(dest), data.size() - offset);
                stream.avail_in = static_cast<uInt>(input_size);
                stream.next_in = reinterpret_cast<const Bytef*>(data.data()) + offset;
                offset += stream.avail_in;

                do
                {
                    stream.avail_out = sizeof(dest);
                    stream.next_out = dest;

                    ret = inflate(&stream, Z_NO_FLUSH);
                    if (ret != Z_OK && ret != Z_STREAM_END)
                    {
                        return {};
                    }

                    buffer.insert(buffer.end(), dest, dest + sizeof(dest) - stream.avail_out);
                } while (stream.avail_out == 0);
            } while (ret != Z_STREAM_END);

            return buffer;
        }

        std::vector<std::uint8_t> compress(const std::vector<std::uint8_t>& data)
        {
            std::vector<std::uint8_t> result{};
            auto length = compressBound(static_cast<uLong>(data.size()));
            result.resize(length);

            if (compress2(reinterpret_cast<Bytef*>(result.data()), &length, reinterpret_cast<const Bytef*>(data.data()),
                          static_cast<uLong>(data.size()), Z_BEST_COMPRESSION) != Z_OK)
            {
                return {};
            }

            result.resize(length);
            return result;
        }
    }
}