#include "emulation_test_utils.hpp"

#include <gtest/gtest.h>

namespace sogen::test
{
    // TscScale read from the reference TSC page of a Hyper-V root whose TSC runs at 3,494,402,315 Hz.
    constexpr uint64_t measured_tsc_frequency = 3'494'402'315;
    constexpr uint64_t measured_tsc_scale = 0x00bb8bb097078acf;

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
