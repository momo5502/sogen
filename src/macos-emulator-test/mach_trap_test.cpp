#include <gtest/gtest.h>

#include "macos_test_utils.hpp"

#include <bsd_syscall_dispatcher.hpp>
#include <mach/mach_kernel.hpp>
#include <mach/mach_traps.hpp>
#include <mach/mach_types.hpp>

#include <guest/guest_memory_object.hpp>

#include <map>
#include <set>
#include <vector>

namespace
{
    constexpr uint64_t code_base = 0x100000000ULL;
    constexpr uint64_t carry = 0x20000000ULL;

    // Every word below is the first instruction of the matching stub in this host's libsystem_kernel.
    constexpr uint32_t movn_x16_dyld_process_info = 0x92800190;   // mov x16, #-13, a trap sogen does not register
    constexpr uint32_t movn_x16_port_allocate = 0x928001F0;       // mov x16, #-16
    constexpr uint32_t movn_x16_port_deallocate = 0x92800230;     // mov x16, #-18
    constexpr uint32_t movn_x16_port_mod_refs = 0x92800250;       // mov x16, #-19
    constexpr uint32_t movn_x16_port_insert_right = 0x92800290;   // mov x16, #-21
    constexpr uint32_t movn_x16_port_construct = 0x928002F0;      // mov x16, #-24
    constexpr uint32_t movn_x16_port_destruct = 0x92800310;       // mov x16, #-25
    constexpr uint32_t movn_x16_reply_port = 0x92800330;          // mov x16, #-26
    constexpr uint32_t movn_x16_thread_self = 0x92800350;         // mov x16, #-27
    constexpr uint32_t movn_x16_task_self = 0x92800370;           // mov x16, #-28
    constexpr uint32_t movn_x16_host_self = 0x92800390;           // mov x16, #-29
    constexpr uint32_t movn_x16_port_get_attributes = 0x928004F0; // mov x16, #-40
    constexpr uint32_t movn_x16_port_guard = 0x92800510;          // mov x16, #-41
    constexpr uint32_t movn_x16_port_unguard = 0x92800530;        // mov x16, #-42
    constexpr uint32_t movn_x16_special_reply_port = 0x92800630;  // mov x16, #-50
    constexpr uint32_t movn_x16_port_type = 0x92800970;           // mov x16, #-76
    constexpr uint32_t mov_x2_x0 = 0xAA0003E2;
    constexpr uint32_t svc_80 = 0xD4001001;

    // One page clear of code_base, so a test that also writes a program by hand never shares a page with
    // the runner's.
    constexpr uint64_t trap_code_base = code_base + sogen::MACOS_PAGE_SIZE;
    constexpr uint64_t scratch = 0x340000000ULL;
    constexpr uint64_t unmapped = 0x700000000ULL;

    constexpr uint32_t MACH_PORT_RIGHT_SEND = 0;
    constexpr uint32_t MACH_PORT_RIGHT_RECEIVE = 1;
    constexpr uint32_t MACH_PORT_RIGHT_SEND_ONCE = 2;
    constexpr uint32_t MACH_PORT_RIGHT_PORT_SET = 3;
    constexpr uint32_t MACH_PORT_RIGHT_DEAD_NAME = 4;

    void allocate_scratch(sogen::macos_emulator& emu)
    {
        emu.memory.allocate_memory(scratch, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);
    }

    uint64_t result_of(sogen::macos_emulator& emu)
    {
        return emu.emu().reg(sogen::arm64_register::x0);
    }

    // The handlers are reached the way the guest reaches them - a real trap word plus svc - rather than by
    // calling them with a hand-built context, so a mis-registered index shows up here too.
    //
    // Each program gets its own page: unicorn keeps the blocks it has already translated, so a second
    // program written over the first at the same address silently re-executes the first one.
    class trap_runner
    {
      public:
        explicit trap_runner(sogen::macos_emulator& emu)
            : emu_(&emu)
        {
        }

        uint64_t operator()(const uint32_t trap_word, const std::vector<uint64_t>& arguments)
        {
            std::vector<uint32_t> words{};
            for (uint32_t reg = 0; reg < arguments.size(); ++reg)
            {
                macos_test::load_x(words, reg, arguments[reg]);
            }

            words.push_back(trap_word);
            words.push_back(svc_80);

            macos_test::write_guest_code(*this->emu_, this->next_base_, words);
            this->next_base_ += sogen::MACOS_PAGE_SIZE;
            this->emu_->start(words.size());

            return result_of(*this->emu_);
        }

      private:
        sogen::macos_emulator* emu_;
        uint64_t next_base_{trap_code_base};
    };

    sogen::mach::port_name_t construct_port(sogen::macos_emulator& emu, trap_runner& run, const uint32_t flags, const uint32_t queue_limit,
                                            const uint64_t context)
    {
        emu.memory.write_memory(scratch, &flags, sizeof(flags));
        emu.memory.write_memory(scratch + 4, &queue_limit, sizeof(queue_limit));

        if (run(movn_x16_port_construct, {0, scratch, context, scratch + 64}) != sogen::mach::kr::success)
        {
            return sogen::mach::PORT_NULL;
        }

        return sogen::guest_object<uint32_t>{emu.memory, scratch + 64}.read();
    }

    sogen::macos_syscall_context make_context(sogen::macos_emulator& emu)
    {
        return sogen::macos_syscall_context{.emu_ref = emu, .emu = emu.emu(), .proc = emu.process, .argument_offset = 0};
    }

    TEST(MachTraps, TaskSelfReturnsAStablePortName)
    {
        const auto emu = macos_test::make_emulator();
        macos_test::write_guest_code(*emu, code_base,
                                     {
                                         movn_x16_task_self,
                                         svc_80,
                                         movn_x16_task_self,
                                         svc_80,
                                         mov_x2_x0,
                                     });

        emu->start(5);

        const auto name = static_cast<uint32_t>(emu->emu().reg(sogen::arm64_register::x2));
        EXPECT_EQ(name, emu->mach.task_self);
        EXPECT_EQ(name, static_cast<uint32_t>(emu->emu().reg(sogen::arm64_register::x0)))
            << "a second task_self_trap must hand back the same name, not allocate another port";
        EXPECT_NE(sogen::mach::port_index(name), 0u);
        EXPECT_NE(sogen::mach::port_generation(name), 0);
    }

    TEST(MachTraps, HostSelfAndTaskSelfAreDifferentPorts)
    {
        const auto emu = macos_test::make_emulator();
        macos_test::write_guest_code(*emu, code_base,
                                     {
                                         movn_x16_host_self,
                                         svc_80,
                                         mov_x2_x0,
                                         movn_x16_task_self,
                                         svc_80,
                                     });

        emu->start(5);

        EXPECT_EQ(static_cast<uint32_t>(emu->emu().reg(sogen::arm64_register::x2)), emu->mach.host_self);
        EXPECT_EQ(static_cast<uint32_t>(emu->emu().reg(sogen::arm64_register::x0)), emu->mach.task_self);
        EXPECT_NE(emu->mach.host_self, emu->mach.task_self);
    }

