#include <gtest/gtest.h>

#include "macos_test_utils.hpp"

#include <mach/mach_exception.hpp>

#include <algorithm>
#include <memory>

namespace
{
    constexpr uint64_t code_base = 0x100000000ULL;

    std::unique_ptr<sogen::macos_emulator> round_trip(const sogen::macos_emulator& emu)
    {
        sogen::utils::buffer_serializer serializer{};
        emu.serialize(serializer, false);

        auto restored = macos_test::make_emulator();
        sogen::utils::buffer_deserializer deserializer{serializer.get_buffer()};
        restored->deserialize(deserializer, false);
        return restored;
    }

    // mach_kernel::serialize had no caller for the whole of Stage 4: every emulator snapshot silently
    // dropped the port namespace, and the mach_kernel-only round-trip test kept passing regardless.
    // This one goes through macos_emulator::serialize, which is the path a real snapshot takes.
    TEST(MachSnapshot, MachStateTravelsInAnEmulatorSnapshot)
    {
        const auto emu = macos_test::make_emulator();

        const auto worker_port = emu->mach.thread_self_for(9);
        const auto semaphore = emu->mach.create_semaphore(0, 3);
        emu->mach.all_image_info_address = 0x7FF000;
        emu->mach.all_image_info_size = 0x40;
        ASSERT_EQ(emu->mach.set_task_special_port(sogen::mach::task_special_port::access, emu->mach.host_self), sogen::mach::kr::success);

        const auto restored = round_trip(*emu);

        EXPECT_EQ(restored->mach.task_self, emu->mach.task_self);
        EXPECT_EQ(restored->mach.bootstrap, emu->mach.bootstrap);
        EXPECT_EQ(restored->mach.ports.live_port_count(), emu->mach.ports.live_port_count());
        EXPECT_EQ(restored->mach.thread_self_for(9), worker_port);
        EXPECT_EQ(restored->mach.get_task_special_port(sogen::mach::task_special_port::access), emu->mach.host_self);
        EXPECT_EQ(restored->mach.all_image_info_address, 0x7FF000u);
        EXPECT_EQ(restored->mach.all_image_info_size, 0x40u);

        const auto* entry = restored->mach.find_semaphore(semaphore);
        ASSERT_NE(entry, nullptr);
        EXPECT_EQ(entry->value, 3);
    }

    TEST(MachSnapshot, TheTimebaseTravelsRatherThanBeingRederived)
    {
        const auto emu = macos_test::make_emulator();
        emu->mach.timebase_numer = 125;
        emu->mach.timebase_denom = 3;

        const auto restored = round_trip(*emu);

        EXPECT_EQ(restored->mach.timebase_numer, 125u);
        EXPECT_EQ(restored->mach.timebase_denom, 3u);
    }

    TEST(MachSnapshot, ExceptionHandlersAndTheLastExceptionTravel)
    {
        const auto emu = macos_test::make_emulator();
        const auto port = emu->mach.ports.allocate_receive_right();

        ASSERT_EQ(emu->mach.exceptions.set_ports(false, 1u << sogen::mach::exception_type::bad_instruction, port,
                                                 sogen::mach::exception_behavior::defaults | sogen::mach::exception_behavior::mach_codes,
                                                 7),
                  sogen::mach::kr::success);

        macos_test::write_guest_code(*emu, code_base, {0x00000000}); // udf #0
        emu->start(4);
        ASSERT_TRUE(emu->mach.last_exception.has_value());

        const auto restored = round_trip(*emu);

        const auto handler = restored->mach.exceptions.find_handler(sogen::mach::exception_type::bad_instruction);
        ASSERT_TRUE(handler.has_value());
        EXPECT_EQ(handler->port, port);
        EXPECT_EQ(handler->flavor, 7);
        EXPECT_EQ(handler->behavior, sogen::mach::exception_behavior::defaults | sogen::mach::exception_behavior::mach_codes);
        EXPECT_FALSE(restored->mach.exceptions.find_handler(sogen::mach::exception_type::arithmetic).has_value());

        ASSERT_TRUE(restored->mach.last_exception.has_value());
        EXPECT_EQ(restored->mach.last_exception->type, emu->mach.last_exception->type);
        EXPECT_EQ(restored->mach.last_exception->signal, emu->mach.last_exception->signal);
        EXPECT_EQ(restored->mach.last_exception->pc, emu->mach.last_exception->pc);
        EXPECT_EQ(restored->mach.last_exception->delivered, emu->mach.last_exception->delivered);
    }

    TEST(MachSnapshot, AnUndeliveredMessageStillOnAPortQueueTravels)
    {
        const auto emu = macos_test::make_emulator();
        const auto port = emu->mach.ports.allocate_receive_right();

        emu->mach.exceptions.set_ports(false, 1u << sogen::mach::exception_type::bad_instruction, port,
                                       sogen::mach::exception_behavior::defaults, 0);

        macos_test::write_guest_code(*emu, code_base, {0x00000000}); // udf #0
        emu->start(4);
        ASSERT_EQ(emu->mach.ports.find(port)->queue.size(), 1u);

        const auto restored = round_trip(*emu);

        const auto* entry = restored->mach.ports.find(port);
        ASSERT_NE(entry, nullptr);
        ASSERT_EQ(entry->queue.size(), 1u) << "a message the guest has not received yet is guest state";
        EXPECT_EQ(entry->queue.front(), emu->mach.ports.find(port)->queue.front());
    }

    // The snapshot gained the whole mach subsystem, so a v1 payload can no longer be read as one.
    TEST(MachSnapshot, ASnapshotFromTheOlderFormatIsRejected)
    {
        sogen::utils::buffer_serializer serializer{};
        serializer.write(std::string{"macos-emulator-state-v1"});

        const auto restored = macos_test::make_emulator();
        sogen::utils::buffer_deserializer deserializer{serializer.get_buffer()};
        EXPECT_THROW(restored->deserialize(deserializer, false), std::runtime_error);
    }

    // Stage 4 freezes this format, so a restored emulator has to re-serialize to the same bytes.
    // Anything that does not (an unordered container, an address, an uninitialised pad) would make
    // snapshots taken by one build unreadable by the next for reasons nothing else would surface.
    TEST(MachSnapshot, SerializingTwiceProducesIdenticalBytes)
    {
        const auto emu = macos_test::make_emulator();
        emu->mach.thread_self_for(4);
        emu->mach.create_semaphore(0, 1);
        emu->mach.create_voucher();
        emu->mach.clock_service(0);
        emu->mach.exceptions.set_ports(false, 1u << sogen::mach::exception_type::bad_access, emu->mach.ports.allocate_receive_right(),
                                       sogen::mach::exception_behavior::defaults, 0);

        sogen::utils::buffer_serializer first{};
        emu->serialize(first, false);

        const auto restored = round_trip(*emu);

        sogen::utils::buffer_serializer second{};
        restored->serialize(second, false);

        EXPECT_EQ(first.get_buffer(), second.get_buffer());
    }
}
