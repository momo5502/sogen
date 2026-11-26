#include "std_include.hpp"

namespace sogen::syscalls
{
    NTSTATUS handle_NtQueryPerformanceCounter(const syscall_context& c, emulator_object<LARGE_INTEGER> performance_counter,
                                              emulator_object<LARGE_INTEGER> performance_frequency);
}

namespace sogen::test
{
    // TscScale read from the reference TSC page of a Hyper-V root whose TSC runs at 3,494,402,315 Hz.
    constexpr uint64_t measured_tsc_frequency = 3'494'402'315;
    constexpr uint64_t measured_tsc_scale = 0x00bb8bb097078acf;

    constexpr auto steady_frequency = static_cast<uint64_t>(utils::clock::steady_duration::period::den);

    namespace
    {
        class manual_clock : public utils::clock
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

            uint64_t now() const
            {
                return static_cast<uint64_t>(this->elapsed.count());
            }

            std::chrono::nanoseconds elapsed{std::chrono::hours(1)};
        };

        uint32_t page_sequence(windows_emulator& emu, const uint64_t page)
        {
            return emulator_object<uint32_t>{emu.memory, page}.read();
        }

        std::optional<uint64_t> page_query_performance_counter(windows_emulator& emu, kusd_mmio& kusd, const uint64_t tsc)
        {
            const auto page = kusd.reference_tsc_page().value();
            if (!page_sequence(emu, page))
            {
                return std::nullopt;
            }

            const auto scale = emulator_object<uint64_t>{emu.memory, page + 8}.read();
            const auto offset = emulator_object<uint64_t>{emu.memory, page + 16}.read();
            const auto bias = kusd.access([](const KUSER_SHARED_DATA64& kusd_data) { return kusd_data.QpcBias; });
            return reference_time(tsc, scale) + offset + bias;
        }

