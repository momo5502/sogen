#include "linux_emulation_test_utils.hpp"

#include <linux_syscall_numbers.hpp>

namespace sogen
{
    namespace linux_test
    {
        TEST(LinuxThreadSchedulerTest, SwitchesAndFiresLifecycleCallbacks)
        {
            auto emu_backend = create_x86_64_emulator();
            linux_emulator linux_emu(std::move(emu_backend), get_linux_emulation_root());
            linux_emu.log.disable_output(true);

            std::vector<uint32_t> created_threads{};
            std::vector<uint32_t> terminated_threads{};
            std::vector<std::pair<uint32_t, uint32_t>> thread_switches{};

            linux_emu.on_thread_create.add([&](linux_thread& thread) { created_threads.push_back(thread.tid); });
            linux_emu.on_thread_terminated.add([&](linux_thread& thread) { terminated_threads.push_back(thread.tid); });
            linux_emu.on_thread_switch.add(
                [&](const uint32_t old_tid, const uint32_t new_tid) { thread_switches.emplace_back(old_tid, new_tid); });

            const auto parent_tid = linux_emu.process.create_thread(0x700000, 0x1000, 0x100000);
            ASSERT_TRUE(linux_emu.activate_thread(parent_tid));
            ASSERT_EQ(linux_emu.current_thread_id(), parent_tid);
            thread_switches.clear();

            linux_emu.emu().reg(x86_register::rip, 0x100000);
            linux_emu.emu().reg(x86_register::rsp, 0x701000);
            linux_emu.emu().reg(x86_register::rax, 0xAAAA);

            constexpr uint64_t linux_clone_thread_flag = 0x00010000;
            constexpr uint64_t child_stack = 0x800000;
            linux_emu.emu().reg(x86_register::rdi, linux_clone_thread_flag);
            linux_emu.emu().reg(x86_register::rsi, child_stack);
            linux_emu.emu().reg(x86_register::rdx, 0);
            linux_emu.emu().reg(x86_register::r10, 0);
            linux_emu.emu().reg(x86_register::r8, 0);

            const auto* clone_entry = linux_emu.dispatcher.get_entry(linux_syscalls::LINUX_SYS_clone);
            ASSERT_NE(clone_entry, nullptr);
            ASSERT_NE(clone_entry->handler, nullptr);
            clone_entry->handler({.emu_ref = linux_emu, .emu = linux_emu.emu(), .proc = linux_emu.process});

            const auto child_tid = static_cast<uint32_t>(linux_emu.emu().reg(x86_register::rax));
            ASSERT_NE(child_tid, parent_tid);
            ASSERT_EQ(created_threads, std::vector<uint32_t>{child_tid});
            ASSERT_EQ(linux_emu.process.threads.size(), 2U);

            ASSERT_TRUE(linux_emu.activate_thread(child_tid));
            EXPECT_EQ(linux_emu.current_thread_id(), child_tid);
            EXPECT_EQ(linux_emu.emu().reg(x86_register::rax), 0U);
            EXPECT_EQ(linux_emu.emu().reg(x86_register::rsp), child_stack);
            ASSERT_EQ(thread_switches.size(), 1U);
            EXPECT_EQ(thread_switches.at(0), std::make_pair(parent_tid, child_tid));

            EXPECT_FALSE(linux_emu.activate_thread(999999));

            const auto* exit_entry = linux_emu.dispatcher.get_entry(linux_syscalls::LINUX_SYS_exit);
            ASSERT_NE(exit_entry, nullptr);
            ASSERT_NE(exit_entry->handler, nullptr);
            linux_emu.emu().reg(x86_register::rdi, 7);
            exit_entry->handler({.emu_ref = linux_emu, .emu = linux_emu.emu(), .proc = linux_emu.process});

            ASSERT_EQ(terminated_threads, std::vector<uint32_t>{child_tid});
            EXPECT_FALSE(linux_emu.process.exit_status.has_value());
            EXPECT_TRUE(linux_emu.process.threads.at(child_tid).terminated);
            EXPECT_FALSE(linux_emu.activate_thread(child_tid));
            ASSERT_EQ(linux_emu.current_thread_id(), parent_tid);
            ASSERT_EQ(thread_switches.size(), 2U);
            EXPECT_EQ(thread_switches.at(1), std::make_pair(child_tid, parent_tid));

            linux_emu.emu().reg(x86_register::rdi, 11);
            exit_entry->handler({.emu_ref = linux_emu, .emu = linux_emu.emu(), .proc = linux_emu.process});

            ASSERT_EQ((terminated_threads), (std::vector<uint32_t>{child_tid, parent_tid}));
            ASSERT_TRUE(linux_emu.process.exit_status.has_value());
            EXPECT_EQ(*linux_emu.process.exit_status, 11);
            EXPECT_EQ(linux_emu.current_thread(), nullptr);
            EXPECT_EQ(linux_emu.current_thread_id(), std::nullopt);
            EXPECT_FALSE(linux_emu.activate_thread(parent_tid));
        }
    } // namespace linux_test
} // namespace sogen
