#include "shared_host_buffer.hpp"

#include <limits>
#include <new>
#include <utility>

#ifdef _WIN32
#include "win.hpp"
#elif defined(__EMSCRIPTEN__)
#include <cstdlib>
#include <cstring>
#else
#include <sys/mman.h>
#endif

namespace sogen::utils
{
    namespace
    {
        constexpr size_t page_size = 0x1000;

        size_t round_up_to_page(const size_t size)
        {
            if (size == 0 || size > std::numeric_limits<size_t>::max() - (page_size - 1))
            {
                throw std::bad_alloc();
            }

            return (size + page_size - 1) & ~(page_size - 1);
        }

        uint8_t* allocate(const size_t size)
        {
#ifdef _WIN32
            return static_cast<uint8_t*>(::VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#elif defined(__EMSCRIPTEN__)
            auto* data = static_cast<uint8_t*>(std::aligned_alloc(page_size, size));
            if (data)
            {
                std::memset(data, 0, size);
            }
            return data;
#else
            void* data = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
            return data == MAP_FAILED ? nullptr : static_cast<uint8_t*>(data);
#endif
        }

        void free(uint8_t* data, const size_t size)
        {
#ifdef _WIN32
            (void)size;
            ::VirtualFree(data, 0, MEM_RELEASE);
#elif defined(__EMSCRIPTEN__)
            (void)size;
            std::free(data);
#else
            ::munmap(data, size);
#endif
        }
    }

    shared_host_buffer::shared_host_buffer(const size_t size)
        : size_(round_up_to_page(size)),
          data_(allocate(this->size_))
    {
        if (!this->data_)
        {
            throw std::bad_alloc();
        }
    }

    shared_host_buffer::~shared_host_buffer()
    {
        this->release();
    }

    shared_host_buffer::shared_host_buffer(shared_host_buffer&& other) noexcept
        : size_(std::exchange(other.size_, 0)),
          data_(std::exchange(other.data_, nullptr))
    {
    }

    shared_host_buffer& shared_host_buffer::operator=(shared_host_buffer&& other) noexcept
    {
        if (this != &other)
        {
            this->release();
            this->data_ = std::exchange(other.data_, nullptr);
            this->size_ = std::exchange(other.size_, 0);
        }

        return *this;
    }

    void shared_host_buffer::release()
    {
        if (this->data_)
        {
            free(this->data_, this->size_);
            this->data_ = nullptr;
            this->size_ = 0;
        }
    }
}
