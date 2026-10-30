#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace sogen::utils
{
    // Page-aligned, zero-initialized host memory that a backend can map into the guest a second time. Backends
    // that alias host memory by duplicating its mapping (FEX on Linux, via mremap) need it to be a shared mapping,
    // which ordinary heap memory is not.
    class shared_host_buffer
    {
      public:
        shared_host_buffer() = default;
        explicit shared_host_buffer(size_t size);
        ~shared_host_buffer();

        // Memory another host process can map by its name. The name stays valid while any process holds the memory.
        static shared_host_buffer create_named(size_t size);
        static std::optional<shared_host_buffer> open_named(const std::string& name, size_t size);

        shared_host_buffer(const shared_host_buffer&) = delete;
        shared_host_buffer& operator=(const shared_host_buffer&) = delete;

        shared_host_buffer(shared_host_buffer&& other) noexcept;
        shared_host_buffer& operator=(shared_host_buffer&& other) noexcept;

        uint8_t* data() const
        {
            return this->data_;
        }

        size_t size() const
        {
            return this->size_;
        }

        // Empty unless the buffer can be opened by another process.
        const std::string& name() const
        {
            return this->name_;
        }

      private:
        size_t size_{};
        uint8_t* data_{};
        std::string name_{};
        void* mapping_{};

        void release();
    };
}
