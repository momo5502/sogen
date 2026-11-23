#include "std_include.hpp"

namespace sogen::syscalls
{
    NTSTATUS handle_NtSetTimerResolution(const syscall_context& c, ULONG desired_resolution, BOOLEAN set_resolution,
                                         emulator_object<ULONG> current_resolution);
}

namespace sogen::test
{
    namespace
    {
        // The guest's time, which moves only when a test moves it.
        class kusd_test_clock : public utils::clock
        {
          public:
            system_time_point system_now() override
            {
                return std::chrono::time_point_cast<system_duration>(system_time_point(std::chrono::seconds(1'700'000'000)) +
                                                                     this->elapsed);
            }

            steady_time_point steady_now() override
            {
                return steady_time_point{} + this->elapsed;
            }

            std::chrono::nanoseconds elapsed{std::chrono::hours(1)};
        };

        // A process whose KUSD is the plain page the vCPUs refresh.
        windows_emulator create_plain_kusd_emulator(kusd_test_clock*& clock)
        {
            emulator_settings settings{};
            settings.use_relative_time = false;
            settings.intercept_kusd = false;

            auto owned = std::make_unique<kusd_test_clock>();
            clock = owned.get();
            emulator_interfaces interfaces{};
            interfaces.clock = std::move(owned);

            return create_sample_emulator(settings, {}, {}, std::move(interfaces));
        }

        // KUSD InterruptTime as the guest reads it, in 100 ns units.
        uint64_t interrupt_time(windows_emulator& emu)
        {
            return emu.memory.read_memory<uint64_t>(kusd_mmio::address() + offsetof(KUSER_SHARED_DATA64, InterruptTime));
        }

        void set_timer_resolution(windows_emulator& emu, const ULONG resolution)
        {
            const syscall_context c{.win_emu = emu, .emu = emu.vcpu(0).cpu, .vcpu = emu.vcpu(0), .proc = emu.process};
            EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtSetTimerResolution(c, resolution, TRUE, {emu.memory}));
        }
    }

    // KiUpdateTime makes TimeUpdateLock odd while it rewrites the time and even again after, so a reader whose read straddled
    // a rewrite, such as RtlGetSystemTimePrecise around its NtQueryPerformanceCounter call, sees it change and reads again.
    TEST(KusdRefreshTest, EveryRewriteMovesTheTimeUpdateLockOnByTwo)
    {
        kusd_test_clock* clock{};
        auto emu = create_plain_kusd_emulator(clock);
        emu.setup_process_if_necessary();
        auto& vcpu = emu.vcpu(0);
        const auto time_update_lock = [&] {
            return emu.memory.read_memory<ULONGLONG>(kusd_mmio::address() + offsetof(KUSER_SHARED_DATA64, TimeUpdateLock));
        };

        ASSERT_TRUE(emu.perform_thread_switch(vcpu));
        const auto before = time_update_lock();
        EXPECT_EQ(before % 2, 0u);

        clock->elapsed += std::chrono::milliseconds(1);
        ASSERT_TRUE(emu.perform_thread_switch(vcpu));
        EXPECT_EQ(time_update_lock(), before + 2);

        ASSERT_TRUE(emu.perform_thread_switch(vcpu));
        EXPECT_EQ(time_update_lock(), before + 2) << "no rewrite inside a clock interval";
    }

    // A relative clock counts the instructions the vCPUs execute, so no interrupt thread can keep a plain page's time while
    // they run: an unset KUSD setting traps every read on every backend, and each read returns the relative time.
    TEST(KusdRefreshTest, AnUnsetSettingTrapsThePageUnderRelativeTime)
    {
        emulator_settings settings{};
        settings.use_relative_time = true;
        settings.use_instruction_precision = false;
        settings.intercept_kusd = std::nullopt;
        EXPECT_TRUE(create_sample_emulator(settings).process.kusd.traps_reads());

        settings.intercept_kusd = false;
        EXPECT_FALSE(create_sample_emulator(settings).process.kusd.traps_reads()) << "an explicit setting still decides";
    }

