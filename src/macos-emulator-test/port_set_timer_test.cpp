#include <gtest/gtest.h>

#include "macos_test_utils.hpp"

#include <mach/mach_kernel.hpp>
#include <mach/mach_msg.hpp>
#include <mach/mach_port_namespace.hpp>
#include <macos_kqueue.hpp>

namespace
{
    using namespace sogen;
    using namespace sogen::mach;

    port_name_t queue_a_message(macos_emulator& emu, const port_name_t port, const int32_t id)
    {
        auto* entry = emu.mach.ports.find(port);
        std::vector<uint8_t> message(MSG_HEADER_SIZE, 0);
        write_msg_header(message, msg_header{.bits = 0, .size = MSG_HEADER_SIZE, .remote_port = PORT_NULL, .local_port = port, .id = id});
        entry->queue.push_back(std::move(message));
        return port;
    }

    TEST(PortSets, AMemberCanBelongToSeveralSetsAtOnce)
    {
        mach_port_namespace ports{};
        const auto member = ports.allocate_receive_right();
        const auto first = ports.allocate_port_set();
        const auto second = ports.allocate_port_set();

        ASSERT_EQ(ports.insert_member(member, first), kr::success);
        ASSERT_EQ(ports.insert_member(member, second), kr::success);

        const auto sets = ports.sets_containing(member);
        ASSERT_EQ(sets.size(), 2u);
        EXPECT_NE(std::ranges::find(sets, first), sets.end());
        EXPECT_NE(std::ranges::find(sets, second), sets.end());
    }

    // move_member is the "leave every other set" form; insert_member is not. Collapsing the two would
    // silently unhook a run loop's other modes.
    TEST(PortSets, MoveMemberDetachesFromTheOtherSetsAndInsertMemberDoesNot)
    {
        mach_port_namespace ports{};
        const auto member = ports.allocate_receive_right();
        const auto first = ports.allocate_port_set();
        const auto second = ports.allocate_port_set();

        ASSERT_EQ(ports.insert_member(member, first), kr::success);
        ASSERT_EQ(ports.move_member(member, second), kr::success);

        const auto sets = ports.sets_containing(member);
        ASSERT_EQ(sets.size(), 1u);
        EXPECT_EQ(sets.front(), second);
    }

    TEST(PortSets, InsertingTheSameMemberTwiceIsRefused)
    {
        mach_port_namespace ports{};
        const auto member = ports.allocate_receive_right();
        const auto set = ports.allocate_port_set();

        ASSERT_EQ(ports.insert_member(member, set), kr::success);
        EXPECT_EQ(ports.insert_member(member, set), kr::already_in_set);
        EXPECT_EQ(ports.sets_containing(member).size(), 1u);
    }

    TEST(PortSets, InsertAndExtractRejectTheWrongKindOfName)
    {
        mach_port_namespace ports{};
        const auto member = ports.allocate_receive_right();
        const auto other = ports.allocate_receive_right();
        const auto set = ports.allocate_port_set();

        EXPECT_EQ(ports.insert_member(member, other), kr::invalid_right);
        EXPECT_EQ(ports.insert_member(set, set), kr::invalid_right);
        EXPECT_EQ(ports.insert_member(0x1234, set), kr::invalid_name);
        EXPECT_EQ(ports.insert_member(member, 0x1234), kr::invalid_name);
        EXPECT_EQ(ports.extract_member(member, set), kr::not_in_set);
        EXPECT_EQ(ports.extract_member(member, other), kr::invalid_right);
    }

    TEST(PortSets, ExtractRemovesOnlyTheNamedSet)
    {
        mach_port_namespace ports{};
        const auto member = ports.allocate_receive_right();
        const auto first = ports.allocate_port_set();
        const auto second = ports.allocate_port_set();

        ASSERT_EQ(ports.insert_member(member, first), kr::success);
        ASSERT_EQ(ports.insert_member(member, second), kr::success);
        ASSERT_EQ(ports.extract_member(member, first), kr::success);

        const auto sets = ports.sets_containing(member);
        ASSERT_EQ(sets.size(), 1u);
        EXPECT_EQ(sets.front(), second);
    }

