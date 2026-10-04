#include <gtest/gtest.h>

#include "macos_test_utils.hpp"

#include <gui/macos_gui_exports.hpp>
#include <gui/macos_process_manager_routines.hpp>

#include <array>
#include <map>
#include <string>
#include <vector>

namespace
{
    constexpr uint64_t stub_base = 0x100020000ULL;
    constexpr uint64_t scratch = 0x341000000ULL;

    struct process_manager_harness
    {
        std::unique_ptr<sogen::macos_emulator> emu{macos_test::make_emulator()};
        sogen::macos_native_dispatch dispatch{};
        std::map<std::string, uint64_t> entries{};

        process_manager_harness()
        {
            this->emu->memory.allocate_memory(stub_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::all);
            this->emu->memory.allocate_memory(scratch, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

            sogen::register_process_manager_routines(this->dispatch);

            uint64_t cursor = stub_base;
            for (const auto& routine : this->dispatch.routines())
            {
                this->entries[routine.symbol] = cursor;
                this->dispatch.bind_entry(cursor, routine.symbol, routine.handler);
                cursor += 4;
            }

            this->emu->set_native_dispatch(&this->dispatch);
        }

        int32_t call(const std::string& symbol, const std::vector<uint64_t>& args)
        {
            const auto found = this->entries.find(symbol);
            EXPECT_NE(found, this->entries.end()) << symbol << " is not registered";
            if (found == this->entries.end())
            {
                return 0;
            }

            for (size_t i = 0; i < args.size(); ++i)
            {
                this->emu->emu().reg(static_cast<sogen::arm64_register>(static_cast<uint32_t>(sogen::arm64_register::x0) + i),
                                     args[i]);
            }

            this->emu->emu().reg(sogen::arm64_register::pc, found->second + 4);
            this->dispatch.invoke(*this->emu, found->second);

            return static_cast<int32_t>(static_cast<uint32_t>(this->emu->emu().reg(sogen::arm64_register::x0)));
        }

        void write_psn(const uint64_t address, const uint32_t high, const uint32_t low)
        {
            const std::array<uint32_t, 2> psn{high, low};
            this->emu->memory.write_memory(address, psn.data(), sizeof(psn));
        }

        std::array<uint32_t, 2> read_psn(const uint64_t address)
        {
            std::array<uint32_t, 2> psn{};
            this->emu->memory.read_memory(address, psn.data(), sizeof(psn));
            return psn;
        }

        uint8_t read_byte(const uint64_t address)
        {
            uint8_t value = 0xFF;
            this->emu->memory.read_memory(address, &value, sizeof(value));
            return value;
        }
    };

    constexpr int32_t no_err = 0;
    constexpr int32_t param_err = -50;
    constexpr uint32_t k_current_process = 2;

    // The emulated session holds one process, so it is the front process. -[NSApplication init] asks
    // exactly this before it sends -setIsActive:YES, through _NXIsActiveApp.
    TEST(ProcessManagerRoutines, TheEmulatedProcessIsTheFrontProcess)
    {
        process_manager_harness harness{};

        EXPECT_EQ(harness.call("_GetFrontProcess", {scratch}), no_err);

        const auto front = harness.read_psn(scratch);
        EXPECT_EQ(front[0], sogen::MACOS_PROCESS_SERIAL_NUMBER_HIGH);
        EXPECT_EQ(front[1], sogen::MACOS_PROCESS_SERIAL_NUMBER_LOW);

        harness.write_psn(scratch + 0x40, 0, 0);
        EXPECT_EQ(harness.call("_GetCurrentProcess", {scratch + 0x40}), no_err);
        EXPECT_EQ(harness.read_psn(scratch + 0x40), front) << "the front process has to be the current one";

        EXPECT_EQ(harness.call("_GetFrontProcess", {0}), param_err);
    }

    TEST(ProcessManagerRoutines, SameProcessResolvesTheCurrentProcessAlias)
    {
        process_manager_harness harness{};

        constexpr uint64_t front = scratch;
        constexpr uint64_t current_alias = scratch + 0x10;
        constexpr uint64_t other = scratch + 0x20;
        constexpr uint64_t out = scratch + 0x30;

        harness.write_psn(front, sogen::MACOS_PROCESS_SERIAL_NUMBER_HIGH, sogen::MACOS_PROCESS_SERIAL_NUMBER_LOW);
        harness.write_psn(current_alias, 0, k_current_process);
        harness.write_psn(other, 0, sogen::MACOS_PROCESS_SERIAL_NUMBER_LOW + 1);

        EXPECT_EQ(harness.call("_SameProcess", {front, current_alias, out}), no_err);
        EXPECT_EQ(harness.read_byte(out), 1) << "_NXIsActiveApp compares the front process against kCurrentProcess";

        EXPECT_EQ(harness.call("_SameProcess", {other, current_alias, out}), no_err);
        EXPECT_EQ(harness.read_byte(out), 0);

        EXPECT_EQ(harness.call("_SameProcess", {front, front, out}), no_err);
        EXPECT_EQ(harness.read_byte(out), 1);
    }

    TEST(ProcessManagerRoutines, SameProcessRefusesANullArgument)
    {
        process_manager_harness harness{};

        constexpr uint64_t front = scratch;
        constexpr uint64_t out = scratch + 0x30;

        harness.write_psn(front, sogen::MACOS_PROCESS_SERIAL_NUMBER_HIGH, sogen::MACOS_PROCESS_SERIAL_NUMBER_LOW);

        for (const auto& args : {std::vector<uint64_t>{0, front, out}, std::vector<uint64_t>{front, 0, out}})
        {
            const uint8_t poison = 0x7F;
            harness.emu->memory.write_memory(out, &poison, sizeof(poison));
            EXPECT_EQ(harness.call("_SameProcess", args), param_err);
            EXPECT_EQ(harness.read_byte(out), 0) << "HIServices clears the answer before it reports paramErr";
        }

        EXPECT_EQ(harness.call("_SameProcess", {front, front, 0}), param_err);
    }
}
