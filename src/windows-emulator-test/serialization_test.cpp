#include "emulation_test_utils.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace sogen::test
{
    namespace
    {
        // TEMPORARY debug instrumentation for the Icicle-on-Linux serialization non-determinism (PR #1235 CI).
        // On a buffer mismatch it prints the divergence shape so the CI log tells us the root-cause category:
        // a single small differing range => an uninitialized field / timestamp; many scattered ranges across the
        // buffer => the emulated state itself diverged (thread-scheduling / timing non-determinism). Revert once
        // the cause is identified.

        // Dumps the raw buffer to <dir>/<label>.<suffix>.bin so CI can upload it as an artifact; the real
        // (de)serialize code can then be pointed at the exact bytes to identify which component owns the
        // divergence, instead of guessing from a hex window.
        void dump_buffer_to_file(const char* label, const char* suffix, const std::vector<std::byte>& buffer)
        {
            const char* dir = std::getenv("GITHUB_WORKSPACE");
            const std::string path = (dir ? std::string(dir) + "/" : std::string()) + label + "." + suffix + ".bin";

            FILE* file = std::fopen(path.c_str(), "wb");
            if (!file)
            {
                std::printf("[serial-diff] %s: failed to open %s for writing\n", label, path.c_str());
                return;
            }

            std::fwrite(buffer.data(), 1, buffer.size(), file);
            std::fclose(file);
            std::printf("[serial-diff] %s: dumped %zu bytes to %s\n", label, buffer.size(), path.c_str());
        }

        void dump_and_expect_equal(const char* label, const std::vector<std::byte>& a, const std::vector<std::byte>& b)
        {
            if (a == b)
            {
                return;
            }

            dump_buffer_to_file(label, "a", a);
            dump_buffer_to_file(label, "b", b);

            const auto size_a = a.size();
            const auto size_b = b.size();
            const auto common = std::min(size_a, size_b);

            size_t first_diff = common;
            size_t differing_bytes = 0;
            size_t ranges = 0;
            std::array<size_t, 12> range_starts{};
            size_t recorded_ranges = 0;
            bool in_range = false;

            for (size_t i = 0; i < common; ++i)
            {
                if (a[i] != b[i])
                {
                    if (first_diff == common)
                    {
                        first_diff = i;
                    }
                    ++differing_bytes;
                    if (!in_range)
                    {
                        ++ranges;
                        if (recorded_ranges < range_starts.size())
                        {
                            range_starts.at(recorded_ranges++) = i;
                        }
                        in_range = true;
                    }
                }
                else
                {
                    in_range = false;
                }
            }

            std::printf("[serial-diff] %s: size_a=%zu size_b=%zu common=%zu first_diff=%zu differing_bytes=%zu ranges=%zu\n", label, size_a,
                        size_b, common, first_diff, differing_bytes, ranges);

            std::string starts;
            for (size_t i = 0; i < recorded_ranges; ++i)
            {
                std::array<char, 24> tmp{};
                std::snprintf(tmp.data(), tmp.size(), "%zu ", range_starts.at(i));
                starts += tmp.data();
            }
            std::printf("[serial-diff] %s: first_range_starts= %s\n", label, starts.c_str());

            if (first_diff < common)
            {
                const size_t start = first_diff > 48 ? first_diff - 48 : 0;
                const size_t end = std::min(common, first_diff + 48);
                const auto hex = [&](const std::vector<std::byte>& v) {
                    std::string s;
                    for (size_t i = start; i < end; ++i)
                    {
                        std::array<char, 4> tmp{};
                        std::snprintf(tmp.data(), tmp.size(), "%02x ", static_cast<unsigned>(std::to_integer<uint8_t>(v.at(i))));
                        s += tmp.data();
                    }
                    return s;
                };
                std::printf("[serial-diff] %s: a[%zu..%zu)= %s\n", label, start, end, hex(a).c_str());
                std::printf("[serial-diff] %s: b[%zu..%zu)= %s\n", label, start, end, hex(b).c_str());
            }

            std::fflush(stdout);
            ADD_FAILURE() << "[serial-diff] " << label << ": serialized buffers differ (see divergence dump above)";
        }

        // Reports the deterministic instruction-tick clock's driving counter for each side, and its delta as a
        // multiple of MAX_INSTRUCTIONS_PER_TIME_SLICE (0x20000, the fixed amount windows_emulator's idle loop
        // adds to this counter per idle poll iteration) -- to check whether a serialization mismatch corresponds
        // to a whole number of idle-loop iterations, or something else entirely. Revert once the cause is identified.
        void report_instruction_counts(const char* label, const char* name_a, uint64_t instructions_a, const char* name_b,
                                       uint64_t instructions_b)
        {
            const int64_t delta = static_cast<int64_t>(instructions_b) - static_cast<int64_t>(instructions_a);
            constexpr int64_t time_slice = 0x20000;
            std::printf("[serial-diff] %s: %s.executed_instructions=%llu %s.executed_instructions=%llu delta=%lld "
                        "delta/0x20000=%lld remainder=%lld\n",
                        label, name_a, static_cast<unsigned long long>(instructions_a), name_b,
                        static_cast<unsigned long long>(instructions_b), static_cast<long long>(delta),
                        static_cast<long long>(delta / time_slice), static_cast<long long>(delta % time_slice));
            std::fflush(stdout);
        }
    }

    TEST(SerializationTest, ResettingEmulatorWorks)
    {
        auto emu = create_sample_emulator();

        utils::buffer_serializer start_state{};
        emu.serialize(start_state);

        emu.start();

        ASSERT_TERMINATED_SUCCESSFULLY(emu);

        utils::buffer_serializer end_state1{};
        emu.serialize(end_state1);
        const auto instructions1 = emu.get_executed_instructions();

        utils::buffer_deserializer deserializer{start_state};
        emu.deserialize(deserializer);

        emu.start();

        ASSERT_TERMINATED_SUCCESSFULLY(emu);

        utils::buffer_serializer end_state2{};
        emu.serialize(end_state2);
        const auto instructions2 = emu.get_executed_instructions();

        report_instruction_counts("ResettingEmulatorWorks", "run1", instructions1, "run2", instructions2);
        dump_and_expect_equal("ResettingEmulatorWorks", end_state1.get_buffer(), end_state2.get_buffer());
    }

    TEST(SerializationTest, SerializedDataIsReproducible)
    {
        auto emu1 = create_sample_emulator();
        emu1.start();

        ASSERT_TERMINATED_SUCCESSFULLY(emu1);

        utils::buffer_serializer serializer1{};
        emu1.serialize(serializer1);
        const auto instructions1 = emu1.get_executed_instructions();

        utils::buffer_deserializer deserializer{serializer1};

        auto new_emu = create_empty_emulator();
        new_emu.deserialize(deserializer);

        utils::buffer_serializer serializer2{};
        new_emu.serialize(serializer2);
        const auto instructions2 = new_emu.get_executed_instructions();

        auto buffer1 = serializer1.move_buffer();
        auto buffer2 = serializer2.move_buffer();

        report_instruction_counts("SerializedDataIsReproducible", "emu1", instructions1, "new_emu", instructions2);
        dump_and_expect_equal("SerializedDataIsReproducible", buffer1, buffer2);
    }

    TEST(SerializationTest, EmulationIsReproducible)
    {
        auto emu1 = create_sample_emulator();
        emu1.start();

        ASSERT_TERMINATED_SUCCESSFULLY(emu1);

        utils::buffer_serializer serializer1{};
        emu1.serialize(serializer1);
        const auto instructions1 = emu1.get_executed_instructions();

        auto emu2 = create_sample_emulator();
        emu2.start();

        ASSERT_TERMINATED_SUCCESSFULLY(emu2);

        utils::buffer_serializer serializer2{};
        emu2.serialize(serializer2);
        const auto instructions2 = emu2.get_executed_instructions();

        report_instruction_counts("EmulationIsReproducible", "emu1", instructions1, "emu2", instructions2);
        dump_and_expect_equal("EmulationIsReproducible", serializer1.get_buffer(), serializer2.get_buffer());
    }

    TEST(SerializationTest, DeserializedEmulatorBehavesLikeSource)
    {
        auto emu = create_sample_emulator();
        emu.start(100);

        utils::buffer_serializer serializer{};
        emu.serialize(serializer);

        utils::buffer_deserializer deserializer{serializer};

        auto new_emu = create_empty_emulator();
        new_emu.deserialize(deserializer);

        new_emu.start();
        ASSERT_TERMINATED_SUCCESSFULLY(new_emu);

        emu.start();
        ASSERT_TERMINATED_SUCCESSFULLY(emu);

        utils::buffer_serializer serializer1{};
        utils::buffer_serializer serializer2{};

        emu.serialize(serializer1);
        const auto instructions1 = emu.get_executed_instructions();
        new_emu.serialize(serializer2);
        const auto instructions2 = new_emu.get_executed_instructions();

        report_instruction_counts("DeserializedEmulatorBehavesLikeSource", "emu", instructions1, "new_emu", instructions2);
        dump_and_expect_equal("DeserializedEmulatorBehavesLikeSource", serializer1.get_buffer(), serializer2.get_buffer());
    }
} // namespace sogen::test
