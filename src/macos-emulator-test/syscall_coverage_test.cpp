#include <gtest/gtest.h>

#include "macos_test_utils.hpp"

#include <bsd_syscall_dispatcher.hpp>
#include <trace/bsd_syscall_table.hpp>

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace
{
    constexpr uint64_t unassigned_in_xnu_12377_121_6 = 519;

    TEST(SyscallCoverage, TheRegisteredSetIsObservableAndSorted)
    {
        const auto emu = macos_test::make_emulator();
        const auto numbers = sogen::registered_bsd_syscall_numbers(emu->dispatcher);

        ASSERT_FALSE(numbers.empty()) << "a bound emulator registers BSD handlers";
        EXPECT_TRUE(std::ranges::is_sorted(numbers)) << "ascending, so a diff against the xnu table is a merge";
        EXPECT_EQ(std::ranges::adjacent_find(numbers), numbers.end()) << "no number is registered twice";
        EXPECT_LT(numbers.size(), size_t{600}) << "the dispatcher's 600-slot table is not fully populated";

        EXPECT_NE(std::ranges::find(numbers, sogen::macos_syscalls::MACOS_SYS_read), numbers.end());

        ASSERT_EQ(sogen::find_bsd_syscall_prototype(unassigned_in_xnu_12377_121_6), nullptr)
            << "this number must stay unassigned in xnu for the next assertion to mean anything";
        EXPECT_EQ(std::ranges::find(numbers, unassigned_in_xnu_12377_121_6), numbers.end())
            << "xnu assigns no syscall here, so no handler can ever legitimately claim it";
    }

    // Every _nocancel syscall in the xnu table, and whether sogen answers it. A _nocancel variant is
    // never a different syscall -- it only skips a pthread cancellation check -- so any one whose base is
    // implemented should be registered, and the ones left are exactly those whose base is missing.
    //
    // "registered" here is checked two ways on purpose: once through registered_bsd_syscall_numbers, and
    // once directly against dispatcher.get_entry()->handler. An accessor that reported "named by xnu" as
    // "registered" would make every _nocancel look present without this cross-check, since xnu names all
    // of them.
    TEST(SyscallCoverage, EveryNocancelWithAnImplementedBaseIsRegistered)
    {
        const auto emu = macos_test::make_emulator();
        const auto numbers = sogen::registered_bsd_syscall_numbers(emu->dispatcher);
        const auto claims_registered = [&](const uint64_t n) { return std::ranges::find(numbers, n) != numbers.end(); };
        const auto actually_registered = [&](const uint64_t n) {
            const auto* entry = emu->dispatcher.get_entry(n);
            return entry != nullptr && entry->handler != nullptr;
        };

        std::vector<std::string> unregistered_with_a_base{};

        for (uint32_t number = 0; number < sogen::bsd_syscall_table_size(); ++number)
        {
            const auto* prototype = sogen::find_bsd_syscall_prototype(number);
            if (prototype == nullptr || !prototype->name.ends_with("_nocancel"))
            {
                continue;
            }

            ASSERT_EQ(claims_registered(number), actually_registered(number))
                << prototype->name
                << " must be reported registered exactly when it has a real handler,"
                   " not merely because xnu names the syscall";

            if (actually_registered(number))
            {
                continue;
            }

            const auto base = prototype->name.substr(0, prototype->name.size() - std::string_view{"_nocancel"}.size());
            for (uint32_t candidate = 0; candidate < sogen::bsd_syscall_table_size(); ++candidate)
            {
                const auto* other = sogen::find_bsd_syscall_prototype(candidate);
                if (other == nullptr || other->name != base)
                {
                    continue;
                }

                ASSERT_EQ(claims_registered(candidate), actually_registered(candidate))
                    << other->name
                    << " must be reported registered exactly when it has a real handler,"
                       " not merely because xnu names the syscall";

                if (actually_registered(candidate))
                {
                    unregistered_with_a_base.emplace_back(prototype->name);
                }
                break;
            }
        }

        EXPECT_TRUE(unregistered_with_a_base.empty())
            << "these _nocancel syscalls have an implemented base and are one registration line away: " << [&] {
                   std::string joined{};
                   for (const auto& name : unregistered_with_a_base)
                   {
                       joined += name + " ";
                   }
                   return joined;
               }();
    }

    // Not a gate -- an inventory. sogen implements a fraction of xnu's table on purpose, and this makes
    // the fraction visible rather than folklore. Update the recorded figure when it moves.
    TEST(SyscallCoverage, ReportsHowMuchOfTheXnuTableIsImplemented)
    {
        const auto emu = macos_test::make_emulator();
        const auto numbers = sogen::registered_bsd_syscall_numbers(emu->dispatcher);

        size_t named = 0;
        for (uint32_t number = 0; number < sogen::bsd_syscall_table_size(); ++number)
        {
            named += sogen::find_bsd_syscall_prototype(number) != nullptr ? 1u : 0u;
        }

        std::cout << "[ COVERAGE ] " << numbers.size() << " of " << named << " xnu syscalls implemented ("
                  << sogen::BSD_SYSCALL_TABLE_XNU_VERSION << ")\n";

        EXPECT_GE(numbers.size(), 128u) << "coverage must never go backwards";
    }
}