    TEST(MachTraps, ThreadSelfNamesTheRunningThreadAndIsStable)
    {
        const auto emu = macos_test::make_emulator();
        macos_test::write_guest_code(*emu, code_base,
                                     {
                                         movn_x16_thread_self,
                                         svc_80,
                                         movn_x16_thread_self,
                                         svc_80,
                                         mov_x2_x0,
                                     });

        emu->start(5);

        const auto name = static_cast<uint32_t>(emu->emu().reg(sogen::arm64_register::x2));
        EXPECT_EQ(name, emu->mach.thread_self_for(sogen::MACH_MAIN_THREAD_ID));
        EXPECT_EQ(name, static_cast<uint32_t>(emu->emu().reg(sogen::arm64_register::x0)));
        EXPECT_NE(name, emu->mach.task_self);
        EXPECT_NE(name, emu->mach.host_self);
        EXPECT_EQ(emu->mach.ports.object_of(name).kind, sogen::mach::kernel_object_kind::thread);
        EXPECT_EQ(emu->mach.ports.object_of(name).id, sogen::MACH_MAIN_THREAD_ID);
    }

    TEST(MachTraps, ThreadSelfNamesTheActiveThreadRatherThanTheMainOne)
    {
        const auto emu = macos_test::make_emulator();

        ASSERT_EQ(emu->process.create_thread(0x200000000ULL, sogen::MACOS_PAGE_SIZE, code_base), sogen::MACH_MAIN_THREAD_ID);
        const auto worker = emu->process.create_thread(0x210000000ULL, sogen::MACOS_PAGE_SIZE, code_base);
        ASSERT_NE(worker, sogen::MACH_MAIN_THREAD_ID);
        emu->process.active_thread = &emu->process.threads.at(worker);

        macos_test::write_guest_code(*emu, code_base, macos_test::mach_trap_words(movn_x16_thread_self));

        emu->start(2);

        const auto name = static_cast<uint32_t>(emu->emu().reg(sogen::arm64_register::x0));
        EXPECT_EQ(name, emu->mach.thread_self_for(worker));
        EXPECT_NE(name, emu->mach.thread_self_for(sogen::MACH_MAIN_THREAD_ID));
        EXPECT_EQ(emu->mach.ports.object_of(name).id, worker);
    }

