#include "std_include.hpp"
#include "host_file_mapping.hpp"

#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace sogen
{
    size_t host_mapping_granularity()
    {
#ifdef _WIN32
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        return info.dwAllocationGranularity;
#else
        const auto value = ::sysconf(_SC_PAGESIZE);
        return value > 0 ? static_cast<size_t>(value) : 0x1000;
#endif
    }

    host_file_mapping::~host_file_mapping()
    {
        this->release();
    }

    host_file_mapping::host_file_mapping(host_file_mapping&& other) noexcept
    {
        *this = std::move(other);
    }

    host_file_mapping& host_file_mapping::operator=(host_file_mapping&& other) noexcept
    {
        if (this != &other)
        {
            this->release();

            this->base_ = std::exchange(other.base_, nullptr);
            this->base_length_ = std::exchange(other.base_length_, 0);
            this->length_ = std::exchange(other.length_, 0);
#ifdef _WIN32
            this->file_handle_ = std::exchange(other.file_handle_, nullptr);
            this->mapping_handle_ = std::exchange(other.mapping_handle_, nullptr);
#endif
        }

        return *this;
    }

    void host_file_mapping::release()
    {
#ifdef _WIN32
        if (this->base_)
        {
            UnmapViewOfFile(this->base_);
        }

        if (this->mapping_handle_)
        {
            CloseHandle(this->mapping_handle_);
        }

        if (this->file_handle_ && this->file_handle_ != INVALID_HANDLE_VALUE)
        {
            CloseHandle(this->file_handle_);
        }

        this->mapping_handle_ = nullptr;
        this->file_handle_ = nullptr;
#elif !defined(__EMSCRIPTEN__)
        if (this->base_)
        {
            ::munmap(this->base_, this->base_length_);
        }
#endif

        this->base_ = nullptr;
        this->base_length_ = 0;
        this->length_ = 0;
    }

    std::optional<host_file_mapping> host_file_mapping::create(const std::filesystem::path& path, const uint64_t file_offset,
                                                               const size_t length)
    {
#ifdef __EMSCRIPTEN__
        // Emscripten has no mmap; its shim would allocate the whole range in linear memory, which is
        // exactly what this class exists to avoid. Stage 8 replaces this with a demand-fault page
        // provider. Callers fall back to copying.
        (void)path;
        (void)file_offset;
        (void)length;
        return std::nullopt;
#else
        const auto granularity = host_mapping_granularity();
        if (length == 0 || granularity == 0 || (file_offset % granularity) != 0)
        {
            return std::nullopt;
        }

        std::error_code error{};
        if (!std::filesystem::is_regular_file(path, error) || error)
        {
            return std::nullopt;
        }

        const auto file_size = std::filesystem::file_size(path, error);
        if (error || file_offset > file_size || length > file_size - file_offset)
        {
            return std::nullopt;
        }

        host_file_mapping mapping{};
        mapping.length_ = length;
        mapping.base_length_ = length;

#ifdef _WIN32
        auto* file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return std::nullopt;
        }

        mapping.file_handle_ = file;

        auto* section = CreateFileMappingW(file, nullptr, PAGE_WRITECOPY, 0, 0, nullptr);
        if (!section)
        {
            return std::nullopt;
        }

        mapping.mapping_handle_ = section;

        auto* view = MapViewOfFile(section, FILE_MAP_COPY, static_cast<DWORD>(file_offset >> 32),
                                   static_cast<DWORD>(file_offset & 0xFFFFFFFFull), length);
        if (!view)
        {
            return std::nullopt;
        }

        mapping.base_ = view;
#else
        const auto descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (descriptor < 0)
        {
            return std::nullopt;
        }

        // MAP_PRIVATE is a safety requirement, not a tuning choice: uc_mem_map_ptr aliases this buffer
        // into the guest with no interposition, so MAP_SHARED would let emulated code write through into
        // the user's real /usr/lib/dyld and dyld shared cache.
        auto* view = ::mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_PRIVATE, descriptor, static_cast<off_t>(file_offset));
        ::close(descriptor);

        if (view == MAP_FAILED)
        {
            return std::nullopt;
        }

        mapping.base_ = view;
#endif

        return mapping;
#endif
    }
}