    TEST(PortSets, FirstQueuedMemberFollowsInsertionOrderAndSkipsEmptyPorts)
    {
        const auto emu = macos_test::make_emulator();
        auto& ports = emu->mach.ports;

        const auto empty = ports.allocate_receive_right();
        const auto loaded = ports.allocate_receive_right();
        const auto also_loaded = ports.allocate_receive_right();
        const auto set = ports.allocate_port_set();

        ASSERT_EQ(ports.insert_member(empty, set), kr::success);
        ASSERT_EQ(ports.insert_member(loaded, set), kr::success);
        ASSERT_EQ(ports.insert_member(also_loaded, set), kr::success);

        queue_a_message(*emu, loaded, 7);
        queue_a_message(*emu, also_loaded, 9);

        const auto* first = ports.first_queued_member(set);
        ASSERT_NE(first, nullptr);
        EXPECT_EQ(first->port_id, ports.find(loaded)->port_id);
    }

    TEST(PortSets, FirstQueuedMemberIsNullForAnEmptySetOrANonSetName)
    {
        mach_port_namespace ports{};
        const auto member = ports.allocate_receive_right();
        const auto set = ports.allocate_port_set();
        ASSERT_EQ(ports.insert_member(member, set), kr::success);

        EXPECT_EQ(ports.first_queued_member(set), nullptr);
        EXPECT_EQ(ports.first_queued_member(member), nullptr);
        EXPECT_EQ(ports.first_queued_member(0x1234), nullptr);
    }

    TEST(PortSets, DestroyingAMemberUnhooksItFromItsSets)
    {
        mach_port_namespace ports{};
        const auto member = ports.allocate_receive_right();
        const auto set = ports.allocate_port_set();
        ASSERT_EQ(ports.insert_member(member, set), kr::success);

        ASSERT_EQ(ports.mod_refs(member, right_kind::receive, -1), kr::success);
        EXPECT_EQ(ports.first_queued_member(set), nullptr);

        const auto fresh = ports.allocate_receive_right();
        EXPECT_TRUE(ports.sets_containing(fresh).empty());
    }

    TEST(MkTimer, ArmReportsWhetherADeadlineWasAlreadySetAndCancelReturnsIt)
    {
        mach_kernel kernel{};
        kernel.setup(MACH_MAIN_THREAD_ID);

        const auto timer = kernel.create_timer();
        ASSERT_NE(timer, PORT_NULL);

        EXPECT_FALSE(kernel.arm_timer(timer, 5000));
        EXPECT_TRUE(kernel.arm_timer(timer, 9000));
        EXPECT_EQ(kernel.cancel_timer(timer), 9000u);
        EXPECT_EQ(kernel.cancel_timer(timer), 0u);
        EXPECT_FALSE(kernel.earliest_armed_timer().has_value());
    }

    TEST(MkTimer, TheEarliestDeadlineWins)
    {
        mach_kernel kernel{};
        kernel.setup(MACH_MAIN_THREAD_ID);

        const auto late = kernel.create_timer();
        const auto early = kernel.create_timer();
        kernel.arm_timer(late, 900);
        kernel.arm_timer(early, 100);

        const auto next = kernel.earliest_armed_timer();
        ASSERT_TRUE(next.has_value());
        EXPECT_EQ(next->name, early);
        EXPECT_EQ(next->deadline, 100u);

        kernel.disarm_timer(early);
        const auto after = kernel.earliest_armed_timer();
        ASSERT_TRUE(after.has_value());
        EXPECT_EQ(after->name, late);
    }

    TEST(MkTimer, OnlyATimerPortCanBeArmedOrDestroyed)
    {
        mach_kernel kernel{};
        kernel.setup(MACH_MAIN_THREAD_ID);

        const auto plain = kernel.ports.allocate_receive_right();
        EXPECT_FALSE(kernel.arm_timer(plain, 100));
        EXPECT_FALSE(kernel.earliest_armed_timer().has_value());
        EXPECT_EQ(kernel.destroy_timer(plain), kr::invalid_argument);

        const auto timer = kernel.create_timer();
        kernel.arm_timer(timer, 100);
        EXPECT_EQ(kernel.destroy_timer(timer), kr::success);
        EXPECT_FALSE(kernel.earliest_armed_timer().has_value());
    }

