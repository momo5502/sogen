#include "std_include.hpp"
#include "host_range_reader.hpp"

#include <algorithm>
#include <fstream>
#include <unordered_map>

#include <platform/compiler.hpp>

#ifdef OS_EMSCRIPTEN
#include <emscripten/em_js.h>

// clang-format off
// The bodies below are JavaScript, not C++. clang-format parses them as C++ and rewrites !==
// into != = and === into == =, which compiles fine and emits glue that fails to parse at load
// time -- a break the build cannot see and only a run in a JS engine reports.
EM_JS_DEPS(sogen_host_range, "$UTF8ToString");

EM_JS(double, sogen_host_range_size, (double path_pointer), {
    if (typeof globalThis.sogenHostRangeSize !== "function")
    {
        return -1;
    }

    return globalThis.sogenHostRangeSize(UTF8ToString(Number(path_pointer)));
});

EM_JS(double, sogen_host_range_read, (double path_pointer, double offset, double size, double destination), {
    if (typeof globalThis.sogenHostRangeRead !== "function")
    {
        return -1;
    }

    const bytes = globalThis.sogenHostRangeRead(UTF8ToString(Number(path_pointer)), offset, size);
    if (!bytes || bytes.length === 0)
    {
        return 0;
    }

    HEAPU8.set(bytes, Number(destination));
    return bytes.length;
});
// clang-format on

#endif

namespace sogen
{
    namespace
    {
        class stream_range_reader : public host_range_reader
        {
          public:
            uint64_t file_size(const std::string& path) override
            {
                const auto* entry = this->entry_for(path);
                return entry ? entry->size : 0;
            }

            size_t read(const std::string& path, const uint64_t offset, const std::span<std::byte> destination) override
            {
                if (destination.empty())
                {
                    return 0;
                }

                auto* entry = this->entry_for(path);
                if (entry == nullptr)
                {
                    return 0;
                }

                // Redundant with the std::min below, which caps the length at the destination whatever
                // the subtraction produces, and deliberately kept for the reason sys_mmap keeps its own
                // copy: it states the bound while the offset is still the caller's raw value, before it
                // can underflow into something enormous. No test can tell the two layers apart.
                if (offset >= entry->size)
                {
                    return 0;
                }

                const auto available = std::min<uint64_t>(destination.size(), entry->size - offset);

                // A previous read that came up short left eofbit set, and seekg on a stream in a failed
                // state does nothing at all -- the read that followed would return bytes from wherever
                // the stream happened to be rather than from the requested range. Cleared here rather
                // than after the read, so the one call that matters sits where its absence shows.
                entry->stream.clear();
                entry->stream.seekg(static_cast<std::streamoff>(offset));
                entry->stream.read(reinterpret_cast<char*>(destination.data()), static_cast<std::streamsize>(available));

                return static_cast<size_t>(std::max<std::streamsize>(entry->stream.gcount(), 0));
            }

          private:
            struct open_file
            {
                std::ifstream stream;
                uint64_t size;
            };

            // Opened once and kept. The pager reads the same file thousands of times, and re-opening or
            // re-stat'ing per page-in would dominate the cost of a page-in. The size is cached with the
            // handle for the same reason; the shared cache does not change under a running process.
            open_file* entry_for(const std::string& path)
            {
                const auto existing = this->files_.find(path);
                if (existing != this->files_.end())
                {
                    return &existing->second;
                }

                std::error_code error{};
                const auto size = std::filesystem::file_size(std::filesystem::path{path}, error);
                if (error)
                {
                    return nullptr;
                }

                std::ifstream stream{path, std::ios::binary};
                if (!stream)
                {
                    return nullptr;
                }

                return &this->files_.emplace(path, open_file{.stream = std::move(stream), .size = size}).first->second;
            }

            std::unordered_map<std::string, open_file> files_{};
        };

#ifdef OS_EMSCRIPTEN
        // The bridge is installed by the page's worker and is absent in a plain node build, so a negative
        // answer means "no bridge here" and falls through to the filesystem. Zero does not: that is the
        // bridge saying the range holds nothing, which is a real answer and must not be second-guessed by
        // reading the same path off a filesystem the browser does not have.
        class bridged_range_reader : public host_range_reader
        {
          public:
            uint64_t file_size(const std::string& path) override
            {
                const auto size = sogen_host_range_size(pointer_to_double(path.c_str()));
                if (size < 0)
                {
                    return this->fallback_.file_size(path);
                }

                return static_cast<uint64_t>(size);
            }

            size_t read(const std::string& path, const uint64_t offset, const std::span<std::byte> destination) override
            {
                if (destination.empty())
                {
                    return 0;
                }

                const auto read_bytes =
                    sogen_host_range_read(pointer_to_double(path.c_str()), static_cast<double>(offset),
                                          static_cast<double>(destination.size()), pointer_to_double(destination.data()));

                if (read_bytes < 0)
                {
                    return this->fallback_.read(path, offset, destination);
                }

                return std::min(destination.size(), static_cast<size_t>(read_bytes));
            }

          private:
            static double pointer_to_double(const void* pointer)
            {
                return static_cast<double>(reinterpret_cast<uintptr_t>(pointer));
            }

            stream_range_reader fallback_{};
        };
#endif
    }

    host_range_reader& default_host_range_reader()
    {
#ifdef OS_EMSCRIPTEN
        static bridged_range_reader reader{};
#else
        static stream_range_reader reader{};
#endif
        return reader;
    }
}
