#include "std_include.hpp"

#include "commpage.hpp"

#include <chrono>

namespace sogen
{
    namespace
    {
        constexpr uint64_t MICROSECONDS_PER_SECOND = 1000000;

        // mach_absolute_time() and mach_continuous_time() read CNTVCT_EL0 inline, so every value the
        // commpage publishes in that timescale has to come from the same register rather than from
        // CNTPCT_EL0.
        uint64_t read_virtual_counter(arm64_64_cpu& cpu)
        {
            return cpu.read_system_register(3, 3, 14, 0, 2);
        }

        // CNTFRQ_EL0 reports the emulated 62.5 MHz (QEMU's NANOSECONDS_PER_SECOND / GTIMER_SCALE), not
        // Apple's 24 MHz. It is only ever used to convert this emulator's own ticks into wall-clock
        // units; the guest never sees it.
        uint64_t read_counter_frequency(arm64_64_cpu& cpu)
        {
            return cpu.read_system_register(3, 3, 14, 0, 0);
        }

        uint64_t ticks_to_microseconds(const uint64_t ticks, const uint64_t frequency)
        {
            if (frequency == 0)
            {
                return 0;
            }

            return (ticks / frequency) * MICROSECONDS_PER_SECOND + (ticks % frequency) * MICROSECONDS_PER_SECOND / frequency;
        }

        uint64_t current_wall_clock_microseconds()
        {
            const auto now = std::chrono::system_clock::now().time_since_epoch();
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
        }

        // The counter has no epoch correspondence: it is the host's monotonic clock scaled by
        // GTIMER_SCALE, so its zero is an arbitrary instant. The wall-clock anchor is therefore
        // calibrated once against the counter and never re-derived from it.
        uint64_t calibrate_boottime_microseconds(const uint64_t counter, const uint64_t frequency)
        {
            const auto wall_clock = current_wall_clock_microseconds();
            const auto uptime = ticks_to_microseconds(counter, frequency);

            return uptime > wall_clock ? 0 : wall_clock - uptime;
        }

        template <typename T>
        void write_field(macos_memory_manager& memory, const uint64_t address, const T& value)
        {
            static_assert(std::is_trivially_copyable_v<T>, "Commpage fields are raw bytes in guest memory");
            memory.write_memory(address, &value, sizeof(value));
        }

        void write_signature(macos_memory_manager& memory, const uint64_t base, const std::string& signature)
        {
            std::array<char, 16> field{};
            const auto length = std::min(signature.size(), field.size() - 1);
            std::memcpy(field.data(), signature.data(), length);

            memory.write_memory(base + commpage_offset::SIGNATURE, field.data(), field.size());
        }

        void write_machine_fields(macos_memory_manager& memory, const uint64_t base, const macos_system_info& info)
        {
            write_signature(memory, base, info.signature);

            write_field(memory, base + commpage_offset::CPU_CAPABILITIES64, info.cpu_capabilities64);
            write_field(memory, base + commpage_offset::VERSION, info.version);
            write_field(memory, base + commpage_offset::CPU_CAPABILITIES, info.cpu_capabilities32);
            write_field(memory, base + commpage_offset::NCPUS, info.ncpus);
            write_field(memory, base + commpage_offset::CACHE_LINESIZE, info.cache_linesize);
            write_field(memory, base + commpage_offset::CPU_CLUSTERS, info.cpu_clusters);
            write_field(memory, base + commpage_offset::ACTIVE_CPUS, info.active_cpus);
            write_field(memory, base + commpage_offset::PHYSICAL_CPUS, info.physical_cpus);
            write_field(memory, base + commpage_offset::LOGICAL_CPUS, info.logical_cpus);
            write_field(memory, base + commpage_offset::MEMORY_SIZE, info.memory_size);
            write_field(memory, base + commpage_offset::CPUFAMILY, info.cpufamily);
        }

        void write_readonly_fields(macos_memory_manager& memory, const uint64_t base, const macos_system_info& info)
        {
            write_field(memory, base + commpage_offset::RO_USER_PAGE_SHIFT_32, info.user_page_shift);
            write_field(memory, base + commpage_offset::RO_USER_PAGE_SHIFT_64, info.user_page_shift);
            write_field(memory, base + commpage_offset::RO_KERNEL_PAGE_SHIFT, info.kernel_page_shift);
        }

        // TIMEBASE_OFFSET, CONT_TIMEBASE and CONT_HW_TIMEBASE are addends libSystem applies to the
        // counter it just read, not snapshots of it: mach_continuous_time() returns
        // CONT_HW_TIMEBASE + CNTVCT_EL0. The emulated machine never sleeps, so all three stay zero.
        void write_time_fields(macos_memory_manager& memory, const uint64_t base, arm64_64_cpu& cpu)
        {
            const auto frequency = read_counter_frequency(cpu);
            const auto counter = read_virtual_counter(cpu);

            write_field(memory, base + commpage_offset::TIMEBASE_OFFSET, uint64_t{0});
            write_field(memory, base + commpage_offset::CONT_TIMEBASE, uint64_t{0});
            write_field(memory, base + commpage_offset::CONT_HW_TIMEBASE, uint64_t{0});
            write_field(memory, base + commpage_offset::USER_TIMEBASE, static_cast<uint8_t>(macos_user_timebase::spec));
            write_field(memory, base + commpage_offset::CONT_HWCLOCK, uint8_t{1});
            write_field(memory, base + commpage_offset::APPROX_TIME_SUPPORTED, uint8_t{1});
            write_field(memory, base + commpage_offset::BOOTTIME_USEC, calibrate_boottime_microseconds(counter, frequency));
            write_field(memory, base + commpage_offset::APPROX_TIME, counter);
        }
    }

    void macos_commpage::setup(macos_memory_manager& memory, arm64_64_cpu& cpu, const macos_system_info& info)
    {
        if (this->mapped_)
        {
            return;
        }

        if (!memory.allocate_reserved_region(MACOS_COMMPAGE_BASE, MACOS_COMMPAGE_MAP_SIZE, memory_permission::read) ||
            !memory.allocate_reserved_region(MACOS_COMMPAGE_RO_BASE, MACOS_COMMPAGE_MAP_SIZE, memory_permission::read))
        {
            return;
        }

        memory.set_memory(MACOS_COMMPAGE_BASE, 0, MACOS_COMMPAGE_MAP_SIZE);
        memory.set_memory(MACOS_COMMPAGE_RO_BASE, 0, MACOS_COMMPAGE_MAP_SIZE);

        write_machine_fields(memory, MACOS_COMMPAGE_BASE, info);
        write_readonly_fields(memory, MACOS_COMMPAGE_RO_BASE, info);
        write_time_fields(memory, MACOS_COMMPAGE_BASE, cpu);

        this->base_address_ = MACOS_COMMPAGE_BASE;
        this->readonly_base_address_ = MACOS_COMMPAGE_RO_BASE;
        this->mapped_ = true;
    }

    void macos_commpage::update_time(macos_memory_manager& memory, arm64_64_cpu& cpu) const
    {
        if (!this->mapped_)
        {
            return;
        }

        write_field(memory, this->base_address_ + commpage_offset::APPROX_TIME, read_virtual_counter(cpu));
    }

    void macos_commpage::serialize(utils::buffer_serializer& buffer) const
    {
        buffer.write(this->base_address_);
        buffer.write(this->readonly_base_address_);
        buffer.write(this->mapped_);
    }

    void macos_commpage::deserialize(utils::buffer_deserializer& buffer)
    {
        buffer.read(this->base_address_);
        buffer.read(this->readonly_base_address_);
        buffer.read(this->mapped_);
    }
}