    // xnu's mk_timer_expire sends a bare header with msgh_id zero; the receiver identifies the timer by
    // msgh_local_port, which is the only field CFRunLoop reads.
    TEST(MkTimer, ExpirationQueuesABareHeaderNamingTheTimerPort)
    {
        const auto emu = macos_test::make_emulator();
        const auto timer = emu->mach.create_timer();

        deliver_timer_expiration(*emu, timer);

        const auto* entry = emu->mach.ports.find(timer);
        ASSERT_NE(entry, nullptr);
        ASSERT_EQ(entry->queue.size(), 1u);

        const auto header = read_msg_header(entry->queue.front());
        EXPECT_EQ(entry->queue.front().size(), MSG_HEADER_SIZE);
        EXPECT_EQ(header.size, MSG_HEADER_SIZE);
        EXPECT_EQ(header.local_port, timer);
        EXPECT_EQ(header.remote_port, PORT_NULL);
        EXPECT_EQ(header.id, 0);
        EXPECT_EQ(header.bits, 0u);
    }

    TEST(MkTimer, ExpiringAnUnknownPortQueuesNothing)
    {
        const auto emu = macos_test::make_emulator();
        const auto before = emu->mach.ports.non_empty_queues().size();

        deliver_timer_expiration(*emu, 0x4321);

        EXPECT_EQ(emu->mach.ports.non_empty_queues().size(), before);
    }

    class UserFilter : public testing::Test
    {
      protected:
        guest_fd_table fds{};
        macos_kqueue_table kqueues{fds};
        uint64_t kq{};

        void SetUp() override
        {
            this->kq = this->kqueues.create();
        }

        size_t apply(const kevent_registration& change)
        {
            size_t fired = 0;
            EXPECT_TRUE(this->kqueues.apply_changes(this->kq, &change, 1, &fired));
            return fired;
        }

        std::vector<kevent_registration> drain(const size_t max = 8)
        {
            std::vector<kevent_registration> events(max);
            const auto count = this->kqueues.deliver(this->kq, events.data(), events.size());
            events.resize(count);
            return events;
        }
    };

    // A changelist entry with no EV_ADD is a touch, and a touch of a knote that does not exist is
    // ENOENT in xnu -- never an implicit registration.
    TEST_F(UserFilter, ATouchOfAnUnregisteredKnoteFiresNothing)
    {
        EXPECT_EQ(this->apply({.filter = MACOS_EVFILT_USER, .ident = 99, .fflags = MACOS_NOTE_TRIGGER}), 0u);
        EXPECT_TRUE(this->drain().empty());
    }

    TEST_F(UserFilter, RegisteringWithoutTriggerFiresNothing)
    {
        EXPECT_EQ(this->apply({.filter = MACOS_EVFILT_USER, .ident = 1, .flags = MACOS_EV_ADD | MACOS_EV_CLEAR}), 0u);
        EXPECT_TRUE(this->drain().empty());
    }

    TEST_F(UserFilter, TriggerDeliversTheStoredFlagsAndData)
    {
        this->apply({.filter = MACOS_EVFILT_USER,
                     .ident = 1,
                     .flags = MACOS_EV_ADD | MACOS_EV_CLEAR,
                     .fflags = MACOS_NOTE_FFCOPY | 0x123,
                     .data = 0x99,
                     .udata = 0xABCD});

        EXPECT_EQ(this->apply({.filter = MACOS_EVFILT_USER, .ident = 1, .fflags = MACOS_NOTE_TRIGGER}), 1u);

        const auto events = this->drain();
        ASSERT_EQ(events.size(), 1u);
        EXPECT_EQ(events.front().filter, MACOS_EVFILT_USER);
        EXPECT_EQ(events.front().ident, 1u);
        EXPECT_EQ(events.front().fflags, 0x123u);
        EXPECT_EQ(events.front().udata, 0xABCDu);
    }

