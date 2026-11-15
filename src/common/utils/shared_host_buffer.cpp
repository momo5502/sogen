#include "shared_host_buffer.hpp"
#include "host_process.hpp"

#include <atomic>
#include <limits>
#include <mutex>
#include <new>
#include <random>
#include <string>
#include <thread>
#include <utility>

// A named buffer needs POSIX shared memory. Emscripten has no second process to share one with, and bionic has no
// shm_open at all, so there creating one falls back to an unnamed buffer and opening one finds nothing.
#if defined(_WIN32) || (!defined(__EMSCRIPTEN__) && !defined(__ANDROID__))
#define SOGEN_NAMED_SHARED_MEMORY 1
#else
#define SOGEN_NAMED_SHARED_MEMORY 0
#endif

#ifdef _WIN32
#include "win.hpp"
#elif defined(__EMSCRIPTEN__)
#include <cstdlib>
#include <cstring>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
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

#if !defined(_WIN32) && SOGEN_NAMED_SHARED_MEMORY
        // POSIX shared memory names outlive their mappings until unlinked, so the last process holding the buffer
        // unlinks its name. Every holding process has a slot in a header page after the data with its host process id
        // and how many mappings it holds. A process that is killed or crashes never gives up its slot or the header
        // lock, so a holder whose process is gone counts as having left. Joining and the final unlink both happen under
        // the header lock, and the unlink marks the buffer closing, so a process that opened the name just before
        // cannot join a buffer whose name is already gone.
        constexpr size_t holder_header_size = page_size;
        constexpr size_t max_holders = 64;

        class holder_table
        {
          public:
            holder_table(uint8_t* data, const size_t size)
                : words_(reinterpret_cast<uint32_t*>(data + size))
            {
            }

            void lock()
            {
                const auto self = current_host_process_id();
                auto owner_word = this->word(lock_index);
                for (;;)
                {
                    auto owner = owner_word.load();
                    if ((owner == 0 || (owner != self && !host_process_alive(owner))) && owner_word.compare_exchange_weak(owner, self))
                    {
                        return;
                    }

                    std::this_thread::yield();
                }
            }

            void unlock()
            {
                this->word(lock_index).store(0);
            }

            bool is_closing()
            {
                return this->word(closing_index).load() != 0;
            }

            void mark_closing()
            {
                this->word(closing_index).store(1);
            }

            bool has_live_holder()
            {
                for (size_t slot = 0; slot < max_holders; ++slot)
                {
                    const auto holder = this->holder(slot).load();
                    if (holder != 0 && host_process_alive(holder))
                    {
                        return true;
                    }
                }

                return false;
            }

            bool join(const uint32_t self)
            {
                std::optional<size_t> free_slot{};
                for (size_t slot = 0; slot < max_holders; ++slot)
                {
                    const auto holder = this->holder(slot).load();
                    if (holder == self)
                    {
                        this->mappings(slot).fetch_add(1);
                        return true;
                    }

                    if (!free_slot && (holder == 0 || !host_process_alive(holder)))
                    {
                        free_slot = slot;
                    }
                }

                if (!free_slot)
                {
                    return false;
                }

                this->holder(*free_slot).store(self);
                this->mappings(*free_slot).store(1);
                return true;
            }

            void leave(const uint32_t self)
            {
                for (size_t slot = 0; slot < max_holders; ++slot)
                {
                    if (this->holder(slot).load() == self)
                    {
                        if (this->mappings(slot).fetch_sub(1) == 1)
                        {
                            this->holder(slot).store(0);
                        }
                        return;
                    }
                }
            }

          private:
            static constexpr size_t lock_index = 0;
            static constexpr size_t closing_index = 1;
            static constexpr size_t first_slot_index = 2;
            static_assert((first_slot_index + 2 * max_holders) * sizeof(uint32_t) <= holder_header_size);

            uint32_t* words_{};

            std::atomic_ref<uint32_t> word(const size_t index)
            {
                return std::atomic_ref<uint32_t>{this->words_[index]};
            }

            std::atomic_ref<uint32_t> holder(const size_t slot)
            {
                return this->word(first_slot_index + 2 * slot);
            }

            std::atomic_ref<uint32_t> mappings(const size_t slot)
            {
                return this->word(first_slot_index + 2 * slot + 1);
            }
        };

        uint8_t* map_shared_memory(const int fd, const size_t size)
        {
            void* data = ::mmap(nullptr, size + holder_header_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            return data == MAP_FAILED ? nullptr : static_cast<uint8_t*>(data);
        }
#endif

#if SOGEN_NAMED_SHARED_MEMORY
        std::string to_hex(uint64_t value, const size_t digits)
        {
            std::string result(digits, '0');
            for (size_t i = digits; i > 0 && value != 0; --i, value >>= 4)
            {
                result[i - 1] = "0123456789abcdef"[value & 0xF];
            }

            return result;
        }

        // A child outlives its parent, so the host can give the parent's process id to an unrelated new sogen process
        // while the child still holds a mapping named after it. A random part keeps those names apart, and anyone else
        // can create such a name in advance, which is why creating one checks that it did not exist and retries.
        std::string make_unique_name()
        {
            static std::atomic<uint64_t> counter{};
            std::random_device device{};
            const auto salt = (static_cast<uint64_t>(device()) << 32) | device();

#ifdef _WIN32
            const auto process_id = static_cast<uint64_t>(::GetCurrentProcessId());
            return "Local\\sogen-" + to_hex(process_id, 8) + "-" + to_hex(counter++, 4) + "-" + to_hex(salt, 16);
#else
            // macOS limits shared memory names to 31 characters.
            const auto process_id = static_cast<uint64_t>(::getpid());
            return "/sogen." + to_hex(salt ^ process_id, 12) + "." + to_hex(counter++, 4);
#endif
        }

        constexpr size_t name_attempts = 16;
#endif
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

    shared_host_buffer shared_host_buffer::create_named(const size_t size)
    {
#if !SOGEN_NAMED_SHARED_MEMORY
        return shared_host_buffer{size};
#else
        shared_host_buffer buffer{};
        buffer.size_ = round_up_to_page(size);

#ifdef _WIN32
        const auto size64 = static_cast<uint64_t>(buffer.size_);

        for (size_t attempt = 0; attempt < name_attempts && !buffer.data_; ++attempt)
        {
            buffer.name_ = make_unique_name();
            const auto wide_name = std::wstring(buffer.name_.begin(), buffer.name_.end());
            auto* mapping = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, static_cast<DWORD>(size64 >> 32),
                                                 static_cast<DWORD>(size64), wide_name.c_str());
            if (!mapping)
            {
                throw std::bad_alloc();
            }

            // CreateFileMappingW opens an existing mapping of the same name and says so only through this error,
            // which would make two unrelated emulations share pages.
            if (::GetLastError() == ERROR_ALREADY_EXISTS)
            {
                ::CloseHandle(mapping);
                continue;
            }

            buffer.mapping_ = mapping;
            buffer.data_ = static_cast<uint8_t*>(::MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, buffer.size_));
        }