        uint64_t qpc_frequency(kusd_mmio& kusd)
        {
            return static_cast<uint64_t>(kusd.access([](const KUSER_SHARED_DATA64& kusd_data) { return kusd_data.QpcFrequency; }));
        }
    }

    TEST(ReferenceTscTest, ScaleMatchesHyperV)
    {
        EXPECT_EQ(reference_tsc_scale(measured_tsc_frequency), measured_tsc_scale);
    }

    TEST(ReferenceTscTest, TimeCountsHundredNanosecondUnits)
    {
        // The scale is rounded down, so whole seconds of TSC ticks land one unit short.
        EXPECT_EQ(reference_time(measured_tsc_frequency, measured_tsc_scale), 9'999'999u);
        EXPECT_EQ(reference_time(measured_tsc_frequency * 3600, measured_tsc_scale), 35'999'999'999u);
        EXPECT_EQ(reference_time(UINT64_MAX, measured_tsc_scale), measured_tsc_scale - 1);
    }

    TEST(ReferenceTscTest, ScaleCountsTheUnitsItIsGiven)
    {
        const auto scale = reference_tsc_scale(measured_tsc_frequency, steady_frequency);
        EXPECT_EQ(reference_time(measured_tsc_frequency, scale), steady_frequency - 1);
        EXPECT_EQ(reference_time(measured_tsc_frequency * 3600, scale), 3600 * steady_frequency - 1);
    }

    TEST(ReferenceTscTest, SteadyTimePageReadsTheSteadyClockFromItsFirstSyscall)
    {
        auto emu = create_empty_emulator();
        manual_clock clock{};
        native_tsc tsc{.value = 5 * measured_tsc_frequency, .frequency = measured_tsc_frequency};
        kusd_mmio kusd{emu.memory, clock, false, [&] { return std::optional{tsc}; }};
        kusd.setup(emu.version, emu.fake_env, tsc.frequency, true);

        const auto page = kusd.reference_tsc_page();
        ASSERT_TRUE(page);
        EXPECT_EQ(qpc_frequency(kusd), steady_frequency);
        EXPECT_EQ(emulator_object<uint64_t>(emu.memory, *page + 8).read(), reference_tsc_scale(tsc.frequency, steady_frequency));
        EXPECT_FALSE(page_query_performance_counter(emu, kusd, tsc.value)) << "without a sequence ntdll makes the syscall";

        EXPECT_EQ(kusd.query_performance_counter(tsc.value), clock.now());
        EXPECT_EQ(page_sequence(emu, *page), 1u);
        EXPECT_EQ(page_query_performance_counter(emu, kusd, tsc.value), clock.now());

        for (const auto seconds : {1u, 3600u})
        {
            tsc.value += seconds * tsc.frequency;
            clock.elapsed += std::chrono::seconds(seconds);
            const auto on_page = page_query_performance_counter(emu, kusd, tsc.value).value();
            EXPECT_EQ(kusd.query_performance_counter(tsc.value), on_page);
            EXPECT_NEAR(static_cast<double>(on_page), static_cast<double>(clock.now()), 1.0);
        }
    }

    TEST(ReferenceTscTest, SteadyTimePageFollowsTheSteadyClockAcrossAHeldGuest)
    {
        auto emu = create_empty_emulator();
        manual_clock clock{};
        native_tsc tsc{.value = 5 * measured_tsc_frequency, .frequency = measured_tsc_frequency};
        kusd_mmio kusd{emu.memory, clock, false, [&] { return std::optional{tsc}; }};
        kusd.setup(emu.version, emu.fake_env, tsc.frequency, true);
        const auto page = kusd.reference_tsc_page().value();

        const auto held_at = kusd.query_performance_counter(tsc.value);
        tsc.value += 30 * tsc.frequency;
        kusd.follow_steady_clock();

        EXPECT_FALSE(page_query_performance_counter(emu, kusd, tsc.value)) << "without a sequence ntdll makes the syscall";
        EXPECT_EQ(kusd.query_performance_counter(tsc.value), held_at);
        EXPECT_EQ(page_sequence(emu, page), 2u);
        EXPECT_EQ(page_query_performance_counter(emu, kusd, tsc.value), held_at);

        tsc.value += tsc.frequency;
        clock.elapsed += std::chrono::seconds(1);
        EXPECT_NEAR(static_cast<double>(page_query_performance_counter(emu, kusd, tsc.value).value()), static_cast<double>(clock.now()),
                    1.0);
    }

    TEST(ReferenceTscTest, SteadyTimePageFollowsTheSteadyClockAcrossARestore)
    {
        auto emu = create_empty_emulator();
        manual_clock clock{};
        native_tsc tsc{.value = 0, .frequency = 3'500'000'000};
        const auto read_tsc = [&] { return std::optional{tsc}; };

        utils::buffer_serializer serializer{};
        uint32_t saved_sequence{};
        {
            kusd_mmio saving{emu.memory, clock, false, read_tsc};
            saving.setup(emu.version, emu.fake_env, tsc.frequency, true);
            tsc.value = 7 * tsc.frequency;
            (void)saving.query_performance_counter(tsc.value);
            saved_sequence = page_sequence(emu, saving.reference_tsc_page().value());
            saving.serialize(serializer);
        }

        tsc = {.value = 1'000'000, .frequency = 2'500'000'000};
        clock.elapsed += std::chrono::seconds(5);
        kusd_mmio restored{emu.memory, clock, false, read_tsc};
        utils::buffer_deserializer deserializer{serializer};
        restored.deserialize(deserializer);

        const auto page = restored.reference_tsc_page();
        ASSERT_TRUE(page);
        EXPECT_EQ(qpc_frequency(restored), steady_frequency);
        EXPECT_EQ(emulator_object<uint64_t>(emu.memory, *page + 8).read(), reference_tsc_scale(tsc.frequency, steady_frequency));
        EXPECT_FALSE(page_query_performance_counter(emu, restored, tsc.value)) << "without a sequence ntdll makes the syscall";

        EXPECT_EQ(restored.query_performance_counter(tsc.value), clock.now());
        EXPECT_GT(page_sequence(emu, *page), saved_sequence);

        tsc.value += tsc.frequency;
        clock.elapsed += std::chrono::seconds(1);
        EXPECT_NEAR(static_cast<double>(page_query_performance_counter(emu, restored, tsc.value).value()), static_cast<double>(clock.now()),
                    1.0);
    }

    TEST(ReferenceTscTest, InterceptedTimingReadsTheSteadyClockFromThePage)
    {
        emulator_settings settings{};
        settings.use_relative_time = false;
        settings.keep_guest_time_across_snapshots = true;
        auto emu = create_sample_emulator(settings);
        if (!emu.native_timestamp_counter_frequency() || !emu.intercepts_timestamp_counter())
        {
            GTEST_SKIP() << "The guest does not read its own TSC through intercepted RDTSC exits on this backend";
        }

        emu.start();
        const auto page = emu.process.kusd.reference_tsc_page();
        ASSERT_TRUE(page);
        EXPECT_EQ(qpc_frequency(emu.process.kusd), steady_frequency);

        const auto counter = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        const auto syscall_query_performance_counter = [&] {
            const syscall_context c{.win_emu = emu, .emu = emu.vcpu(0).cpu, .vcpu = emu.vcpu(0), .proc = emu.process};
            EXPECT_EQ(STATUS_SUCCESS,
                      syscalls::handle_NtQueryPerformanceCounter(c, {emu.memory, counter}, {emu.memory, counter + sizeof(LARGE_INTEGER)}));
            EXPECT_EQ(emu.memory.read_memory<uint64_t>(counter + sizeof(LARGE_INTEGER)), steady_frequency);
            return emu.memory.read_memory<uint64_t>(counter);
        };
        const auto page_now = [&] {
            return page_query_performance_counter(emu, emu.process.kusd, emu.vcpu(0).cpu.read_native_timestamp_counter().value());
        };

        const auto first = syscall_query_performance_counter();
        const auto on_page = page_now();
        ASSERT_TRUE(on_page) << "a syscall anchors the page";
        EXPECT_GE(*on_page, first);

        emu.save_snapshot();
        emu.restore_snapshot();
        EXPECT_FALSE(page_now()) << "after the restore ntdll makes the syscall until it anchors the page again";

        const auto restored = syscall_query_performance_counter();
        EXPECT_GE(restored, *on_page);
        const auto anchored_again = page_now();
        ASSERT_TRUE(anchored_again) << "the syscall anchors the page again";
        EXPECT_GE(*anchored_again, restored);
    }

    // ntdll keeps reading the page it got at process start, so a snapshot keeps the page and a restore continues it: the
    // restoring partition's TSC starts over, so the TscOffset moves and the sequence changes, as under Hyper-V.
    TEST(ReferenceTscTest, RestoreContinuesQueryPerformanceCounterOnThePage)
    {
        uint64_t page{};
        const auto sequence = [&](windows_emulator& e) { return emulator_object<uint32_t>{e.memory, page}.read(); };
        const auto scale = [&](windows_emulator& e) { return emulator_object<uint64_t>{e.memory, page + 8}.read(); };
        const auto query_performance_counter = [&](windows_emulator& e) {
            // ntdll: ((rdtsc * TscScale) >> 64) + TscOffset + QpcBias.
            const auto offset = emulator_object<uint64_t>{e.memory, page + 16}.read();
            const auto bias = e.process.kusd.access([](const KUSER_SHARED_DATA64& kusd) { return kusd.QpcBias; });
            return reference_time(e.vcpu(0).cpu.read_native_timestamp_counter().value(), scale(e)) + offset + bias;
        };

        utils::buffer_serializer serializer{};
        uint64_t saved_scale{};
        uint32_t saved_sequence{};
        uint64_t saved_counter{};

        // Saved, then gone before the restore, as a snapshot is loaded by another run.
        {
            auto emu = create_sample_emulator(emulator_settings{.intercept_rdtsc = false});
            if (!emu.native_timestamp_counter_frequency())
            {
                GTEST_SKIP() << "The guest does not read its TSC natively on this backend";
            }

            emu.start();
            page = emu.process.kusd.reference_tsc_page().value_or(0);
            ASSERT_NE(page, 0u);

            saved_scale = scale(emu);
            saved_sequence = sequence(emu);
            saved_counter = query_performance_counter(emu);
            emu.serialize(serializer);
        }

        auto restored = create_emulator(emulator_settings{.intercept_rdtsc = false});
        utils::buffer_deserializer deserializer{serializer};
        restored.deserialize(deserializer);

        ASSERT_EQ(restored.process.kusd.reference_tsc_page().value_or(0), page);
        EXPECT_EQ(scale(restored), saved_scale);
        EXPECT_GT(sequence(restored), saved_sequence);
        EXPECT_EQ(restored.process.kusd.access([](const KUSER_SHARED_DATA64& kusd) { return kusd.QpcFrequency; }), 10'000'000);

        const auto restored_counter = query_performance_counter(restored);
        EXPECT_GE(restored_counter, saved_counter);
        EXPECT_LT(restored_counter - saved_counter, 10u * 10'000'000u);
    }

    // A snapshot can be loaded on a machine whose TSC runs at another rate. As after a Hyper-V migration, the page then takes
    // its TscScale from that machine's rate, so QPC continues from the saved value and still counts 10 MHz.
    TEST(ReferenceTscTest, RestoreTakesTheScaleOfTheRestoringHostsTsc)
    {
        auto emu = create_empty_emulator();
        native_tsc tsc{.value = 0, .frequency = 3'500'000'000};
        const auto read_tsc = [&] { return std::optional{tsc}; };

        utils::buffer_serializer serializer{};
        uint64_t saved_counter{};
        {
            kusd_mmio saving{emu.memory, emu.clock(), false, read_tsc};
            saving.setup(emu.version, emu.fake_env, tsc.frequency);
            tsc.value = 7 * tsc.frequency;
            saved_counter = saving.query_performance_counter(tsc.value);
            saving.serialize(serializer);
        }

        // The restoring partition has just started, on a TSC that runs at 2.5 GHz.
        tsc = {.value = 1'000'000, .frequency = 2'500'000'000};
        kusd_mmio restored{emu.memory, emu.clock(), false, read_tsc};
        utils::buffer_deserializer deserializer{serializer};
        restored.deserialize(deserializer);

        const auto page = restored.reference_tsc_page();
        ASSERT_TRUE(page);
        EXPECT_EQ(emulator_object<uint64_t>(emu.memory, *page + 8).read(), reference_tsc_scale(tsc.frequency));
        EXPECT_EQ(restored.query_performance_counter(tsc.value), saved_counter);

        const auto one_second_later = restored.query_performance_counter(tsc.value + tsc.frequency) - saved_counter;
        EXPECT_GE(one_second_later, 9'999'999u);
        EXPECT_LE(one_second_later, 10'000'000u);
    }
}