    TEST_F(UserFilter, TheControlBitsFoldTheUserFlagsTheWayFiltUserTouchDoes)
    {
        EXPECT_EQ(macos_kqueue_table::combine_user_fflags(0xF0F0, MACOS_NOTE_FFNOP | 0x0F0F), 0xF0F0u);
        EXPECT_EQ(macos_kqueue_table::combine_user_fflags(0xF0F0, MACOS_NOTE_FFOR | 0x0F0F), 0xFFFFu);
        EXPECT_EQ(macos_kqueue_table::combine_user_fflags(0xFFFF, MACOS_NOTE_FFAND | 0x0F0F), 0x0F0Fu);
        EXPECT_EQ(macos_kqueue_table::combine_user_fflags(0xF0F0, MACOS_NOTE_FFCOPY | 0x0F0F), 0x0F0Fu);

        // NOTE_TRIGGER lives outside NOTE_FFLAGSMASK, so it never reaches the stored set.
        EXPECT_EQ(macos_kqueue_table::combine_user_fflags(0, MACOS_NOTE_FFCOPY | MACOS_NOTE_TRIGGER | 0x7), 0x7u);
    }

    TEST_F(UserFilter, ASecondTriggerBeforeDeliveryCoalescesIntoTheFirst)
    {
        this->apply({.filter = MACOS_EVFILT_USER, .ident = 2, .flags = MACOS_EV_ADD | MACOS_EV_CLEAR});

        EXPECT_EQ(this->apply({.filter = MACOS_EVFILT_USER, .ident = 2, .fflags = MACOS_NOTE_FFOR | MACOS_NOTE_TRIGGER | 0x1}), 1u);
        EXPECT_EQ(this->apply({.filter = MACOS_EVFILT_USER, .ident = 2, .fflags = MACOS_NOTE_FFOR | MACOS_NOTE_TRIGGER | 0x2}), 0u);

        const auto events = this->drain();
        ASSERT_EQ(events.size(), 1u);
        EXPECT_EQ(events.front().fflags, 0x3u);
    }

    TEST_F(UserFilter, AClearedKnoteStopsAfterOneDeliveryAndALevelTriggeredOneDoesNot)
    {
        this->apply({.filter = MACOS_EVFILT_USER, .ident = 3, .flags = MACOS_EV_ADD | MACOS_EV_CLEAR});
        this->apply({.filter = MACOS_EVFILT_USER, .ident = 3, .fflags = MACOS_NOTE_TRIGGER});
        ASSERT_EQ(this->drain().size(), 1u);
        EXPECT_TRUE(this->drain().empty());

        this->apply({.filter = MACOS_EVFILT_USER, .ident = 4, .flags = MACOS_EV_ADD});
        this->apply({.filter = MACOS_EVFILT_USER, .ident = 4, .fflags = MACOS_NOTE_TRIGGER});
        ASSERT_EQ(this->drain().size(), 1u);
        EXPECT_EQ(this->drain().size(), 1u);
    }

    TEST_F(UserFilter, OneShotDeletesTheKnoteAndDeleteDropsAPendingEvent)
    {
        this->apply({.filter = MACOS_EVFILT_USER, .ident = 5, .flags = MACOS_EV_ADD | MACOS_EV_ONESHOT});
        this->apply({.filter = MACOS_EVFILT_USER, .ident = 5, .fflags = MACOS_NOTE_TRIGGER});
        ASSERT_EQ(this->drain().size(), 1u);
        EXPECT_EQ(this->apply({.filter = MACOS_EVFILT_USER, .ident = 5, .fflags = MACOS_NOTE_TRIGGER}), 0u);

        this->apply({.filter = MACOS_EVFILT_USER, .ident = 6, .flags = MACOS_EV_ADD | MACOS_EV_CLEAR});
        this->apply({.filter = MACOS_EVFILT_USER, .ident = 6, .fflags = MACOS_NOTE_TRIGGER});
        this->apply({.filter = MACOS_EVFILT_USER, .ident = 6, .flags = MACOS_EV_DELETE});

        const auto events = this->drain();
        EXPECT_EQ(std::ranges::count_if(events, [](const kevent_registration& event) { return event.ident == 6; }), 0);
    }
}