#else
        for (size_t attempt = 0; attempt < name_attempts && !buffer.data_; ++attempt)
        {
            buffer.name_ = make_unique_name();
            const auto fd = ::shm_open(buffer.name_.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
            if (fd < 0)
            {
                if (errno == EEXIST)
                {
                    continue;
                }

                throw std::bad_alloc();
            }

            if (::ftruncate(fd, static_cast<off_t>(buffer.size_) + static_cast<off_t>(holder_header_size)) == 0)
            {
                buffer.data_ = map_shared_memory(fd, buffer.size_);
            }
            ::close(fd);

            if (!buffer.data_)
            {
                ::shm_unlink(buffer.name_.c_str());
                throw std::bad_alloc();
            }

            holder_table holders{buffer.data_, buffer.size_};
            const std::scoped_lock lock{holders};
            if (!holders.join(current_host_process_id()))
            {
                throw std::bad_alloc();
            }
        }
#endif

        if (!buffer.data_)
        {
            throw std::bad_alloc();
        }

        return buffer;
#endif
    }

    std::optional<shared_host_buffer> shared_host_buffer::open_named(const std::string& name, const size_t size)
    {
#if !SOGEN_NAMED_SHARED_MEMORY
        (void)name;
        (void)size;
        return std::nullopt;
#else
        shared_host_buffer buffer{};
        buffer.size_ = round_up_to_page(size);
        buffer.name_ = name;

#ifdef _WIN32
        const auto wide_name = std::wstring(name.begin(), name.end());
        auto* mapping = ::OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, wide_name.c_str());
        if (!mapping)
        {
            return std::nullopt;
        }

        buffer.mapping_ = mapping;
        buffer.data_ = static_cast<uint8_t*>(::MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, buffer.size_));
