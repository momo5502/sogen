#pragma once

#include "std_include.hpp"
#include <serialization.hpp>

#include "arch_emulator.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <utils/nt_handle.hpp>
#include <utils/time.hpp>

namespace sogen
{

    struct process_context;
    struct emulator_settings;
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

    // A clock interrupt's next tick: one clock interval (100 ns units) after the last, or the first tick still ahead when
    // the last was handled later than that, since the hardware keeps its cadence while a handler runs late.
    std::chrono::steady_clock::time_point next_clock_tick(std::chrono::steady_clock::time_point last_tick,
                                                          std::chrono::steady_clock::time_point now, ULONG clock_interval);

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

        // Rewrites the page's time fields as KiUpdateTime does at every clock interrupt: once clock_interval (100 ns units)
        // has passed since the last rewrite or the clock went back, and right away without an interval. The vCPUs call it
        // under the kernel lock and the interrupt thread without it, while the vCPUs run; the second of two calls that
        // found the page due finds it no longer due under the KUSD's mutex. Readers retry a read that a rewrite straddled
        // (TimeUpdateLock, High1Time and High2Time).
        void refresh(ULONG clock_interval = 0);

        // Whether every guest read is trapped; refresh() leaves a trapped page alone.
        bool traps_reads() const
        {
            return this->intercept_;
        }

      private:
        memory_manager* memory_{};
        utils::clock* clock_{};

        bool registered_{};
        bool intercept_{};
        std::atomic<utils::clock::steady_time_point> last_refresh_{};

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
        bool refresh_due(ULONG clock_interval) const;
        void write_page();

        void update();

        void register_mmio();
        void deregister_mmio();
        void map_reference_tsc_page();
        native_tsc read_native_tsc() const;
    };

    // Whether every guest read of the KUSD traps. An unset KUSD setting keeps the page plain only where the interrupt thread
    // keeps its time (kusd_clock). Elsewhere every read is trapped, so it reads the current time.
    bool intercepts_kusd(const emulator_settings& settings, bool instruction_precision, guest_machine& emu);

    // The interrupt thread's wait for its next preemption, which stop() ends early. Where the thread keeps a plain KUSD's
    // time, the wait rewrites the page at every clock tick on the way, one clock interval after the last, as
    // KeClockInterruptNotify has KiUpdateTime do whatever the CPUs run, whether or not a vCPU rewrote the page since. It
    // takes no kernel lock for that: the KUSD's mutex keeps its rewrites apart from the vCPUs', which would otherwise queue
    // behind each of them.
    class kusd_clock
    {
      public:
        kusd_clock(kusd_mmio& kusd, bool instruction_precision, bool relative_time, guest_machine& emu);

        // Returns at until, or once stop() was called; until then it rewrites a plain KUSD whose time it keeps at every
        // clock tick, clock_interval (100 ns units) apart.
        void wait_until(std::chrono::steady_clock::time_point until, ULONG clock_interval);

        // Ends the current wait and every later one.
        void stop();

      private:
        kusd_mmio* kusd_{};
        bool keeps_time_{};
        std::chrono::steady_clock::time_point clock_tick_{};

        std::atomic_bool stopped_{false};
        std::mutex mutex_{};
        std::condition_variable condition_{};
#ifdef OS_WINDOWS
        utils::nt::handle<> timer_{};
        utils::nt::handle<> stop_event_{};
#endif

        void sleep_until(std::chrono::steady_clock::time_point wake);
    };

} // namespace sogen
