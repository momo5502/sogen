#pragma once

#include "std_include.hpp"
#include <serialization.hpp>

#include "arch_emulator.hpp"

#include <mutex>
#include <utils/time.hpp>

namespace sogen
{

    struct process_context;
    struct fake_environment_config;
    class windows_emulator;
    class windows_version_manager;

    // The simulated physical memory, about 48 GB, which the KUSD and every memory query report.
    constexpr uint32_t simulated_physical_page_count = 0x00bf0958;

    // Describes an ARM64 machine the way Windows on ARM does: ARM64 image numbers and architecture, no
    // XState, and processor features derived from the CPU's identification registers.
    void apply_arm64_processor(KUSER_SHARED_DATA64& kusd, uint64_t isar0, uint64_t isar1);

    // TscScale of the Hyper-V reference TSC page for a TSC running at tsc_frequency Hz: floor(10^7 * 2^64 / tsc_frequency).
    uint64_t reference_tsc_scale(uint64_t tsc_frequency);

    // Reference time in 100 ns units for a TSC value, as Hyper-V defines it: (tsc * tsc_scale) >> 64, before the page's TscOffset.
    uint64_t reference_time(uint64_t tsc, uint64_t tsc_scale);

    // A TSC the guest reads natively: vCPU 0's value now and the rate the host's TSC runs at.
    struct native_tsc
    {
        uint64_t value{};
        uint64_t frequency{};
    };

    class kusd_mmio
    {
      public:
        // Trapping every guest read keeps the times exact; otherwise the page is plain memory that refresh() updates.
        // read_tsc reads the guest's TSC while the guest reads it natively, which a snapshot of the reference TSC page needs.
        kusd_mmio(memory_manager& memory, utils::clock& clock, bool intercept, std::function<std::optional<native_tsc>()> read_tsc = {});
        ~kusd_mmio();

        kusd_mmio(kusd_mmio&&) = delete;
        kusd_mmio(const kusd_mmio&) = delete;
        kusd_mmio& operator=(kusd_mmio&& obj) = delete;
        kusd_mmio& operator=(const kusd_mmio&) = delete;

        void serialize(utils::buffer_serializer& buffer) const;
        void deserialize(utils::buffer_deserializer& buffer);

        // Locked access to the KUSD block: the MMIO read callback mutates kusd_ under mutex_ on the WHP
        // worker thread, so callers must not touch it unsynchronized. The functor's result is forwarded.
        template <typename F>
        decltype(auto) access(const F& functor)
        {
            const std::lock_guard lock{this->mutex_};
            return functor(this->kusd_);
        }

        template <typename F>
        decltype(auto) access(const F& functor) const
        {
            const std::lock_guard lock{this->mutex_};
            return functor(this->kusd_);
        }

        static uint64_t address();

        // Given the rate of a TSC the guest reads natively, QueryPerformanceCounter runs in user mode as under Hyper-V: ntdll
        // reads a reference TSC page mapped after the KUSD instead of making a syscall.
        void setup(const windows_version_manager& version, const fake_environment_config& fake_env, std::optional<uint64_t> tsc_frequency);

        // The reference TSC page's address, which SystemHypervisorSharedPageInformation reports; empty without one.
        std::optional<uint64_t> reference_tsc_page() const;

        // QueryPerformanceCounter for a guest TSC value, as ntdll computes it from the reference TSC page.
        uint64_t query_performance_counter(uint64_t tsc) const;

        void refresh();

      private:
        memory_manager* memory_{};
        utils::clock* clock_{};

        bool registered_{};
        bool intercept_{};

        std::function<std::optional<native_tsc>()> read_tsc_{};

        // TscScale of the reference TSC page; 0 while QueryPerformanceCounter goes through the syscall.
        uint64_t reference_tsc_scale_{};
        // TscSequence and TscOffset, which a restore changes so the reference time goes on from the saved one.
        uint32_t reference_tsc_sequence_{};
        int64_t reference_tsc_offset_{};

        // The MMIO read callback runs on the WHP worker thread during guest execution
        // (outside the kernel lock), so multiple vCPUs can read KUSD concurrently. This
        // guards update() + the read snapshot so kusd_ is never torn.
        mutable std::mutex mutex_{};

        // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
        KUSER_SHARED_DATA64 kusd_{};

        void read(uint64_t addr, void* data, size_t size);
        void write_page();

        void update();

        void register_mmio();
        void deregister_mmio();
        void map_reference_tsc_page();
        native_tsc read_native_tsc() const;
    };

} // namespace sogen