#else
        const auto fd = ::shm_open(name.c_str(), O_RDWR, 0600);
        if (fd < 0)
        {
            return std::nullopt;
        }

        buffer.data_ = map_shared_memory(fd, buffer.size_);
        ::close(fd);
        if (!buffer.data_)
        {
            return std::nullopt;
        }

        holder_table holders{buffer.data_, buffer.size_};
        bool joined = false;
        {
            const std::scoped_lock lock{holders};
            if (!holders.is_closing() && !holders.has_live_holder())
            {
                // Every holder died without releasing it, so nobody is left to unlink the name.
                holders.mark_closing();
                ::shm_unlink(name.c_str());
            }

            joined = !holders.is_closing() && holders.join(current_host_process_id());
        }

        if (!joined)
        {
            ::munmap(buffer.data_, buffer.size_ + holder_header_size);
            buffer.data_ = nullptr;
            return std::nullopt;
        }
#endif

        if (!buffer.data_)
        {
            return std::nullopt;
        }

        return buffer;
#endif
    }

    shared_host_buffer::~shared_host_buffer()
    {
        this->release();
    }

    shared_host_buffer::shared_host_buffer(shared_host_buffer&& other) noexcept
        : size_(std::exchange(other.size_, 0)),
          data_(std::exchange(other.data_, nullptr)),
          name_(std::move(other.name_)),
          mapping_(std::exchange(other.mapping_, nullptr))
    {
    }

    shared_host_buffer& shared_host_buffer::operator=(shared_host_buffer&& other) noexcept
    {
        if (this != &other)
        {
            this->release();
            this->data_ = std::exchange(other.data_, nullptr);
            this->size_ = std::exchange(other.size_, 0);
            this->name_ = std::move(other.name_);
            this->mapping_ = std::exchange(other.mapping_, nullptr);
        }

        return *this;
    }

    void shared_host_buffer::release()
    {
        if (this->name_.empty())
        {
            if (this->data_)
            {
                free(this->data_, this->size_);
            }
        }
        else
        {
#ifdef _WIN32
            if (this->data_)
            {
                ::UnmapViewOfFile(this->data_);
            }
            if (this->mapping_)
            {
                ::CloseHandle(this->mapping_);
            }
#elif SOGEN_NAMED_SHARED_MEMORY
            if (this->data_)
            {
                holder_table holders{this->data_, this->size_};
                {
                    const std::scoped_lock lock{holders};
                    holders.leave(current_host_process_id());
                    if (!holders.has_live_holder())
                    {
                        holders.mark_closing();
                        ::shm_unlink(this->name_.c_str());
                    }
                }
                ::munmap(this->data_, this->size_ + holder_header_size);
            }
#endif
        }

        this->data_ = nullptr;
        this->size_ = 0;
        this->name_.clear();
        this->mapping_ = nullptr;
    }
}
