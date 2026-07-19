#pragma once
#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <functional>
#include <stdexcept>

#include "memory_permission.hpp"

namespace sogen
{

    using mmio_read_callback = std::function<void(uint64_t addr, void* data, size_t size)>;
    using mmio_write_callback = std::function<void(uint64_t addr, const void* data, size_t size)>;

    struct host_reserved_range
    {
        uint64_t address;
        size_t size;
    };

    class memory_manager;
    class linux_memory_manager;

    class memory_interface
    {
      public:
        friend memory_manager;
        friend linux_memory_manager;

        virtual ~memory_interface() = default;

        virtual void read_memory(uint64_t address, void* data, size_t size) const = 0;
        virtual bool try_read_memory(uint64_t address, void* data, size_t size) const = 0;
        virtual void write_memory(uint64_t address, const void* data, size_t size) = 0;
        virtual bool try_write_memory(uint64_t address, const void* data, size_t size) = 0;

      private:
        virtual void map_mmio(uint64_t address, size_t size, mmio_read_callback read_cb, mmio_write_callback write_cb) = 0;
        virtual void map_memory(uint64_t address, size_t size, memory_permission permissions) = 0;
        virtual void unmap_memory(uint64_t address, size_t size) = 0;

        virtual void map_host_memory(uint64_t /*address*/, size_t /*size*/, void* /*host_pointer*/, memory_permission /*permissions*/)
        {
            throw std::runtime_error("Host memory mapping is not supported by this backend");
        }

        virtual void apply_memory_protection(uint64_t address, size_t size, memory_permission permissions) = 0;

      public:
        virtual bool host_memory_aliasing_is_coherent() const
        {
            return true;
        }

        virtual void flush_host_memory_cache(const void* /*host_pointer*/, size_t /*size*/)
        {
        }

        // Ranges of the *host* process's own address space the guest address space must avoid, for
        // backends that share one address space with the guest (guest VA == host VA) rather than
        // sandboxing/translating it independently - e.g. the analyzer's own loaded image, dyld, and
        // shared libraries. Queried once, early, before any guest memory is allocated, so the memory
        // manager can pre-reserve these ranges. Backends with a fully independent guest address space
        // (the default) have nothing to report. This is a best-effort snapshot taken at that one point
        // in time - host allocations that happen afterwards are not covered.
        virtual std::vector<host_reserved_range> reserved_host_ranges() const
        {
            return {};
        }

        // Windowed form of reserved_host_ranges(), for callers that only need to know whether ONE
        // specific range has been claimed by a foreign host mapping (e.g. a fixed-address allocation
        // checking its exact target), rather than re-enumerating the whole address space. Returns
        // only the reserved sub-ranges intersecting [address, address + size). Default: fall back to
        // the full query, so backends that don't specialize keep identical behavior.
        virtual std::vector<host_reserved_range> reserved_host_ranges_in(uint64_t /*address*/, size_t /*size*/) const
        {
            return this->reserved_host_ranges();
        }

        // Called by the memory manager whenever it claims a guest address range - both when reserved
        // (MEM_RESERVE, not yet backed by any real mapping) and when committed - so backends sharing
        // the host address space with the guest (see reserved_host_ranges) can claim the same range at
        // the host OS level immediately. Without this, a reserved-but-uncommitted range is invisible to
        // the host allocator, so nothing stops an unconstrained host allocation (e.g. a JIT code buffer)
        // from landing there before the guest range is actually committed. No-op for backends with an
        // independent guest address space (the default).
        virtual void reserve_guest_address_range(uint64_t /*address*/, size_t /*size*/)
        {
        }

        // Counterpart to reserve_guest_address_range: called by the memory manager once a guest range
        // is genuinely released (freed) - not on a mere decommit, where the guest range stays reserved
        // and the host-level claim must persist. The caller guarantees [address, address + size)
        // contains no reserved guest ranges at call time (it passes the released range expanded to the
        // surrounding unreserved gap), so the backend may drop any host-level claim wholly inside it.
        // No-op for backends with an independent guest address space (the default).
        virtual void release_guest_address_range(uint64_t /*address*/, size_t /*size*/)
        {
        }

        template <typename T>
        T read_memory(const uint64_t address) const
        {
            T value{};
            this->read_memory(address, &value, sizeof(value));
            return value;
        }

        template <typename T>
        T read_memory(const void* address) const
        {
            return this->read_memory<T>(reinterpret_cast<uint64_t>(address));
        }

        std::vector<std::byte> read_memory(const uint64_t address, const size_t size) const
        {
            std::vector<std::byte> data{};
            data.resize(size);

            this->read_memory(address, data.data(), data.size());

            return data;
        }

        std::vector<std::byte> read_memory(const void* address, const size_t size) const
        {
            return this->read_memory(reinterpret_cast<uint64_t>(address), size);
        }

        template <typename T>
        void write_memory(const uint64_t address, const T& value)
        {
            this->write_memory(address, &value, sizeof(value));
        }

        template <typename T>
        void write_memory(void* address, const T& value)
        {
            this->write_memory(reinterpret_cast<uint64_t>(address), &value, sizeof(value));
        }

        void write_memory(void* address, const void* data, const size_t size)
        {
            this->write_memory(reinterpret_cast<uint64_t>(address), data, size);
        }

        void move_memory(uint64_t dst, uint64_t src, size_t size)
        {
            if (dst == src || !size)
            {
                return;
            }

            const auto copy_from_end = src < dst;
            const auto increment = copy_from_end ? -1 : 1;

            auto p_src = copy_from_end ? src + size - 1 : src;
            auto p_dst = copy_from_end ? dst + size - 1 : dst;

            while (size--)
            {
                const auto elem = this->read_memory<std::byte>(p_src);
                this->write_memory(p_dst, elem);
                p_src += increment;
                p_dst += increment;
            }
        }

        // Fill a guest range with a byte value (memset). Writes in bounded chunks so a large size never
        // materializes a matching host allocation.
        void set_memory(uint64_t address, uint8_t value, uint64_t size)
        {
            std::array<std::byte, 0x1000> buffer{};
            buffer.fill(static_cast<std::byte>(value));

            while (size > 0)
            {
                const auto count = static_cast<size_t>(std::min<uint64_t>(buffer.size(), size));
                this->write_memory(address, buffer.data(), count);
                address += count;
                size -= count;
            }
        }
    };

} // namespace sogen