    TEST(MachTraps, AFailingMachTrapLeavesTheCarryFlagClear)
    {
        const auto emu = macos_test::make_emulator();
        emu->emu().reg(sogen::arm64_register::nzcv, carry);

        macos_test::write_guest_code(*emu, code_base,
                                     {
                                         movn_x16_task_self, svc_80,
                                         0x9A9F37E3, // cset x3, cs
                                     });

        emu->start(3);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x3), 0u)
            << "mach traps return kern_return_t in x0; xnu's thread_syscall_return never sets carry";
    }

    // The trap above succeeds, so it cannot show what a *failing* mach trap does with the carry flag.
    // Nothing registered yet fails, so the result writer is driven directly.
    TEST(MachTraps, AMachResultNeitherSetsNorClearsCarry)
    {
        const auto emu = macos_test::make_emulator();
        const auto context = make_context(*emu);

        emu->emu().reg(sogen::arm64_register::nzcv, uint64_t{0});
        emu->emu().reg(sogen::arm64_register::x1, uint64_t{0xBADBADBADBADBADEULL});
        sogen::mach_traps::write_mach_result(context, sogen::mach::kr::invalid_name);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 15u);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x1), 0u);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, 0u) << "a mach failure must not signal through carry";

        emu->emu().reg(sogen::arm64_register::nzcv, carry);
        sogen::mach_traps::write_mach_result(context, sogen::mach::kr::success);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, carry)
            << "clearing carry is the dispatcher's job, done before the handler runs";
    }

    TEST(MachTraps, APortResultIsWrittenAsAWholeName)
    {
        const auto emu = macos_test::make_emulator();
        const auto context = make_context(*emu);

        emu->emu().reg(sogen::arm64_register::x1, uint64_t{0xBADBADBADBADBADEULL});
        sogen::mach_traps::write_mach_port_result(context, emu->mach.bootstrap);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), emu->mach.bootstrap);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x1), 0u);
    }

    TEST(MachTraps, AnUnregisteredMachTrapStillReportsByIndex)
    {
        const auto emu = macos_test::make_emulator();

        uint64_t observed_id = 0;
        emu->callbacks.on_syscall = [&](const uint64_t id, std::string_view) {
            observed_id = id;
            return sogen::instruction_hook_continuation::run_instruction;
        };

        macos_test::write_guest_code(*emu, code_base, macos_test::mach_trap_words(movn_x16_dyld_process_info));

        emu->start(2);

        EXPECT_EQ(observed_id, 13u);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 78u);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, carry);
    }

    TEST(MachTraps, TheSelfTrapsAreRegisteredAtTheirMeasuredIndices)
    {
        sogen::bsd_syscall_dispatcher dispatcher{};
        dispatcher.add_handlers();

        const auto* thread_self = dispatcher.get_mach_trap_entry(27);
        const auto* task_self = dispatcher.get_mach_trap_entry(28);
        const auto* host_self = dispatcher.get_mach_trap_entry(29);

        ASSERT_NE(thread_self, nullptr);
        ASSERT_NE(task_self, nullptr);
        ASSERT_NE(host_self, nullptr);

        EXPECT_EQ(thread_self->name, "thread_self_trap");
        EXPECT_EQ(task_self->name, "task_self_trap");
        EXPECT_EQ(host_self->name, "host_self_trap");
        EXPECT_NE(thread_self->handler, nullptr);
        EXPECT_NE(task_self->handler, nullptr);
        EXPECT_NE(host_self->handler, nullptr);

        // 13 is task_dyld_process_info_notify_get, which sogen does not register. 11 used to stand here
        // and no longer can: CoreAnimation marks its backing stores volatile between frames, so
        // mach_vm_purgable_control is answered now.
        const auto* dyld_process_info = dispatcher.get_mach_trap_entry(13);
        ASSERT_NE(dyld_process_info, nullptr);
        EXPECT_EQ(dyld_process_info->handler, nullptr) << "trap 13 is the index this plan never registers";
    }

    TEST(MachTraps, BootstrapPortExistsBeforeTheGuestRuns)
    {
        const auto emu = macos_test::make_emulator();

        EXPECT_NE(emu->mach.bootstrap, sogen::mach::PORT_NULL);
        EXPECT_EQ(emu->mach.get_task_special_port(sogen::mach::task_special_port::bootstrap), emu->mach.bootstrap);
        EXPECT_EQ(emu->mach.ports.object_of(emu->mach.bootstrap).kind, sogen::mach::kernel_object_kind::bootstrap);
    }

    TEST(MachTraps, TheFourBootPortsAreDistinctAndCarryTheirOwnKinds)
    {
        const auto emu = macos_test::make_emulator();
        const auto thread_self = emu->mach.thread_self_for(sogen::MACH_MAIN_THREAD_ID);

        const std::set<sogen::mach::port_name_t> names{emu->mach.task_self, emu->mach.host_self, emu->mach.bootstrap, thread_self};
        EXPECT_EQ(names.size(), 4u) << "the boot ports must be four distinct names";

        EXPECT_EQ(emu->mach.ports.object_of(emu->mach.task_self).kind, sogen::mach::kernel_object_kind::task);
        EXPECT_EQ(emu->mach.ports.object_of(emu->mach.host_self).kind, sogen::mach::kernel_object_kind::host);
        EXPECT_EQ(emu->mach.ports.object_of(emu->mach.bootstrap).kind, sogen::mach::kernel_object_kind::bootstrap);
        EXPECT_EQ(emu->mach.ports.object_of(thread_self).kind, sogen::mach::kernel_object_kind::thread);
    }

    TEST(MachTraps, TaskSpecialPortsAreLookedUpAndBoundChecked)
    {
        const auto emu = macos_test::make_emulator();
        auto& mach = emu->mach;

        EXPECT_EQ(mach.get_task_special_port(sogen::mach::task_special_port::kernel), mach.task_self);
        EXPECT_EQ(mach.get_task_special_port(sogen::mach::task_special_port::host), mach.host_self);

        EXPECT_EQ(mach.get_task_special_port(0), sogen::mach::PORT_NULL);
        EXPECT_EQ(mach.get_task_special_port(-1), sogen::mach::PORT_NULL);
        EXPECT_EQ(mach.get_task_special_port(sogen::mach::task_special_port::max), sogen::mach::PORT_NULL);
        EXPECT_EQ(mach.get_task_special_port(0x7FFFFFFF), sogen::mach::PORT_NULL);

        EXPECT_EQ(mach.set_task_special_port(0, mach.bootstrap), sogen::mach::kr::invalid_argument);
        EXPECT_EQ(mach.set_task_special_port(-1, mach.bootstrap), sogen::mach::kr::invalid_argument);
        EXPECT_EQ(mach.set_task_special_port(sogen::mach::task_special_port::max, mach.bootstrap), sogen::mach::kr::invalid_argument);
        EXPECT_EQ(mach.set_task_special_port(0x7FFFFFFF, mach.bootstrap), sogen::mach::kr::invalid_argument);
        EXPECT_EQ(mach.set_task_special_port(sogen::mach::task_special_port::max - 1, mach.bootstrap), sogen::mach::kr::success);

        EXPECT_EQ(mach.set_task_special_port(sogen::mach::task_special_port::access, 0x99999999), sogen::mach::kr::invalid_right);
        EXPECT_EQ(mach.get_task_special_port(sogen::mach::task_special_port::access), sogen::mach::PORT_NULL);

        ASSERT_EQ(mach.set_task_special_port(sogen::mach::task_special_port::access, mach.host_self), sogen::mach::kr::success);
        EXPECT_EQ(mach.get_task_special_port(sogen::mach::task_special_port::access), mach.host_self);

        ASSERT_EQ(mach.set_task_special_port(sogen::mach::task_special_port::bootstrap, sogen::mach::PORT_NULL), sogen::mach::kr::success);
        EXPECT_EQ(mach.get_task_special_port(sogen::mach::task_special_port::bootstrap), sogen::mach::PORT_NULL);
    }

    TEST(MachTraps, HostPrivIsTheOnlyHostSpecialPortThatExists)
    {
        const auto emu = macos_test::make_emulator();

        EXPECT_EQ(emu->mach.get_host_special_port(sogen::mach::host_special_port::priv), emu->mach.host_self);
        EXPECT_EQ(emu->mach.get_host_special_port(sogen::mach::host_special_port::io_main), sogen::mach::PORT_NULL);
        EXPECT_EQ(emu->mach.get_host_special_port(0), sogen::mach::PORT_NULL);
        EXPECT_EQ(emu->mach.get_host_special_port(-1), sogen::mach::PORT_NULL);
        EXPECT_EQ(emu->mach.get_host_special_port(sogen::mach::host_special_port::max), sogen::mach::PORT_NULL);

        // TASK_BOOTSTRAP_PORT and HOST_IO_MAIN_PORT are both 4, so only a write to one table proves the
        // two lookups do not share it.
        ASSERT_EQ(emu->mach.set_task_special_port(sogen::mach::task_special_port::bootstrap, emu->mach.task_self),
                  sogen::mach::kr::success);
        EXPECT_EQ(emu->mach.get_host_special_port(sogen::mach::task_special_port::bootstrap), sogen::mach::PORT_NULL)
            << "the host special ports are a separate table from the task's";
    }

    TEST(MachTraps, TheSpecialReplyPortIsCachedPerThread)
    {
        const auto emu = macos_test::make_emulator();
        auto& mach = emu->mach;

        const auto first = mach.make_special_reply_port(1);
        EXPECT_NE(first, sogen::mach::PORT_NULL);
        EXPECT_EQ(mach.make_special_reply_port(1), first) << "libsystem_kernel re-uses one reply port per thread";

        const auto other_thread = mach.make_special_reply_port(2);
        EXPECT_NE(other_thread, first);

        const auto* entry = mach.ports.find(first);
        ASSERT_NE(entry, nullptr);
        EXPECT_TRUE(entry->has_receive);

        ASSERT_EQ(mach.ports.mod_refs(first, sogen::mach::right_kind::receive, -1), sogen::mach::kr::success);
        const auto replacement = mach.make_special_reply_port(1);
        EXPECT_NE(replacement, first) << "a destroyed reply port must be replaced, not handed back dead";
        EXPECT_TRUE(mach.ports.exists(replacement));
    }

    TEST(MachTraps, ThreadPortsAreCachedPerThreadAndReplacedAfterDestruction)
    {
        const auto emu = macos_test::make_emulator();
        auto& mach = emu->mach;

        const auto main_port = mach.thread_self_for(sogen::MACH_MAIN_THREAD_ID);
        EXPECT_EQ(mach.thread_self_for(sogen::MACH_MAIN_THREAD_ID), main_port);

        const auto second = mach.thread_self_for(7);
        EXPECT_NE(second, main_port);
        EXPECT_EQ(mach.ports.object_of(second).id, 7u);

        ASSERT_EQ(mach.ports.mod_refs(main_port, sogen::mach::right_kind::receive, -1), sogen::mach::kr::success);
        const auto replacement = mach.thread_self_for(sogen::MACH_MAIN_THREAD_ID);
        EXPECT_NE(replacement, main_port);
        EXPECT_EQ(mach.ports.object_of(replacement).kind, sogen::mach::kernel_object_kind::thread);
    }

    TEST(MachTraps, MachStateSurvivesASerializationRoundTrip)
    {
        const auto emu = macos_test::make_emulator();
        const auto reply_port = emu->mach.make_special_reply_port(1);
        const auto worker_port = emu->mach.thread_self_for(9);
        ASSERT_EQ(emu->mach.set_task_special_port(sogen::mach::task_special_port::access, emu->mach.host_self), sogen::mach::kr::success);

        sogen::utils::buffer_serializer serializer{};
        emu->mach.serialize(serializer);

        sogen::mach_kernel restored{};
        sogen::utils::buffer_deserializer deserializer{serializer.get_buffer()};
        restored.deserialize(deserializer);

        EXPECT_EQ(restored.task_self, emu->mach.task_self);
        EXPECT_EQ(restored.host_self, emu->mach.host_self);
        EXPECT_EQ(restored.bootstrap, emu->mach.bootstrap);
        EXPECT_EQ(restored.get_task_special_port(sogen::mach::task_special_port::bootstrap), emu->mach.bootstrap);
        EXPECT_EQ(restored.get_task_special_port(sogen::mach::task_special_port::access), emu->mach.host_self);
        EXPECT_EQ(restored.get_host_special_port(sogen::mach::host_special_port::priv), emu->mach.host_self);
        EXPECT_EQ(restored.make_special_reply_port(1), reply_port) << "the reply-port cache must survive, or the guest's cached name dies";
        EXPECT_EQ(restored.thread_self_for(9), worker_port);
        EXPECT_EQ(restored.thread_self_for(sogen::MACH_MAIN_THREAD_ID), emu->mach.thread_self_for(sogen::MACH_MAIN_THREAD_ID));
        EXPECT_EQ(restored.ports.live_port_count(), emu->mach.ports.live_port_count());
    }

    TEST(MachTraps, ASnapshotWithAnOutOfRangeSpecialPortIsRejected)
    {
        const auto emu = macos_test::make_emulator();

        sogen::utils::buffer_serializer serializer{};
        emu->mach.ports.serialize(serializer);
        serializer.write(emu->mach.task_self);
        serializer.write(emu->mach.host_self);
        serializer.write(emu->mach.bootstrap);
        serializer.write_map(std::map<uint64_t, sogen::mach::port_name_t>{});
        serializer.write_map(std::map<uint64_t, sogen::mach::port_name_t>{});
        serializer.write_map(std::map<int32_t, sogen::mach::port_name_t>{{0x40000000, emu->mach.bootstrap}});
        serializer.write_map(std::map<int32_t, sogen::mach::port_name_t>{});

        sogen::mach_kernel restored{};
        sogen::utils::buffer_deserializer deserializer{serializer.get_buffer()};
        EXPECT_THROW(restored.deserialize(deserializer), std::runtime_error);
    }

    TEST(MachTraps, SpecialReplyPortIsStableAndDistinctFromReplyPort)
    {
        const auto emu = macos_test::make_emulator();
        macos_test::write_guest_code(*emu, code_base,
                                     {
                                         movn_x16_special_reply_port,
                                         svc_80,
                                         mov_x2_x0,
                                         movn_x16_special_reply_port,
                                         svc_80,
                                     });

        emu->start(5);

        const auto first = static_cast<uint32_t>(emu->emu().reg(sogen::arm64_register::x2));
        const auto second = static_cast<uint32_t>(emu->emu().reg(sogen::arm64_register::x0));

        EXPECT_NE(first, sogen::mach::PORT_NULL);
        EXPECT_EQ(first, second) << "libsystem_kernel caches one special reply port per thread";
        EXPECT_TRUE(emu->mach.ports.exists(first));
        EXPECT_EQ(first, emu->mach.make_special_reply_port(sogen::MACH_MAIN_THREAD_ID));

        trap_runner run{*emu};
        EXPECT_NE(static_cast<uint32_t>(run(movn_x16_reply_port, {})), first) << "trap 26 must not hand back the cached special reply port";
    }

    TEST(MachTraps, TheSpecialReplyPortFollowsTheRunningThread)
    {
        const auto emu = macos_test::make_emulator();
        trap_runner run{*emu};

        ASSERT_EQ(emu->process.create_thread(0x200000000ULL, sogen::MACOS_PAGE_SIZE, code_base), sogen::MACH_MAIN_THREAD_ID);
        const auto worker = emu->process.create_thread(0x210000000ULL, sogen::MACOS_PAGE_SIZE, code_base);
        emu->process.active_thread = &emu->process.threads.at(worker);

        const auto on_worker = static_cast<uint32_t>(run(movn_x16_special_reply_port, {}));

        EXPECT_NE(on_worker, sogen::mach::PORT_NULL);
        EXPECT_EQ(on_worker, emu->mach.make_special_reply_port(worker));
        EXPECT_NE(on_worker, emu->mach.make_special_reply_port(sogen::MACH_MAIN_THREAD_ID));
    }

    TEST(MachTraps, ReplyPortAllocatesAFreshReceiveRight)
    {
        const auto emu = macos_test::make_emulator();
        macos_test::write_guest_code(*emu, code_base,
                                     {
                                         movn_x16_reply_port,
                                         svc_80,
                                         mov_x2_x0,
                                         movn_x16_reply_port,
                                         svc_80,
                                     });

        emu->start(5);

        const auto first = static_cast<uint32_t>(emu->emu().reg(sogen::arm64_register::x2));
        const auto second = static_cast<uint32_t>(emu->emu().reg(sogen::arm64_register::x0));

        EXPECT_NE(first, sogen::mach::PORT_NULL);
        EXPECT_NE(first, second) << "mach_reply_port allocates, it does not cache";
        ASSERT_NE(emu->mach.ports.find(first), nullptr);
        EXPECT_TRUE(emu->mach.ports.find(first)->has_receive);
        EXPECT_TRUE(emu->mach.ports.exists(second));
    }

    TEST(MachTraps, PortAllocateWritesAReceiveRightThroughTheOutPointer)
    {
        const auto emu = macos_test::make_emulator();
        trap_runner run{*emu};
        allocate_scratch(*emu);

        EXPECT_EQ(run(movn_x16_port_allocate, {0, MACH_PORT_RIGHT_RECEIVE, scratch}), sogen::mach::kr::success);

        const auto name = sogen::guest_object<uint32_t>{emu->memory, scratch}.read();

        EXPECT_NE(name, sogen::mach::PORT_NULL);
        ASSERT_NE(emu->mach.ports.find(name), nullptr);
        EXPECT_TRUE(emu->mach.ports.find(name)->has_receive);
        EXPECT_EQ(emu->mach.ports.type_of(name), sogen::mach::port_type::receive);
    }

    TEST(MachTraps, PortAllocateBuildsAPortSetWhenAskedForOne)
    {
        const auto emu = macos_test::make_emulator();
        trap_runner run{*emu};
        allocate_scratch(*emu);

        EXPECT_EQ(run(movn_x16_port_allocate, {0, MACH_PORT_RIGHT_PORT_SET, scratch}), sogen::mach::kr::success);

        const auto name = sogen::guest_object<uint32_t>{emu->memory, scratch}.read();

        ASSERT_NE(emu->mach.ports.find(name), nullptr);
        EXPECT_TRUE(emu->mach.ports.find(name)->is_port_set);
        EXPECT_FALSE(emu->mach.ports.find(name)->has_receive);
        EXPECT_EQ(emu->mach.ports.type_of(name), sogen::mach::port_type::port_set);
    }

    TEST(MachTraps, PortAllocateRejectsRightsItCannotCreate)
    {
        const auto emu = macos_test::make_emulator();
        trap_runner run{*emu};
        allocate_scratch(*emu);

        const sogen::guest_object<uint32_t> out{emu->memory, scratch};
        const auto before = emu->mach.ports.live_port_count();

        for (const auto right : {MACH_PORT_RIGHT_SEND, MACH_PORT_RIGHT_SEND_ONCE, MACH_PORT_RIGHT_DEAD_NAME, 5u, 0xFFFFFFFFu})
        {
            out.write(0xDEADBEEF);

            EXPECT_EQ(run(movn_x16_port_allocate, {0, right, scratch}), sogen::mach::kr::invalid_value) << "right " << right;
            EXPECT_EQ(out.read(), 0xDEADBEEFu) << "a rejected right must not write the out pointer, right " << right;
            EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, 0u) << "right " << right;
        }

        EXPECT_EQ(emu->mach.ports.live_port_count(), before) << "a rejected right must not allocate";
    }

    TEST(MachTraps, PortAllocateReportsAnUnwritableOutPointer)
    {
        const auto emu = macos_test::make_emulator();
        trap_runner run{*emu};

        EXPECT_EQ(run(movn_x16_port_allocate, {0, MACH_PORT_RIGHT_RECEIVE, unmapped}), sogen::mach::kr::invalid_address);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, 0u);
        EXPECT_EQ(emu->last_stop_reason(), sogen::stop_reason::instruction_limit) << "a bad guest pointer must not fault the guest";
    }

    TEST(MachTraps, PortDeallocateDropsOneSendRightAtATime)
    {
        const auto emu = macos_test::make_emulator();
        trap_runner run{*emu};
        const auto receive = emu->mach.ports.allocate_receive_right();
        ASSERT_NE(emu->mach.ports.insert_send_right(receive), sogen::mach::PORT_NULL);
        ASSERT_EQ(emu->mach.ports.mod_refs(receive, sogen::mach::right_kind::send, 1), sogen::mach::kr::success);

        EXPECT_EQ(run(movn_x16_port_deallocate, {0, receive}), sogen::mach::kr::success);
        ASSERT_NE(emu->mach.ports.find(receive), nullptr);
        EXPECT_EQ(emu->mach.ports.find(receive)->send_urefs, 1u);

        EXPECT_EQ(run(movn_x16_port_deallocate, {0, 0x9999}), sogen::mach::kr::invalid_name);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, 0u);
    }

    TEST(MachTraps, ModRefsOnAnUnknownNameReturnsInvalidName)
    {
        const auto emu = macos_test::make_emulator();
        trap_runner run{*emu};
        emu->emu().reg(sogen::arm64_register::nzcv, carry);

        EXPECT_EQ(run(movn_x16_port_mod_refs, {0, 0x9999, MACH_PORT_RIGHT_SEND, static_cast<uint64_t>(-1)}), sogen::mach::kr::invalid_name);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, 0u);
    }

    TEST(MachTraps, ModRefsRejectsARightNumberThatNamesNoRight)
    {
        const auto emu = macos_test::make_emulator();
        trap_runner run{*emu};
        const auto receive = emu->mach.ports.allocate_receive_right();

        for (const auto right : {5u, 6u, 0x10000u, 0xFFFFFFFFu})
        {
            EXPECT_EQ(run(movn_x16_port_mod_refs, {0, receive, right, static_cast<uint64_t>(-1)}), sogen::mach::kr::invalid_value)
                << "right " << right;
            EXPECT_TRUE(emu->mach.ports.exists(receive)) << "a rejected right must not release anything, right " << right;
        }
    }

    TEST(MachTraps, ModRefsReleasesTheReceiveRightItIsGiven)
    {
        const auto emu = macos_test::make_emulator();
        trap_runner run{*emu};
        const auto receive = emu->mach.ports.allocate_receive_right();

        EXPECT_EQ(run(movn_x16_port_mod_refs, {0, receive, MACH_PORT_RIGHT_RECEIVE, static_cast<uint64_t>(-1)}), sogen::mach::kr::success);
        EXPECT_FALSE(emu->mach.ports.exists(receive));
    }

    TEST(MachTraps, InsertRightAddsASendRightUnderTheNameItIsGiven)
    {
        const auto emu = macos_test::make_emulator();
        trap_runner run{*emu};
        const auto receive = emu->mach.ports.allocate_receive_right();

        EXPECT_EQ(run(movn_x16_port_insert_right, {0, receive, receive, sogen::mach::disposition::make_send}), sogen::mach::kr::success);
        ASSERT_NE(emu->mach.ports.find(receive), nullptr);
        EXPECT_EQ(emu->mach.ports.find(receive)->send_urefs, 1u);

        EXPECT_EQ(run(movn_x16_port_insert_right, {0, receive, receive, sogen::mach::disposition::copy_send}), sogen::mach::kr::success);
        EXPECT_EQ(emu->mach.ports.find(receive)->send_urefs, 2u);
        EXPECT_EQ(emu->mach.ports.type_of(receive), sogen::mach::port_type::receive | sogen::mach::port_type::send);
    }

    TEST(MachTraps, InsertRightRejectsWhatTheNamespaceCannotExpress)
    {
        const auto emu = macos_test::make_emulator();
        trap_runner run{*emu};
        const auto receive = emu->mach.ports.allocate_receive_right();
        const auto other = emu->mach.ports.allocate_receive_right();
        const auto once = emu->mach.ports.allocate_send_once_right(receive);

        EXPECT_EQ(run(movn_x16_port_insert_right, {0, receive, 0x9999, sogen::mach::disposition::make_send}),
                  sogen::mach::kr::invalid_name);

        EXPECT_EQ(run(movn_x16_port_insert_right, {0, other, receive, sogen::mach::disposition::make_send}), sogen::mach::kr::name_exists)
            << "a right cannot be installed under a caller-chosen name";
        ASSERT_NE(emu->mach.ports.find(receive), nullptr);
        EXPECT_EQ(emu->mach.ports.find(receive)->send_urefs, 0u);

        EXPECT_EQ(run(movn_x16_port_insert_right, {0, receive, receive, sogen::mach::disposition::move_receive}),
                  sogen::mach::kr::invalid_value);
        EXPECT_EQ(emu->mach.ports.find(receive)->send_urefs, 0u);

        EXPECT_EQ(run(movn_x16_port_insert_right, {0, once, once, sogen::mach::disposition::make_send}), sogen::mach::kr::invalid_right)
            << "a send-once name holds no receive right to make a send from";
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, 0u);

        constexpr auto max_urefs = sogen::mach::mach_port_namespace::MAX_UREFS;
        ASSERT_NE(emu->mach.ports.insert_send_right(other), sogen::mach::PORT_NULL);
        ASSERT_EQ(emu->mach.ports.mod_refs(other, sogen::mach::right_kind::send, static_cast<int32_t>(max_urefs - 1)),
                  sogen::mach::kr::success);
        ASSERT_EQ(emu->mach.ports.find(other)->send_urefs, max_urefs);

        EXPECT_EQ(run(movn_x16_port_insert_right, {0, other, other, sogen::mach::disposition::make_send}), sogen::mach::kr::urefs_overflow);
        EXPECT_EQ(emu->mach.ports.find(other)->send_urefs, max_urefs) << "a refused insert must not change the count";
    }

    TEST(MachTraps, PortConstructWritesTheNameThroughTheOutPointer)
    {
        const auto emu = macos_test::make_emulator();
        trap_runner run{*emu};
        allocate_scratch(*emu);

        EXPECT_EQ(run(movn_x16_port_construct, {0, scratch, 0, scratch + 64}), sogen::mach::kr::success) << "KERN_SUCCESS";

        const auto name = sogen::guest_object<uint32_t>{emu->memory, scratch + 64}.read();

        EXPECT_NE(name, sogen::mach::PORT_NULL);
        EXPECT_TRUE(emu->mach.ports.exists(name));
        ASSERT_NE(emu->mach.ports.find(name), nullptr);
        EXPECT_TRUE(emu->mach.ports.find(name)->has_receive);
        EXPECT_EQ(emu->mach.ports.find(name)->queue_limit, sogen::mach::PORT_QLIMIT_DEFAULT) << "no MPO_QLIMIT means no change";
        EXPECT_EQ(emu->mach.ports.find(name)->send_urefs, 0u);
        EXPECT_FALSE(emu->mach.ports.find(name)->guard.has_value());
    }

    TEST(MachTraps, PortConstructHonoursTheOptionsItSupports)
    {
        const auto emu = macos_test::make_emulator();
        trap_runner run{*emu};
        allocate_scratch(*emu);

        const auto flags = sogen::mach::MPO_QLIMIT | sogen::mach::MPO_INSERT_SEND_RIGHT | sogen::mach::MPO_CONTEXT_AS_GUARD |
                           sogen::mach::MPO_STRICT | sogen::mach::MPO_REPLY_PORT;
        const auto name = construct_port(*emu, run, flags, 17, 0xFEEDFACEULL);

        ASSERT_NE(name, sogen::mach::PORT_NULL);
        const auto* entry = emu->mach.ports.find(name);
        ASSERT_NE(entry, nullptr);
        EXPECT_EQ(entry->queue_limit, 17u);
        EXPECT_EQ(entry->send_urefs, 1u);
        ASSERT_TRUE(entry->guard.has_value());
        EXPECT_EQ(*entry->guard, 0xFEEDFACEULL);
        EXPECT_TRUE(entry->strict_guard);
    }

    TEST(MachTraps, PortConstructAppliesAGuardWithoutStrictnessWhenMpoStrictIsAbsent)
    {
        const auto emu = macos_test::make_emulator();
        trap_runner run{*emu};
        allocate_scratch(*emu);

        const auto name = construct_port(*emu, run, sogen::mach::MPO_CONTEXT_AS_GUARD, 0, 0x1234);

        ASSERT_NE(name, sogen::mach::PORT_NULL);
        const auto* entry = emu->mach.ports.find(name);
        ASSERT_NE(entry, nullptr);
        ASSERT_TRUE(entry->guard.has_value());
        EXPECT_EQ(*entry->guard, 0x1234u);
        EXPECT_FALSE(entry->strict_guard);
        EXPECT_EQ(entry->send_urefs, 0u) << "MPO_INSERT_SEND_RIGHT was not asked for";
        EXPECT_EQ(entry->queue_limit, sogen::mach::PORT_QLIMIT_DEFAULT) << "MPO_QLIMIT was not asked for";
    }

    TEST(MachTraps, PortConstructRejectsAQueueLimitAboveTheMaximum)
    {
        const auto emu = macos_test::make_emulator();
        trap_runner run{*emu};
        allocate_scratch(*emu);

        const auto before = emu->mach.ports.live_port_count();
        EXPECT_EQ(construct_port(*emu, run, sogen::mach::MPO_QLIMIT, sogen::mach::PORT_QLIMIT_MAX + 1, 0), sogen::mach::PORT_NULL);
        EXPECT_EQ(result_of(*emu), sogen::mach::kr::invalid_value);
        EXPECT_EQ(emu->mach.ports.live_port_count(), before) << "a rejected option must not leave a port behind";

        const auto name = construct_port(*emu, run, sogen::mach::MPO_QLIMIT, sogen::mach::PORT_QLIMIT_MAX, 0);
        ASSERT_NE(name, sogen::mach::PORT_NULL) << "the maximum itself is allowed";
        ASSERT_NE(emu->mach.ports.find(name), nullptr);
        EXPECT_EQ(emu->mach.ports.find(name)->queue_limit, sogen::mach::PORT_QLIMIT_MAX);
    }

    TEST(MachTraps, PortConstructReportsUnreachableGuestPointers)
    {
        const auto emu = macos_test::make_emulator();
        trap_runner run{*emu};
        allocate_scratch(*emu);

        const auto before = emu->mach.ports.live_port_count();
        EXPECT_EQ(run(movn_x16_port_construct, {0, unmapped, 0, scratch}), sogen::mach::kr::invalid_address);
        EXPECT_EQ(emu->mach.ports.live_port_count(), before) << "unreadable options must be refused before allocating";

        EXPECT_EQ(run(movn_x16_port_construct, {0, scratch, 0, unmapped}), sogen::mach::kr::invalid_address);
        EXPECT_EQ(emu->last_stop_reason(), sogen::stop_reason::instruction_limit) << "a bad guest pointer must not fault the guest";
    }

    TEST(MachTraps, PortDestructRequiresTheGuardThePortWasBuiltWith)
    {
        const auto emu = macos_test::make_emulator();
        trap_runner run{*emu};
        allocate_scratch(*emu);

        const auto name = construct_port(*emu, run, sogen::mach::MPO_CONTEXT_AS_GUARD | sogen::mach::MPO_INSERT_SEND_RIGHT, 0, 0xABCD);
        ASSERT_NE(name, sogen::mach::PORT_NULL);

        EXPECT_EQ(run(movn_x16_port_destruct, {0, name, 0, 0xBADD}), sogen::mach::kr::invalid_argument);
        EXPECT_TRUE(emu->mach.ports.exists(name));
        ASSERT_NE(emu->mach.ports.find(name), nullptr);
        EXPECT_TRUE(emu->mach.ports.find(name)->has_receive);

        EXPECT_EQ(run(movn_x16_port_destruct, {0, name, static_cast<uint64_t>(-1), 0xABCD}), sogen::mach::kr::success);
        EXPECT_FALSE(emu->mach.ports.exists(name)) << "the last send right went with the receive right";
    }

    TEST(MachTraps, GuardAndUnguardRoundTripThroughTheTraps)
    {
        const auto emu = macos_test::make_emulator();
        trap_runner run{*emu};
        const auto receive = emu->mach.ports.allocate_receive_right();

        EXPECT_EQ(run(movn_x16_port_guard, {0, receive, 0x5150, 1}), sogen::mach::kr::success);
        ASSERT_NE(emu->mach.ports.find(receive), nullptr);
        ASSERT_TRUE(emu->mach.ports.find(receive)->guard.has_value());
        EXPECT_EQ(*emu->mach.ports.find(receive)->guard, 0x5150u);
        EXPECT_TRUE(emu->mach.ports.find(receive)->strict_guard);

        EXPECT_EQ(run(movn_x16_port_unguard, {0, receive, 0x5151}), sogen::mach::kr::invalid_argument);
        EXPECT_TRUE(emu->mach.ports.find(receive)->guard.has_value());

        EXPECT_EQ(run(movn_x16_port_unguard, {0, receive, 0x5150}), sogen::mach::kr::success);
        EXPECT_FALSE(emu->mach.ports.find(receive)->guard.has_value());

        EXPECT_EQ(run(movn_x16_port_guard, {0, receive, 0x5150, 0}), sogen::mach::kr::success);
        EXPECT_FALSE(emu->mach.ports.find(receive)->strict_guard) << "strict comes from the fourth argument, not a constant";

        EXPECT_EQ(run(movn_x16_port_guard, {0, 0x9999, 1, 1}), sogen::mach::kr::invalid_name);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, 0u);
    }

    TEST(MachTraps, GetAttributesReportsTheQueueLimit)
    {
        const auto emu = macos_test::make_emulator();
        trap_runner run{*emu};
        allocate_scratch(*emu);

        const auto name = construct_port(*emu, run, sogen::mach::MPO_QLIMIT, 23, 0);
        ASSERT_NE(name, sogen::mach::PORT_NULL);

        const sogen::guest_object<uint32_t> count{emu->memory, scratch + 128};
        const sogen::guest_object<uint32_t> info{emu->memory, scratch + 256};
        count.write(4);
        info.write(0xDEADBEEF, 1);

        EXPECT_EQ(run(movn_x16_port_get_attributes, {0, name, 1, scratch + 256, scratch + 128}), sogen::mach::kr::success);
        EXPECT_EQ(info.read(), 23u);
        EXPECT_EQ(count.read(), 1u) << "the count out-parameter reports what was produced, not what was asked for";
        EXPECT_EQ(info.read(1), 0xDEADBEEFu) << "MACH_PORT_LIMITS_INFO writes exactly one word";
    }

    TEST(MachTraps, GetAttributesFillsTheReceiveStatusBlock)
    {
        const auto emu = macos_test::make_emulator();
        trap_runner run{*emu};
        allocate_scratch(*emu);

        const auto name = construct_port(*emu, run, sogen::mach::MPO_QLIMIT | sogen::mach::MPO_INSERT_SEND_RIGHT, 9, 0);
        ASSERT_NE(name, sogen::mach::PORT_NULL);
        ASSERT_NE(emu->mach.ports.find(name), nullptr);
        emu->mach.ports.find(name)->queue.push_back({1, 2, 3});
        emu->mach.ports.find(name)->queue.push_back({4});

        const sogen::guest_object<uint32_t> count{emu->memory, scratch + 128};
        const sogen::guest_object<uint32_t> info{emu->memory, scratch + 256};
        count.write(10);
        info.write(0xDEADBEEF, 10);

        EXPECT_EQ(run(movn_x16_port_get_attributes, {0, name, 2, scratch + 256, scratch + 128}), sogen::mach::kr::success);
        EXPECT_EQ(count.read(), 10u);
        EXPECT_EQ(info.read(3), 9u) << "mps_qlimit is the fourth word of mach_port_status_t";
        EXPECT_EQ(info.read(4), 2u) << "mps_msgcount is the fifth word";
        EXPECT_EQ(info.read(6), 1u) << "mps_srights is the seventh word";
        EXPECT_EQ(info.read(0), 0u);
        EXPECT_EQ(info.read(9), 0u);
        EXPECT_EQ(info.read(10), 0xDEADBEEFu) << "MACH_PORT_RECEIVE_STATUS writes exactly ten words";
    }

    TEST(MachTraps, GetAttributesRefusesWhatItCannotAnswer)
    {
        const auto emu = macos_test::make_emulator();
        trap_runner run{*emu};
        allocate_scratch(*emu);

        const auto name = construct_port(*emu, run, sogen::mach::MPO_QLIMIT, 23, 0);
        ASSERT_NE(name, sogen::mach::PORT_NULL);

        const sogen::guest_object<uint32_t> count{emu->memory, scratch + 128};
        const sogen::guest_object<uint32_t> info{emu->memory, scratch + 256};

        count.write(0);
        info.write(0xDEADBEEF);
        EXPECT_EQ(run(movn_x16_port_get_attributes, {0, name, 1, scratch + 256, scratch + 128}), sogen::mach::kr::failure)
            << "a count below the flavour's size is KERN_FAILURE";
        EXPECT_EQ(info.read(), 0xDEADBEEFu) << "a refused call must not write the buffer";
        EXPECT_EQ(count.read(), 0u);

        count.write(9);
        EXPECT_EQ(run(movn_x16_port_get_attributes, {0, name, 2, scratch + 256, scratch + 128}), sogen::mach::kr::failure)
            << "ten words are needed for MACH_PORT_RECEIVE_STATUS";
        EXPECT_EQ(info.read(), 0xDEADBEEFu);

        count.write(64);
        EXPECT_EQ(run(movn_x16_port_get_attributes, {0, name, 3, scratch + 256, scratch + 128}), sogen::mach::kr::invalid_argument)
            << "an unmodelled flavour is refused, not guessed";
        EXPECT_EQ(info.read(), 0xDEADBEEFu);

        EXPECT_EQ(run(movn_x16_port_get_attributes, {0, 0x9999, 1, scratch + 256, scratch + 128}), sogen::mach::kr::invalid_name);
        EXPECT_EQ(run(movn_x16_port_get_attributes, {0, name, 1, scratch + 256, unmapped}), sogen::mach::kr::invalid_address);
        EXPECT_EQ(run(movn_x16_port_get_attributes, {0, name, 1, unmapped, scratch + 128}), sogen::mach::kr::invalid_address);
        EXPECT_EQ(emu->last_stop_reason(), sogen::stop_reason::instruction_limit) << "a bad guest pointer must not fault the guest";
    }

    TEST(MachTraps, PortTypeReportsEveryRightHeldUnderTheName)
    {
        const auto emu = macos_test::make_emulator();
        trap_runner run{*emu};
        allocate_scratch(*emu);

        const auto receive = emu->mach.ports.allocate_receive_right();
        const sogen::guest_object<uint32_t> type{emu->memory, scratch};

        EXPECT_EQ(run(movn_x16_port_type, {0, receive, scratch}), sogen::mach::kr::success);
        EXPECT_EQ(type.read(), sogen::mach::port_type::receive);

        ASSERT_NE(emu->mach.ports.insert_send_right(receive), sogen::mach::PORT_NULL);
        EXPECT_EQ(run(movn_x16_port_type, {0, receive, scratch}), sogen::mach::kr::success);
        EXPECT_EQ(type.read(), sogen::mach::port_type::receive | sogen::mach::port_type::send);

        const auto once = emu->mach.ports.allocate_send_once_right(receive);
        EXPECT_EQ(run(movn_x16_port_type, {0, once, scratch}), sogen::mach::kr::success);
        EXPECT_EQ(type.read(), sogen::mach::port_type::send_once);

        type.write(0xDEADBEEF);
        EXPECT_EQ(run(movn_x16_port_type, {0, 0x9999, scratch}), sogen::mach::kr::invalid_name);
        EXPECT_EQ(type.read(), 0xDEADBEEFu) << "an unknown name must not write the out pointer";

        EXPECT_EQ(run(movn_x16_port_type, {0, receive, unmapped}), sogen::mach::kr::invalid_address);
        EXPECT_EQ(emu->last_stop_reason(), sogen::stop_reason::instruction_limit) << "a bad guest pointer must not fault the guest";
    }

    TEST(MachTraps, EveryFailingPortTrapLeavesTheCarryFlagClear)
    {
        const auto emu = macos_test::make_emulator();
        trap_runner run{*emu};

        const std::vector<std::pair<uint32_t, std::vector<uint64_t>>> failures{
            {movn_x16_port_allocate, {0, MACH_PORT_RIGHT_SEND, unmapped}},
            {movn_x16_port_deallocate, {0, 0x9999}},
            {movn_x16_port_mod_refs, {0, 0x9999, MACH_PORT_RIGHT_SEND, static_cast<uint64_t>(-1)}},
            {movn_x16_port_insert_right, {0, 0x9999, 0x9999, sogen::mach::disposition::make_send}},
            {movn_x16_port_construct, {0, unmapped, 0, unmapped}},
            {movn_x16_port_destruct, {0, 0x9999, 0, 0}},
            {movn_x16_port_get_attributes, {0, 0x9999, 1, unmapped, unmapped}},
            {movn_x16_port_guard, {0, 0x9999, 1, 1}},
            {movn_x16_port_unguard, {0, 0x9999, 1}},
            {movn_x16_port_type, {0, 0x9999, unmapped}},
        };

        for (const auto& [word, arguments] : failures)
        {
            emu->emu().reg(sogen::arm64_register::nzcv, carry);
            const auto result = run(word, arguments);

            EXPECT_NE(result, sogen::mach::kr::success) << "trap word " << std::hex << word;
            EXPECT_NE(result, 78u) << "the trap must be registered, not fall through to ENOSYS, word " << std::hex << word;
            EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, 0u)
                << "a mach trap reports failure in x0 only, word " << std::hex << word;
            EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x1), 0u) << "trap word " << std::hex << word;
        }
    }

    TEST(MachTraps, ThePortTrapsAreRegisteredAtTheirMeasuredIndices)
    {
        sogen::bsd_syscall_dispatcher dispatcher{};
        dispatcher.add_handlers();

        const std::map<uint32_t, std::pair<sogen::macos_syscall_handler, std::string>> expected{
            {16, {sogen::mach_traps::trap_port_allocate, "_kernelrpc_mach_port_allocate_trap"}},
            {18, {sogen::mach_traps::trap_port_deallocate, "_kernelrpc_mach_port_deallocate_trap"}},
            {19, {sogen::mach_traps::trap_port_mod_refs, "_kernelrpc_mach_port_mod_refs_trap"}},
            {20, {sogen::mach_traps::trap_port_move_member, "_kernelrpc_mach_port_move_member_trap"}},
            {21, {sogen::mach_traps::trap_port_insert_right, "_kernelrpc_mach_port_insert_right_trap"}},
            {22, {sogen::mach_traps::trap_port_insert_member, "_kernelrpc_mach_port_insert_member_trap"}},
            {23, {sogen::mach_traps::trap_port_extract_member, "_kernelrpc_mach_port_extract_member_trap"}},
            {24, {sogen::mach_traps::trap_port_construct, "_kernelrpc_mach_port_construct_trap"}},
            {25, {sogen::mach_traps::trap_port_destruct, "_kernelrpc_mach_port_destruct_trap"}},
            {26, {sogen::mach_traps::trap_reply_port, "mach_reply_port"}},
            {40, {sogen::mach_traps::trap_port_get_attributes, "_kernelrpc_mach_port_get_attributes_trap"}},
            {41, {sogen::mach_traps::trap_port_guard, "_kernelrpc_mach_port_guard_trap"}},
            {42, {sogen::mach_traps::trap_port_unguard, "_kernelrpc_mach_port_unguard_trap"}},
            {50, {sogen::mach_traps::trap_thread_get_special_reply_port, "thread_get_special_reply_port"}},
            {76, {sogen::mach_traps::trap_port_type, "_kernelrpc_mach_port_type_trap"}},
            // Registered by the XPC service task: libxpc watches every looked-up service port with a
            // dead-name registration, and a connection setup without it never completes.
            {77, {sogen::mach_traps::trap_request_notification, "_kernelrpc_mach_port_request_notification_trap"}},
            {43, {sogen::mach_traps::trap_generate_activity_id, "mach_generate_activity_id"}},
            {91, {sogen::mach_traps::trap_mk_timer_create, "mk_timer_create"}},
            {92, {sogen::mach_traps::trap_mk_timer_destroy, "mk_timer_destroy"}},
            {93, {sogen::mach_traps::trap_mk_timer_arm, "mk_timer_arm"}},
            {94, {sogen::mach_traps::trap_mk_timer_cancel, "mk_timer_cancel"}},
            {95, {sogen::mach_traps::trap_mk_timer_arm_leeway, "mk_timer_arm_leeway"}},
        };

        for (const auto& [index, entry] : expected)
        {
            const auto* registered = dispatcher.get_mach_trap_entry(index);
            ASSERT_NE(registered, nullptr) << "index " << index;
            EXPECT_EQ(registered->handler, entry.first) << "index " << index;
            EXPECT_EQ(registered->name, entry.second) << "index " << index;
        }

        for (const auto index : {17u, 49u, 51u, 75u})
        {
            const auto* registered = dispatcher.get_mach_trap_entry(index);
            ASSERT_NE(registered, nullptr) << "index " << index;
            EXPECT_EQ(registered->handler, nullptr) << "index " << index << " is not this task's to register";
        }
    }

    constexpr uint64_t activity_scratch = 0x330000000ULL;

    // os_activity asks the kernel for a run of identifiers and then uses that run without asking again,
    // so the counter has to advance by the whole count rather than by one. Two calls that overlap would
    // put the same id on unrelated log records.
    TEST(MachTraps, GenerateActivityIdHandsOutDisjointRuns)
    {
        const auto emu = macos_test::make_emulator();
        ASSERT_TRUE(emu->memory.allocate_memory(activity_scratch, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        const auto request = [&](const uint64_t count, const uint64_t out) {
            emu->emu().reg(sogen::arm64_register::x0, uint64_t{0});
            emu->emu().reg(sogen::arm64_register::x1, count);
            emu->emu().reg(sogen::arm64_register::x2, out);
            sogen::mach_traps::trap_generate_activity_id({.emu_ref = *emu, .emu = emu->emu(), .proc = emu->process});
            return emu->emu().reg(sogen::arm64_register::x0);
        };

        uint64_t first = 0;
        ASSERT_EQ(request(4, activity_scratch), sogen::mach::kr::success);
        emu->memory.read_memory(activity_scratch, &first, sizeof(first));
        EXPECT_NE(first, 0u) << "zero is os_activity's no-activity sentinel";

        uint64_t second = 0;
        ASSERT_EQ(request(4, activity_scratch + 8), sogen::mach::kr::success);
        emu->memory.read_memory(activity_scratch + 8, &second, sizeof(second));

        EXPECT_EQ(second, first + 4) << "the second run starts past the whole of the first";
    }

    TEST(MachTraps, GenerateActivityIdRefusesAnEmptyRunOrANullPointer)
    {
        const auto emu = macos_test::make_emulator();
        ASSERT_TRUE(emu->memory.allocate_memory(activity_scratch, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        const auto request = [&](const uint64_t count, const uint64_t out) {
            emu->emu().reg(sogen::arm64_register::x0, uint64_t{0});
            emu->emu().reg(sogen::arm64_register::x1, count);
            emu->emu().reg(sogen::arm64_register::x2, out);
            sogen::mach_traps::trap_generate_activity_id({.emu_ref = *emu, .emu = emu->emu(), .proc = emu->process});
            return emu->emu().reg(sogen::arm64_register::x0);
        };

        EXPECT_EQ(request(0, activity_scratch), sogen::mach::kr::invalid_argument);
        EXPECT_EQ(request(4, 0), sogen::mach::kr::invalid_argument);
        EXPECT_EQ(request(4, 0x900000000ull), sogen::mach::kr::invalid_address);
    }
}
