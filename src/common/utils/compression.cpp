#include "compression.hpp"

#include <zlib.h>
#include <array>
#include <cstdint>
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

        std::vector<std::byte> decompress(const std::span<const std::byte> data)
        {
            std::vector<std::byte> buffer{};
            zlib_stream stream_container{};
            if (!stream_container.is_valid())
            {
                return {};
            }

            static thread_local std::array<std::byte, ZCHUNK_SIZE> dest{};
            auto& stream = stream_container.get();

            stream.avail_in = static_cast<uInt>(data.size());
            stream.next_in = reinterpret_cast<const Bytef*>(data.data());

            do
            {
                stream.avail_out = static_cast<uInt>(dest.size());
                stream.next_out = reinterpret_cast<uint8_t*>(dest.data());

                const auto ret = inflate(&stream, Z_FINISH);
                if (ret != Z_OK && ret != Z_BUF_ERROR && ret != Z_STREAM_END)
                {
                    return {};
                }

                buffer.insert(buffer.end(), dest.data(), dest.data() + dest.size() - stream.avail_out);
            } while (stream.avail_out == 0);

            return buffer;
        }

        std::vector<std::byte> compress(const std::span<const std::byte> data)
        {
            std::vector<std::byte> result{};
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
