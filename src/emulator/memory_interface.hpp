#pragma once
#include "interface.hpp"

#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <functional>
#include <limits>
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

    class EMULATOR_INTERFACE memory_interface
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

        // Returns false if this backend cannot alias that particular host memory into the guest.
        virtual bool map_host_memory(uint64_t /*address*/, size_t /*size*/, void* /*host_pointer*/, memory_permission /*permissions*/)
        {
            throw std::runtime_error("Host memory mapping is not supported by this backend");
        }

        virtual bool supports_host_memory_mapping() const
        {
            return false;
        }

        virtual bool host_memory_mapping_requires_identity() const
        {
            return false;
        }

        virtual void apply_memory_protection(uint64_t address, size_t size, memory_permission permissions) = 0;

      public:
        // For backends whose guest memory is host memory at fixed host addresses: the host address backing address,
        // which is where host memory has to be placed to appear there in the guest. 0 for backends that can map host
        // memory to any guest address.
        virtual uint64_t host_placement_address(uint64_t /*address*/) const
        {
            return 0;
        }

        virtual bool host_memory_aliasing_is_coherent() const
        {
            return true;
        }

        virtual void flush_host_memory_cache(const void* /*host_pointer*/, size_t /*size*/)
        {
        }

        // For backends that can notice accesses by protecting memory: tracks [address, address + size) until it is unmapped,
        // for take_written_ranges and invalidate_cache_on_access. Returns false if this backend cannot.
        virtual bool track_access(uint64_t /*address*/, size_t /*size*/)
        {
            return false;
        }

        // Calls f for each part of tracked memory in [address, address + size) written since tracking began or since the
        // last call, and tracks writes to those parts again.
        virtual void take_written_ranges(uint64_t /*address*/, size_t /*size*/,
                                         const std::function<void(uint64_t written_address, size_t written_size)>& /*f*/)
        {
        }

        // For tracked memory whose CPU cache lines may be stale, e.g. after a non-coherent device wrote it: the next access
        // to each part of [address, address + size) first cleans and invalidates its cache lines.
        virtual void invalidate_cache_on_access(uint64_t /*address*/, size_t /*size*/)
        {
        }

        // Ranges of the host process's own address space (its image, dyld, shared libraries) that the
        // guest must avoid, for backends where guest VA == host VA. Best-effort snapshot: host
        // allocations made after the query are not covered. Backends with an independent guest
        // address space have nothing to report.
        virtual std::vector<host_reserved_range> reserved_host_ranges() const
        {
            return {};
        }

        // reserved_host_ranges() restricted to [address, address + size), for callers checking one
        // specific target instead of re-enumerating the whole address space.
        virtual std::vector<host_reserved_range> reserved_host_ranges_in(uint64_t /*address*/, size_t /*size*/) const
        {
            return this->reserved_host_ranges();
        }

        // The end of the guest addresses the backend can map. Backends sharing the address space with the
        // guest are bound by the host's user address space, which can be far smaller than Windows' (512 GB
        // on Android kernels built with 39-bit virtual addresses).
        virtual uint64_t guest_address_end() const
        {
            return std::numeric_limits<uint64_t>::max();
        }

        // Called whenever the memory manager claims a guest range, including a bare MEM_RESERVE that is
        // not backed by a real mapping yet. Backends sharing the address space with the guest (see
        // reserved_host_ranges) must claim it at the host OS level here, otherwise an unconstrained
        // host allocation (e.g. a JIT code buffer) can land inside a reserved-but-uncommitted range.
        virtual void reserve_guest_address_range(uint64_t /*address*/, size_t /*size*/)
        {
        }

        // A range every process maps at the same guest address, such as KUSER_SHARED_DATA. A snapshot may keep
        // memory there, since restoring one maps the range like every other start does.
        virtual void register_fixed_guest_range(uint64_t /*address*/, size_t /*size*/)
        {
        }

        // Counterpart to reserve_guest_address_range, called once a guest range is genuinely freed -
        // not on a decommit, where the range stays reserved and the host claim must persist. The
        // caller expands the freed range to the surrounding unreserved gap, so the backend may drop
        // any host claim wholly inside it.
        virtual void release_guest_address_range(uint64_t /*address*/, uint64_t /*size*/)
        {
        }

        template <typename T>
        T read_memory(const uint64_t address) const
        {
            static_assert(std::is_trivially_copyable_v<T>, "Type must be trivially copyable!");
            // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
            T value{};
            this->read_memory(address, &value, sizeof(value));
            return value;
        }

        template <typename T>
        T read_memory(const uint64_t address, const size_t size) const
        {
            static_assert(std::is_trivially_copyable_v<T>, "Type must be trivially copyable!");
            T value{};
            this->read_memory(address, &value, std::min(size, sizeof(T)));
            return value;
        }

        template <typename T>
        T read_memory(const void* address) const
        {
            static_assert(std::is_trivially_copyable_v<T>, "Type must be trivially copyable!");
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
            static_assert(std::is_trivially_copyable_v<T>, "Type must be trivially copyable!");
            this->write_memory(address, &value, sizeof(value));
        }

        template <typename T>
        void write_memory(void* address, const T& value)
        {
            static_assert(std::is_trivially_copyable_v<T>, "Type must be trivially copyable!");
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
