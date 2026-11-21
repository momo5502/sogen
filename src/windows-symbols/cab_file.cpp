#include "cab_file.hpp"

#include <utils/finally.hpp>
#include <utils/string.hpp>

#include <mspack.h>

#include <cstddef>
#include <type_traits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>

namespace sogen::symbols
{
    namespace
    {
        struct cabinet_stream
        {
            mspack_file handle{};
            FILE* stream{};
        };

        struct cabinet_system
        {
            mspack_system api{};
            const std::filesystem::path* input{};
            const std::filesystem::path* output{};
        };

        static_assert(std::is_standard_layout_v<cabinet_stream> && offsetof(cabinet_stream, handle) == 0);
        static_assert(std::is_standard_layout_v<cabinet_system> && offsetof(cabinet_system, api) == 0);

        mspack_file* open_cabinet_stream(mspack_system* system, const char* filename, const int mode) noexcept
        {
            const auto& context = *reinterpret_cast<cabinet_system*>(system);
            const auto reading = mode == MSPACK_SYS_OPEN_READ && std::strcmp(filename, "input") == 0;
            const auto writing = mode == MSPACK_SYS_OPEN_WRITE && std::strcmp(filename, "output") == 0;

            if (!reading && !writing)
            {
                return nullptr;
            }

            const auto& path = *(reading ? context.input : context.output);
#ifdef _WIN32
            auto* stream = _wfopen(path.c_str(), reading ? L"rb" : L"wb");
#else
            auto* stream = std::fopen(path.c_str(), reading ? "rb" : "wb");
#endif
            if (!stream)
            {
                return nullptr;
            }

            auto* handle = new (std::nothrow) cabinet_stream{.stream = stream};
            if (!handle)
            {
                std::fclose(stream);
            }

            return handle ? &handle->handle : nullptr;
        }

        FILE* file_stream(mspack_file* file)
        {
            return reinterpret_cast<cabinet_stream*>(file)->stream;
        }

        // NOLINTNEXTLINE(cert-dcl50-cpp,modernize-avoid-variadic-functions): libmspack requires a C variadic message callback.
        void ignore_cabinet_message(mspack_file*, const char*, ...)
        {
        }

        cabinet_system make_cabinet_system(const std::filesystem::path& input, const std::filesystem::path& output)
        {
            cabinet_system system{};
            system.input = &input;
            system.output = &output;
            system.api.open = open_cabinet_stream;
            system.api.close = [](mspack_file* file) {
                std::fclose(file_stream(file));
                delete reinterpret_cast<cabinet_stream*>(file);
            };
            system.api.read = [](mspack_file* file, void* buffer, const int size) {
                if (size < 0)
                {
                    return -1;
                }
                const auto count = std::fread(buffer, 1, static_cast<size_t>(size), file_stream(file));
                return std::ferror(file_stream(file)) ? -1 : static_cast<int>(count);
            };
            system.api.write = [](mspack_file* file, void* buffer, const int size) {
                if (size < 0)
                {
                    return -1;
                }
                return static_cast<int>(std::fwrite(buffer, 1, static_cast<size_t>(size), file_stream(file)));
            };
            system.api.seek = [](mspack_file* file, const off_t offset, const int mode) {
                int origin{};
                switch (mode)
                {
                case MSPACK_SYS_SEEK_START:
                    origin = SEEK_SET;
                    break;
                case MSPACK_SYS_SEEK_CUR:
                    origin = SEEK_CUR;
                    break;
                case MSPACK_SYS_SEEK_END:
                    origin = SEEK_END;
                    break;
                default:
                    return -1;
                }
#ifdef _WIN32
                return _fseeki64(file_stream(file), offset, origin);
#else
                return fseeko(file_stream(file), offset, origin);
#endif
            };
            system.api.tell = [](mspack_file* file) -> off_t {
#ifdef _WIN32
                const auto offset = _ftelli64(file_stream(file));
                return offset > std::numeric_limits<off_t>::max() ? -1 : static_cast<off_t>(offset);
#else
                return ftello(file_stream(file));
#endif
            };
            system.api.message = ignore_cabinet_message;
            system.api.alloc = [](mspack_system*, const size_t size) { return std::malloc(size); };
            system.api.free = std::free;
            system.api.copy = [](void* source, void* destination, const size_t size) { std::memcpy(destination, source, size); };
            return system;
        }
    }

    void extract_cab_file(const std::filesystem::path& cabinet_path, const std::filesystem::path& output_path,
                          const std::string_view filename)
    {
        int self_test{};
        MSPACK_SYS_SELFTEST(self_test);
        if (self_test != MSPACK_ERR_OK)
        {
            throw std::runtime_error("CAB decoder offset ABI mismatch");
        }

        auto system = make_cabinet_system(cabinet_path, output_path);
        auto* decoder = mspack_create_cab_decompressor(&system.api);
        if (!decoder)
        {
            throw std::runtime_error("Failed to create CAB decoder");
        }

        const auto destroy_decoder = utils::finally([&] { mspack_destroy_cab_decompressor(decoder); });

        auto* cabinet = decoder->open(decoder, "input");
        if (!cabinet)
        {
            throw std::runtime_error("Invalid CAB artifact");
        }

        const auto close_cabinet = utils::finally([&] { decoder->close(decoder, cabinet); });

        mscabd_file* selected{};
        for (auto* file = cabinet->files; file; file = file->next)
        {
            if (utils::string::equals_ignore_case(std::string_view{file->filename}, filename))
            {
                if (selected)
                {
                    throw std::runtime_error("Ambiguous CAB member");
                }
                selected = file;
            }
        }

        if (!selected)
        {
            throw std::runtime_error("CAB does not contain the requested PDB");
        }

        try
        {
            if (decoder->extract(decoder, selected, "output") != MSPACK_ERR_OK)
            {
                throw std::runtime_error("Failed to decompress CAB artifact");
            }
        }
        catch (...)
        {
            std::error_code ec{};
            std::filesystem::remove(output_path, ec);
            throw;
        }
    }
}