    // KiUpdateTime rewrites the KUSD's time at every clock interrupt: 1 ms apart, or finer while a timer resolution request
    // asks for it. A vCPU does it for the plain page when it schedules a thread, once a clock interval has passed.
    TEST(KusdRefreshTest, SchedulingAThreadUpdatesTheTimeEveryClockInterval)
    {
        kusd_test_clock* clock{};
        auto emu = create_plain_kusd_emulator(clock);
        emu.setup_process_if_necessary();
        auto& vcpu = emu.vcpu(0);

        ASSERT_TRUE(emu.perform_thread_switch(vcpu));
        const auto start = interrupt_time(emu);

        clock->elapsed += std::chrono::microseconds(900);
        ASSERT_TRUE(emu.perform_thread_switch(vcpu));
        EXPECT_EQ(interrupt_time(emu), start);

        clock->elapsed += std::chrono::microseconds(100);
        ASSERT_TRUE(emu.perform_thread_switch(vcpu));
        EXPECT_EQ(interrupt_time(emu), start + 10'000);

        set_timer_resolution(emu, 5'000);
        clock->elapsed += std::chrono::microseconds(500);
        ASSERT_TRUE(emu.perform_thread_switch(vcpu));
        EXPECT_EQ(interrupt_time(emu), start + 15'000);
    }

    // A thread that runs without scheduling sees the time move at its system calls.
    TEST(KusdRefreshTest, SystemCallsUpdateTheTimeEveryClockInterval)
    {
        kusd_test_clock* clock{};
        auto emu = create_plain_kusd_emulator(clock);
        emu.setup_process_if_necessary();

        // NtQueryTimerResolution without outputs: a system call that changes nothing else.
        constexpr uint32_t syscall_id = 0x42;
        std::array<std::byte, 8> stub{std::byte{0x4C}, std::byte{0x8B}, std::byte{0xD1}, std::byte{0xB8}};
        std::memcpy(stub.data() + 4, &syscall_id, sizeof(syscall_id));
        emu.dispatcher.setup({exported_symbol{.name = "NtQueryTimerResolution"}}, stub, {}, {});

        auto& cpu = emu.vcpu(0).x64_cpu();
        const auto system_call = [&] {
            cpu.reg<uint64_t>(x86_register::rax, syscall_id);
            cpu.reg<uint64_t>(x86_register::r10, 0);
            cpu.reg<uint64_t>(x86_register::rdx, 0);
            cpu.reg<uint64_t>(x86_register::r8, 0);
            emu.dispatch_on_cpu(emu.vcpu(0).cpu, [&] { emu.dispatcher.dispatch(emu, emu.vcpu(0)); });
            EXPECT_EQ(cpu.reg<uint64_t>(x86_register::rax), static_cast<uint64_t>(STATUS_SUCCESS));
        };

        system_call();
        const auto start = interrupt_time(emu);

        clock->elapsed += std::chrono::microseconds(900);
        system_call();
        EXPECT_EQ(interrupt_time(emu), start);

        clock->elapsed += std::chrono::microseconds(100);
        system_call();
        EXPECT_EQ(interrupt_time(emu), start + 10'000);
    }

    TEST(KusdRefreshTest, ClockTicksKeepTheCadenceOfTheClockInterval)
    {
        constexpr ULONG one_millisecond = 10'000;
        constexpr std::chrono::steady_clock::time_point tick{};
        const auto next_tick_after = [&](const int64_t wake_microseconds) {
            const auto next = next_clock_tick(tick, tick + std::chrono::microseconds(wake_microseconds), one_millisecond);
            return std::chrono::duration_cast<std::chrono::microseconds>(next - tick).count();
        };

        EXPECT_EQ(1'000, next_tick_after(0));
        EXPECT_EQ(1'000, next_tick_after(700));
        EXPECT_EQ(2'000, next_tick_after(1'000));
        EXPECT_EQ(3'000, next_tick_after(2'500));
    }
}
