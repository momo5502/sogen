#include <gtest/gtest.h>

#include <mach/mach_port_namespace.hpp>

#include <map>
#include <optional>
#include <stdexcept>
#include <vector>

namespace
{
    using namespace sogen::mach; // NOLINT(google-build-using-namespace) // NOLINT(google-build-using-namespace): the tree's own namespace,
                                 // pulled into a test-local anonymous namespace

    using sogen::utils::buffer_deserializer;
    using sogen::utils::buffer_serializer;

    TEST(MachPortNamespace, FirstNamesUseAscendingIndicesAndGenerationOne)
    {
        mach_port_namespace ports{};

        const auto first = ports.allocate_receive_right();
        const auto second = ports.allocate_receive_right();

        EXPECT_EQ(port_index(first), 1u);
        EXPECT_EQ(port_index(second), 2u);
        EXPECT_NE(port_generation(first), 0);
        EXPECT_TRUE(ports.exists(first));
        EXPECT_TRUE(ports.exists(second));
    }

    TEST(MachPortNamespace, ReusedIndexGetsAFreshGeneration)
    {
        mach_port_namespace ports{};

        const auto first = ports.allocate_receive_right();
        ASSERT_EQ(ports.mod_refs(first, right_kind::receive, -1), kr::success);
        EXPECT_FALSE(ports.exists(first));

        const auto reused = ports.allocate_receive_right();

        EXPECT_EQ(port_index(reused), port_index(first));
        EXPECT_NE(reused, first) << "a reused index must not produce the same name; the generation is observable";
        EXPECT_FALSE(ports.exists(first));
        EXPECT_TRUE(ports.exists(reused));
    }

    TEST(MachPortNamespace, StaleNamesCannotReachThePortThatReplacedThem)
    {
        mach_port_namespace ports{};

        const auto first = ports.allocate_receive_right({.kind = kernel_object_kind::host, .id = 7});
        ASSERT_EQ(ports.mod_refs(first, right_kind::receive, -1), kr::success);
        const auto reused = ports.allocate_receive_right({.kind = kernel_object_kind::task, .id = 9});

        ASSERT_EQ(port_index(reused), port_index(first));
        ASSERT_EQ(port_generation(reused), port_generation(first) + 1) << "the reuse is exactly one generation away";

        EXPECT_EQ(ports.find(first), nullptr);
        EXPECT_EQ(ports.type_of(first), 0u);
        EXPECT_EQ(ports.object_of(first).kind, kernel_object_kind::none);
        EXPECT_EQ(ports.mod_refs(first, right_kind::receive, -1), kr::invalid_name);
        EXPECT_EQ(ports.deallocate(first), kr::invalid_name);
        EXPECT_EQ(ports.guard(first, 1, false), kr::invalid_name);
        EXPECT_EQ(ports.destruct(first, 0, 0), kr::invalid_name);
        EXPECT_EQ(ports.insert_send_right(first), PORT_NULL);

        EXPECT_TRUE(ports.exists(reused)) << "the stale name must not have reached the port that replaced it";
        EXPECT_EQ(ports.object_of(reused).kind, kernel_object_kind::task);
        EXPECT_EQ(ports.object_of(reused).id, 9u);
    }

    TEST(MachPortNamespace, GenerationsWrapPastZeroWithoutEverBeingZero)
    {
        mach_port_namespace ports{};

        constexpr uint32_t cycles = 512;
        for (uint32_t i = 0; i < cycles; ++i)
        {
            const auto name = ports.allocate_receive_right();
            ASSERT_EQ(port_index(name), 1u) << "iteration " << i;
            ASSERT_NE(port_generation(name), 0) << "iteration " << i;
            ASSERT_EQ(port_generation(name), (i % 255) + 1) << "iteration " << i;
            ASSERT_EQ(ports.mod_refs(name, right_kind::receive, -1), kr::success) << "iteration " << i;
        }
    }

    TEST(MachPortNamespace, FreedIndicesAreHandedBackLowestFirst)
    {
        mach_port_namespace ports{};

        const auto first = ports.allocate_receive_right();
        const auto second = ports.allocate_receive_right();
        const auto third = ports.allocate_receive_right();
        ASSERT_EQ(port_index(third), 3u);

        ASSERT_EQ(ports.mod_refs(third, right_kind::receive, -1), kr::success);
        ASSERT_EQ(ports.mod_refs(first, right_kind::receive, -1), kr::success);

        EXPECT_EQ(port_index(ports.allocate_receive_right()), 1u);
        EXPECT_EQ(port_index(ports.allocate_receive_right()), 3u);
        EXPECT_EQ(port_index(ports.allocate_receive_right()), 4u);
        EXPECT_TRUE(ports.exists(second));
    }

    TEST(MachPortNamespace, SendRightsAreRefCountedAndDieAtZero)
    {
        mach_port_namespace ports{};
        const auto receive = ports.allocate_receive_right();
        const auto send = ports.insert_send_right(receive);

        EXPECT_EQ(send, receive) << "a send right on an owned receive right shares the name";
        ASSERT_EQ(ports.mod_refs(send, right_kind::send, 2), kr::success);
        ASSERT_NE(ports.find(send), nullptr);
        EXPECT_EQ(ports.find(send)->send_urefs, 3u);
        ASSERT_EQ(ports.mod_refs(send, right_kind::send, -3), kr::success);
        EXPECT_EQ(ports.find(send)->send_urefs, 0u);
        EXPECT_TRUE(ports.exists(receive)) << "dropping send rights must not destroy the receive right";
    }

    TEST(MachPortNamespace, SendRightCountsRefuseToGoNegative)
    {
        mach_port_namespace ports{};
        const auto receive = ports.allocate_receive_right();
        const auto send = ports.insert_send_right(receive);
        ASSERT_NE(ports.find(send), nullptr);

        EXPECT_EQ(ports.mod_refs(send, right_kind::send, -2), kr::invalid_value);
        EXPECT_EQ(ports.find(send)->send_urefs, 1u) << "a rejected delta must not be applied";

        ASSERT_EQ(ports.mod_refs(send, right_kind::send, -1), kr::success);
        EXPECT_EQ(ports.type_of(receive) & 0x10000u, 0u) << "a send count of zero is no longer a send right";
        EXPECT_EQ(ports.mod_refs(send, right_kind::send, 1), kr::invalid_right);
    }

    TEST(MachPortNamespace, RightsWithoutUserRefsOnlyAcceptZeroOrMinusOne)
    {
        mach_port_namespace ports{};
        const auto receive = ports.allocate_receive_right();
        const auto once = ports.allocate_send_once_right(receive);
        const auto set = ports.allocate_port_set();

        EXPECT_EQ(ports.mod_refs(receive, right_kind::receive, 1), kr::invalid_value);
        EXPECT_EQ(ports.mod_refs(receive, right_kind::receive, -2), kr::invalid_value);
        EXPECT_EQ(ports.mod_refs(receive, right_kind::receive, 0), kr::success);
        EXPECT_TRUE(ports.exists(receive)) << "a rejected or zero delta must leave the receive right alone";

        EXPECT_EQ(ports.mod_refs(once, right_kind::send_once, 1), kr::invalid_value);
        EXPECT_EQ(ports.mod_refs(once, right_kind::send_once, 0), kr::success);
        EXPECT_TRUE(ports.exists(once));

        EXPECT_EQ(ports.mod_refs(set, right_kind::port_set, -2), kr::invalid_value);
        EXPECT_EQ(ports.mod_refs(set, right_kind::port_set, 0), kr::success);
        EXPECT_TRUE(ports.exists(set));

        EXPECT_EQ(ports.mod_refs(set, right_kind::receive, 0), kr::invalid_right);
        EXPECT_EQ(ports.mod_refs(receive, right_kind::port_set, 0), kr::invalid_right);
        EXPECT_EQ(ports.mod_refs(receive, right_kind::send_once, 0), kr::invalid_right);
        EXPECT_EQ(ports.mod_refs(receive, right_kind::dead_name, 0), kr::invalid_right);
    }

    TEST(MachPortNamespace, UnknownNamesAreRejectedNotInvented)
    {
        mach_port_namespace ports{};

        EXPECT_FALSE(ports.exists(0x9999));
        EXPECT_EQ(ports.mod_refs(0x9999, right_kind::send, -1), kr::invalid_name);
        EXPECT_EQ(ports.deallocate(0x9999), kr::invalid_name);
        EXPECT_EQ(ports.mod_refs(PORT_NULL, right_kind::send, -1), kr::invalid_name);
        EXPECT_EQ(ports.type_of(0x9999), 0u);
    }

    TEST(MachPortNamespace, ReservedNamesNeverResolve)
    {
        mach_port_namespace ports{};
        const auto receive = ports.allocate_receive_right();

        EXPECT_EQ(ports.find(PORT_NULL), nullptr);
        EXPECT_EQ(ports.find(PORT_DEAD), nullptr);
        EXPECT_EQ(ports.type_of(PORT_NULL), 0u);
        EXPECT_EQ(ports.type_of(PORT_DEAD), 0u);
        EXPECT_EQ(ports.insert_send_right(PORT_NULL), PORT_NULL);
        EXPECT_EQ(ports.allocate_send_once_right(PORT_DEAD), PORT_NULL);
        EXPECT_EQ(ports.mod_refs(receive, static_cast<right_kind>(99), -1), kr::invalid_value);
    }

    TEST(MachPortNamespace, KernelObjectsAreRecoverableFromTheName)
    {
        mach_port_namespace ports{};
        const auto task = ports.allocate_receive_right({.kind = kernel_object_kind::task, .id = 1});
        const auto plain = ports.allocate_receive_right();

        EXPECT_EQ(ports.object_of(task).kind, kernel_object_kind::task);
        EXPECT_EQ(ports.object_of(plain).kind, kernel_object_kind::none);
        EXPECT_EQ(ports.object_of(0x9999).kind, kernel_object_kind::none);
    }

    TEST(MachPortNamespace, SendOnceRightsAreDistinctNames)
    {
        mach_port_namespace ports{};
        const auto receive = ports.allocate_receive_right();

        const auto once = ports.allocate_send_once_right(receive);

        EXPECT_NE(once, receive);
        ASSERT_NE(ports.find(once), nullptr);
        EXPECT_TRUE(ports.find(once)->has_send_once);
        EXPECT_EQ(ports.type_of(once), 0x40000u);
        EXPECT_EQ(ports.type_of(receive) & 0x20000u, 0x20000u);
    }

    TEST(MachPortNamespace, SendOnceRightsAreConsumedOnce)
    {
        mach_port_namespace ports{};
        const auto receive = ports.allocate_receive_right({.kind = kernel_object_kind::bootstrap, .id = 3});
        const auto once = ports.allocate_send_once_right(receive);

        EXPECT_EQ(ports.object_of(once).kind, kernel_object_kind::bootstrap) << "a send-once right names the object its target names";

        EXPECT_EQ(ports.mod_refs(once, right_kind::send_once, -2), kr::invalid_value);
        EXPECT_EQ(ports.mod_refs(once, right_kind::send, -1), kr::invalid_right);
        ASSERT_EQ(ports.mod_refs(once, right_kind::send_once, -1), kr::success);

        EXPECT_FALSE(ports.exists(once));
        EXPECT_TRUE(ports.exists(receive)) << "consuming a send-once right must not touch the receive right";
        EXPECT_EQ(ports.mod_refs(once, right_kind::send_once, -1), kr::invalid_name);
        EXPECT_EQ(ports.allocate_send_once_right(0x9999), PORT_NULL);
    }

    TEST(MachPortNamespace, ASendOnceRightRemembersWhichPortItTargets)
    {
        mach_port_namespace ports{};
        const auto first = ports.allocate_receive_right({.kind = kernel_object_kind::bootstrap, .id = 3});
        const auto second = ports.allocate_receive_right({.kind = kernel_object_kind::task, .id = 4});

        const auto once = ports.allocate_send_once_right(second);

        ASSERT_NE(ports.find(once), nullptr);
        ASSERT_NE(ports.find(second), nullptr);
        EXPECT_EQ(ports.find(once)->send_once_target_id, ports.find(second)->port_id)
            << "the link must identify the target, not be copied from it";
        EXPECT_NE(ports.find(once)->send_once_target_id, static_cast<port_id_t>(second))
            << "the link is a port id, not a name that generation reuse can re-aim";
        EXPECT_EQ(ports.destination_of(once), ports.find(second));
        EXPECT_NE(ports.destination_of(once), ports.find(first));
        EXPECT_NE(ports.destination_of(once), ports.find(once)) << "a message to a send-once name lands on its target's queue";

        EXPECT_EQ(ports.destination_of(second), ports.find(second));
        EXPECT_EQ(ports.destination_of(once)->object.id, 4u);
    }

    TEST(MachPortNamespace, TwoSendOnceRightsToOnePortShareItsDestination)
    {
        mach_port_namespace ports{};
        const auto receive = ports.allocate_receive_right({.kind = kernel_object_kind::host, .id = 1});

        const auto first = ports.allocate_send_once_right(receive);
        const auto second = ports.allocate_send_once_right(receive);

        EXPECT_NE(first, second);
        EXPECT_EQ(ports.destination_of(first), ports.find(receive));
        EXPECT_EQ(ports.destination_of(second), ports.find(receive));

        ASSERT_EQ(ports.mod_refs(first, right_kind::send_once, -1), kr::success);
        EXPECT_EQ(ports.destination_of(second), ports.find(receive)) << "consuming one send-once right must not unlink the other";
    }

    TEST(MachPortNamespace, ASendOnceRightWhoseTargetDiedResolvesToNothing)
    {
        mach_port_namespace ports{};
        const auto receive = ports.allocate_receive_right({.kind = kernel_object_kind::bootstrap, .id = 3});
        const auto once = ports.allocate_send_once_right(receive);

        ASSERT_EQ(ports.mod_refs(receive, right_kind::receive, -1), kr::success);

        EXPECT_TRUE(ports.exists(once));
        EXPECT_EQ(ports.destination_of(once), nullptr);
        EXPECT_EQ(ports.object_of(once).kind, kernel_object_kind::none);
    }

    TEST(MachPortNamespace, ASendOnceLinkDoesNotFollowAReusedIndex)
    {
        mach_port_namespace ports{};
        const auto receive = ports.allocate_receive_right({.kind = kernel_object_kind::bootstrap, .id = 3});
        const auto once = ports.allocate_send_once_right(receive);

        ASSERT_EQ(ports.mod_refs(receive, right_kind::receive, -1), kr::success);
        const auto reused = ports.allocate_receive_right({.kind = kernel_object_kind::task, .id = 4});
        ASSERT_EQ(port_index(reused), port_index(receive)) << "the test needs the index back to be meaningful";

        EXPECT_EQ(ports.destination_of(once), nullptr) << "the generation in the stored name must reject the new occupant";
        EXPECT_EQ(ports.object_of(once).kind, kernel_object_kind::none);
    }

    TEST(MachPortNamespace, ASendOnceLinkSurvivesAGenerationWrapWithoutRetargeting)
    {
        mach_port_namespace ports{};
        const auto receive = ports.allocate_receive_right({.kind = kernel_object_kind::bootstrap, .id = 3});
        const auto once = ports.allocate_send_once_right(receive);
        ASSERT_EQ(ports.mod_refs(receive, right_kind::receive, -1), kr::success);

        auto reused = PORT_NULL;
        for (int cycle = 0; cycle < 255 && reused != receive; ++cycle)
        {
            if (reused != PORT_NULL)
            {
                ASSERT_EQ(ports.mod_refs(reused, right_kind::receive, -1), kr::success);
            }

            reused = ports.allocate_receive_right({.kind = kernel_object_kind::task, .id = 4});
            ASSERT_EQ(port_index(reused), port_index(receive)) << "the freed index must come straight back";
        }

        ASSERT_EQ(reused, receive) << "the 8-bit generation wraps back onto the original name in 255 cycles";
        EXPECT_TRUE(ports.exists(once));
        EXPECT_EQ(ports.destination_of(once), nullptr) << "the wrapped name must not re-aim the send-once link";
        EXPECT_EQ(ports.object_of(once).kind, kernel_object_kind::none);
    }

    TEST(MachPortNamespace, EveryPortCarriesADistinctMonotonicId)
    {
        mach_port_namespace ports{};

        const auto first = ports.allocate_receive_right();
        const auto second = ports.allocate_port_set();
        ASSERT_NE(ports.find(first), nullptr);
        ASSERT_NE(ports.find(second), nullptr);

        const auto first_id = ports.find(first)->port_id;
        EXPECT_NE(first_id, INVALID_PORT_ID);
        EXPECT_GT(ports.find(second)->port_id, first_id);

        ASSERT_EQ(ports.mod_refs(first, right_kind::receive, -1), kr::success);
        const auto reused = ports.allocate_receive_right();
        ASSERT_EQ(port_index(reused), port_index(first));
        ASSERT_NE(ports.find(reused), nullptr);
        EXPECT_GT(ports.find(reused)->port_id, ports.find(second)->port_id) << "a recycled index must not recycle the id";
    }

    TEST(MachPortNamespace, PortIdsResolveOnlyWhileTheirPortLives)
    {
        mach_port_namespace ports{};
        const auto receive = ports.allocate_receive_right({.kind = kernel_object_kind::host, .id = 5});
        ASSERT_NE(ports.find(receive), nullptr);
        const auto id = ports.find(receive)->port_id;

        EXPECT_EQ(ports.find_by_port_id(id), ports.find(receive));
        EXPECT_EQ(ports.find_by_port_id(INVALID_PORT_ID), nullptr);
        EXPECT_EQ(ports.find_by_port_id(id + 1000), nullptr);

        ASSERT_EQ(ports.mod_refs(receive, right_kind::receive, -1), kr::success);
        EXPECT_EQ(ports.find_by_port_id(id), nullptr) << "destroying a port must retire its id";

        const auto dead_holder = ports.allocate_receive_right();
        ASSERT_NE(ports.insert_send_right(dead_holder), PORT_NULL);
        ASSERT_NE(ports.find(dead_holder), nullptr);
        const auto dead_id = ports.find(dead_holder)->port_id;
        ASSERT_EQ(ports.mod_refs(dead_holder, right_kind::receive, -1), kr::success);
        EXPECT_EQ(ports.find_by_port_id(dead_id), ports.find(dead_holder)) << "a dead name is still the same port";
    }

    TEST(MachPortNamespace, DestinationOfRejectsNamesThatCannotReceive)
    {
        mach_port_namespace ports{};
        const auto receive = ports.allocate_receive_right();
        const auto set = ports.allocate_port_set();

        EXPECT_EQ(ports.destination_of(PORT_NULL), nullptr);
        EXPECT_EQ(ports.destination_of(PORT_DEAD), nullptr);
        EXPECT_EQ(ports.destination_of(0x9999), nullptr);
        EXPECT_EQ(ports.destination_of(set), nullptr) << "a port set is not a message destination";

        ASSERT_NE(ports.insert_send_right(receive), PORT_NULL);
        ASSERT_EQ(ports.mod_refs(receive, right_kind::receive, -1), kr::success);
        EXPECT_TRUE(ports.exists(receive)) << "the send right keeps the name alive as a dead name";
        EXPECT_EQ(ports.destination_of(receive), nullptr) << "a dead name has no queue to deliver to";
    }

    TEST(MachPortNamespace, TheSendOnceLinkSurvivesASnapshot)
    {
        mach_port_namespace ports{};

        // Recycle an index first, so the target's port id and its index are different numbers and a
        // snapshot that confuses the two cannot pass by coincidence.
        const auto recycled = ports.allocate_receive_right();
        ports.allocate_receive_right();
        ASSERT_EQ(ports.mod_refs(recycled, right_kind::receive, -1), kr::success);

        const auto receive = ports.allocate_receive_right({.kind = kernel_object_kind::bootstrap, .id = 3});
        const auto once = ports.allocate_send_once_right(receive);
        ASSERT_NE(ports.find(receive), nullptr);
        ASSERT_NE(ports.find(receive)->port_id, port_index(receive));

        buffer_serializer serializer{};
        ports.serialize(serializer);

        mach_port_namespace restored{};
        buffer_deserializer deserializer{serializer.get_buffer()};
        restored.deserialize(deserializer);

        ASSERT_NE(restored.find(once), nullptr);
        ASSERT_NE(restored.find(receive), nullptr);
        EXPECT_EQ(restored.find(receive)->port_id, ports.find(receive)->port_id) << "the port id is the link's only anchor";
        EXPECT_EQ(restored.find(once)->send_once_target_id, restored.find(receive)->port_id);
        EXPECT_EQ(restored.destination_of(once), restored.find(receive));
        EXPECT_EQ(restored.object_of(once).kind, kernel_object_kind::bootstrap);
    }

    TEST(MachPortNamespace, GuardsRejectTheWrongContext)
    {
        mach_port_namespace ports{};
        const auto receive = ports.allocate_receive_right();

        ASSERT_EQ(ports.guard(receive, 0xDEAD, true), kr::success);
        EXPECT_EQ(ports.guard(receive, 0xBEEF, true), kr::invalid_argument);
        EXPECT_EQ(ports.unguard(receive, 0xBEEF), kr::invalid_argument);
        EXPECT_EQ(ports.unguard(receive, 0xDEAD), kr::success);
    }

    TEST(MachPortNamespace, GuardsRecordStrictnessAndCanBeReapplied)
    {
        mach_port_namespace ports{};
        const auto receive = ports.allocate_receive_right();
        ASSERT_NE(ports.find(receive), nullptr);

        EXPECT_EQ(ports.unguard(receive, 0), kr::invalid_argument) << "an unguarded port cannot be unguarded";

        ASSERT_EQ(ports.guard(receive, 0xDEAD, true), kr::success);
        EXPECT_TRUE(ports.find(receive)->strict_guard);
        ASSERT_EQ(ports.unguard(receive, 0xDEAD), kr::success);
        EXPECT_FALSE(ports.find(receive)->guard.has_value());
        EXPECT_FALSE(ports.find(receive)->strict_guard);

        ASSERT_EQ(ports.guard(receive, 0xBEEF, false), kr::success);
        ASSERT_TRUE(ports.find(receive)->guard.has_value());
        EXPECT_EQ(*ports.find(receive)->guard, 0xBEEFu);
        EXPECT_FALSE(ports.find(receive)->strict_guard);

        EXPECT_EQ(ports.guard(0x9999, 1, false), kr::invalid_name);
        EXPECT_EQ(ports.unguard(0x9999, 1), kr::invalid_name);
    }

    TEST(MachPortNamespace, PortSetsCollectMembersAndReleaseThem)
    {
        mach_port_namespace ports{};
        const auto set = ports.allocate_port_set();
        const auto first = ports.allocate_receive_right();
        const auto second = ports.allocate_receive_right();
        ASSERT_NE(ports.find(set), nullptr);

        EXPECT_EQ(ports.type_of(set), 0x80000u);
        EXPECT_NE(port_index(set), port_index(first));

        ASSERT_EQ(ports.move_member(first, set), kr::success);
        ASSERT_EQ(ports.move_member(second, set), kr::success);
        EXPECT_EQ(ports.find(set)->members.size(), 2u);

        ASSERT_EQ(ports.move_member(first, set), kr::success);
        EXPECT_EQ(ports.find(set)->members.size(), 2u) << "re-adding a member must not duplicate it";

        ASSERT_EQ(ports.move_member(first, PORT_NULL), kr::success);
        EXPECT_EQ(ports.find(set)->members.size(), 1u);

        EXPECT_EQ(ports.move_member(second, first), kr::invalid_right) << "a plain receive right is not a port set";
        EXPECT_EQ(ports.move_member(second, 0x9999), kr::invalid_name);
        EXPECT_EQ(ports.move_member(0x9999, set), kr::invalid_name);
        EXPECT_EQ(ports.move_member(set, set), kr::invalid_right) << "a port set holds no receive right to move";
        EXPECT_EQ(ports.find(set)->members.size(), 1u) << "a rejected move must leave the membership alone";

        ASSERT_EQ(ports.mod_refs(second, right_kind::receive, -1), kr::success);
        EXPECT_TRUE(ports.find(set)->members.empty()) << "a destroyed port leaves the set it belonged to";
    }

    TEST(MachPortNamespace, DestructRequiresTheMatchingGuard)
    {
        mach_port_namespace ports{};

        const auto plain = ports.allocate_receive_right();
        EXPECT_EQ(ports.destruct(plain, 0, 0xDEAD), kr::invalid_argument) << "an unguarded port destructs only against a zero guard";
        EXPECT_TRUE(ports.exists(plain));
        ASSERT_EQ(ports.destruct(plain, 0, 0), kr::success);
        EXPECT_FALSE(ports.exists(plain));

        const auto guarded = ports.allocate_receive_right();
        ASSERT_EQ(ports.guard(guarded, 0xC0FFEE, true), kr::success);
        EXPECT_EQ(ports.destruct(guarded, 0, 0), kr::invalid_argument);
        EXPECT_TRUE(ports.exists(guarded));
        ASSERT_EQ(ports.destruct(guarded, 0, 0xC0FFEE), kr::success);
        EXPECT_FALSE(ports.exists(guarded));

        const auto with_send = ports.allocate_receive_right();
        ASSERT_NE(ports.insert_send_right(with_send), PORT_NULL);
        EXPECT_EQ(ports.destruct(with_send, -2, 0), kr::invalid_value);
        EXPECT_TRUE(ports.exists(with_send));
        ASSERT_EQ(ports.destruct(with_send, -1, 0), kr::success);
        EXPECT_FALSE(ports.exists(with_send)) << "no send right remains, so the name is gone";

        EXPECT_EQ(ports.destruct(0x9999, 0, 0), kr::invalid_name);
    }

    TEST(MachPortNamespace, DeallocateNeverDropsAReceiveRight)
    {
        mach_port_namespace ports{};
        const auto receive = ports.allocate_receive_right();

        EXPECT_EQ(ports.deallocate(receive), kr::invalid_right);
        EXPECT_TRUE(ports.exists(receive));

        ASSERT_NE(ports.insert_send_right(receive), PORT_NULL);
        ASSERT_EQ(ports.mod_refs(receive, right_kind::send, 1), kr::success);
        ASSERT_EQ(ports.deallocate(receive), kr::success);
        ASSERT_NE(ports.find(receive), nullptr);
        EXPECT_EQ(ports.find(receive)->send_urefs, 1u);
        ASSERT_EQ(ports.deallocate(receive), kr::success);
        EXPECT_EQ(ports.find(receive)->send_urefs, 0u);
        EXPECT_EQ(ports.deallocate(receive), kr::invalid_right);

        const auto once = ports.allocate_send_once_right(receive);
        ASSERT_EQ(ports.deallocate(once), kr::success);
        EXPECT_FALSE(ports.exists(once));

        const auto set = ports.allocate_port_set();
        EXPECT_EQ(ports.deallocate(set), kr::invalid_right);
    }

    TEST(MachPortNamespace, TypeBitsCombineEveryHeldRight)
    {
        mach_port_namespace ports{};
        const auto receive = ports.allocate_receive_right();

        EXPECT_EQ(ports.type_of(receive), 0x20000u);
        ASSERT_NE(ports.insert_send_right(receive), PORT_NULL);
        EXPECT_EQ(ports.type_of(receive), 0x30000u);

        EXPECT_EQ(ports.type_of(ports.allocate_port_set()), 0x80000u);
        EXPECT_EQ(ports.type_of(ports.allocate_send_once_right(receive)), 0x40000u);
    }

    TEST(MachPortNamespace, LosingAReceiveRightWithLiveSendRightsLeavesADeadName)
    {
        mach_port_namespace ports{};
        const auto receive = ports.allocate_receive_right();
        ASSERT_NE(ports.insert_send_right(receive), PORT_NULL);
        ASSERT_EQ(ports.mod_refs(receive, right_kind::receive, -1), kr::success);

        ASSERT_NE(ports.find(receive), nullptr);
        EXPECT_TRUE(ports.exists(receive)) << "a name with live send rights outlives its receive right";
        EXPECT_TRUE(ports.find(receive)->dead);
        EXPECT_FALSE(ports.find(receive)->has_receive);
        EXPECT_EQ(ports.type_of(receive), 0x100000u);
        EXPECT_EQ(ports.mod_refs(receive, right_kind::send, -1), kr::invalid_right);
        EXPECT_EQ(ports.mod_refs(receive, right_kind::receive, -1), kr::invalid_right);

        ASSERT_EQ(ports.mod_refs(receive, right_kind::dead_name, -1), kr::success);
        EXPECT_FALSE(ports.exists(receive));
        EXPECT_EQ(port_index(ports.allocate_receive_right()), port_index(receive))
            << "the index returns to the pool only once the dead name is gone";
    }

    TEST(MachPortNamespace, LivePortCountTracksAllocationAndRelease)
    {
        mach_port_namespace ports{};
        EXPECT_EQ(ports.live_port_count(), 0u);

        const auto receive = ports.allocate_receive_right();
        const auto set = ports.allocate_port_set();
        const auto once = ports.allocate_send_once_right(receive);
        EXPECT_EQ(ports.live_port_count(), 3u);

        ASSERT_EQ(ports.mod_refs(once, right_kind::send_once, -1), kr::success);
        EXPECT_EQ(ports.live_port_count(), 2u);
        ASSERT_EQ(ports.mod_refs(set, right_kind::port_set, -1), kr::success);
        EXPECT_EQ(ports.live_port_count(), 1u);
        ASSERT_EQ(ports.mod_refs(receive, right_kind::receive, -1), kr::success);
        EXPECT_EQ(ports.live_port_count(), 0u);
    }

    TEST(MachPortNamespace, SerializationRoundTripsNamesAndGenerations)
    {
        mach_port_namespace ports{};
        const auto first = ports.allocate_receive_right({.kind = kernel_object_kind::host, .id = 7});
        ASSERT_EQ(ports.mod_refs(first, right_kind::receive, -1), kr::success);
        const auto reused = ports.allocate_receive_right();

        buffer_serializer serializer{};
        ports.serialize(serializer);

        mach_port_namespace restored{};
        buffer_deserializer deserializer{serializer.get_buffer()};
        restored.deserialize(deserializer);

        EXPECT_TRUE(restored.exists(reused));
        EXPECT_FALSE(restored.exists(first));
        EXPECT_EQ(restored.allocate_receive_right() >> 8, 2u) << "the index allocator must survive a round trip";
    }

    TEST(MachPortNamespace, SerializationRoundTripsTheFreeIndexPoolAndGenerationHistory)
    {
        mach_port_namespace ports{};
        const auto first = ports.allocate_receive_right();
        const auto second = ports.allocate_receive_right();
        ports.allocate_receive_right();
        ASSERT_EQ(ports.mod_refs(second, right_kind::receive, -1), kr::success);

        buffer_serializer serializer{};
        ports.serialize(serializer);

        mach_port_namespace restored{};
        buffer_deserializer deserializer{serializer.get_buffer()};
        restored.deserialize(deserializer);

        const auto recycled = restored.allocate_receive_right();
        EXPECT_EQ(port_index(recycled), port_index(second)) << "the freed index pool must survive a round trip";
        EXPECT_NE(recycled, second) << "the generation history must survive a round trip";
        EXPECT_EQ(port_generation(recycled), port_generation(second) + 1);
        EXPECT_EQ(port_index(restored.allocate_receive_right()), 4u);
        EXPECT_TRUE(restored.exists(first));
    }

    TEST(MachPortNamespace, SerializationRoundTripsEntryState)
    {
        mach_port_namespace ports{};
        const auto receive = ports.allocate_receive_right({.kind = kernel_object_kind::bootstrap, .id = 42});
        ASSERT_NE(ports.insert_send_right(receive), PORT_NULL);
        ASSERT_EQ(ports.mod_refs(receive, right_kind::send, 2), kr::success);
        ASSERT_EQ(ports.guard(receive, 0xFEEDFACE, true), kr::success);
        ASSERT_NE(ports.find(receive), nullptr);
        ports.find(receive)->queue_limit = 17;
        ports.find(receive)->queue.push_back({1, 2, 3});
        ports.find(receive)->queue.emplace_back();

        const auto set = ports.allocate_port_set();
        ASSERT_EQ(ports.move_member(receive, set), kr::success);
        const auto once = ports.allocate_send_once_right(receive);

        buffer_serializer serializer{};
        ports.serialize(serializer);

        mach_port_namespace restored{};
        buffer_deserializer deserializer{serializer.get_buffer()};
        restored.deserialize(deserializer);

        const auto* entry = restored.find(receive);
        ASSERT_NE(entry, nullptr);
        EXPECT_EQ(entry->object.kind, kernel_object_kind::bootstrap);
        EXPECT_EQ(entry->object.id, 42u);
        EXPECT_EQ(entry->send_urefs, 3u);
        EXPECT_TRUE(entry->has_receive);
        EXPECT_TRUE(entry->strict_guard);
        ASSERT_TRUE(entry->guard.has_value());
        EXPECT_EQ(*entry->guard, 0xFEEDFACEu);
        EXPECT_EQ(entry->queue_limit, 17u);
        ASSERT_EQ(entry->queue.size(), 2u);
        EXPECT_EQ(entry->queue.front(), (std::vector<uint8_t>{1, 2, 3}));
        EXPECT_TRUE(entry->queue.back().empty());

        const auto* restored_set = restored.find(set);
        ASSERT_NE(restored_set, nullptr);
        EXPECT_TRUE(restored_set->is_port_set);
        EXPECT_EQ(restored_set->members, std::vector<uint32_t>{port_index(receive)});

        ASSERT_NE(restored.find(once), nullptr);
        EXPECT_TRUE(restored.find(once)->has_send_once);
        EXPECT_EQ(restored.live_port_count(), ports.live_port_count());
    }

    void write_receive_entry(buffer_serializer& out, const uint32_t index, const uint8_t generation, const port_id_t port_id)
    {
        out.write<uint32_t>(index);
        out.write<uint8_t>(generation);
        out.write<uint64_t>(port_id);
        out.write<bool>(true);
        out.write<bool>(false);
        out.write<uint64_t>(INVALID_PORT_ID);
        out.write<uint32_t>(0);
        out.write<bool>(false);
        out.write<uint8_t>(static_cast<uint8_t>(kernel_object_kind::none));
        out.write<uint64_t>(0);
        out.write<uint32_t>(PORT_QLIMIT_DEFAULT);
        out.write_optional(std::optional<uint64_t>{});
        out.write<bool>(false);
        out.write<bool>(false);
        out.write<uint64_t>(0);
        out.write<uint64_t>(0);
    }

    buffer_serializer empty_snapshot(const port_id_t next_port_id)
    {
        buffer_serializer out{};
        out.write<uint64_t>(0);
        out.write<uint32_t>(1);
        out.write<uint64_t>(next_port_id);
        out.write<uint64_t>(0);
        out.write_map(std::map<uint32_t, uint8_t>{});
        return out;
    }

    buffer_serializer two_port_snapshot(const port_id_t first_id, const port_id_t second_id, const port_id_t next_port_id)
    {
        buffer_serializer out{};
        out.write<uint64_t>(2);
        write_receive_entry(out, 1, 1, first_id);
        write_receive_entry(out, 2, 1, second_id);
        out.write<uint32_t>(3);
        out.write<uint64_t>(next_port_id);
        out.write<uint64_t>(0);
        out.write_map(std::map<uint32_t, uint8_t>{{1, 1}, {2, 1}});
        return out;
    }

    void expect_snapshot_rejected(const buffer_serializer& snapshot, const char* why)
    {
        SCOPED_TRACE(why);
        mach_port_namespace restored{};
        buffer_deserializer deserializer{snapshot.get_buffer()};
        EXPECT_THROW(restored.deserialize(deserializer), std::runtime_error);
    }

    TEST(MachPortNamespace, ASnapshotThatBreaksPortIdentityIsRejected)
    {
        {
            const auto well_formed = two_port_snapshot(4, 5, 6);
            mach_port_namespace restored{};
            buffer_deserializer deserializer{well_formed.get_buffer()};
            ASSERT_NO_THROW(restored.deserialize(deserializer)) << "the well formed shape this test mutates must load";
            ASSERT_NE(restored.find_by_port_id(5), nullptr);
            EXPECT_EQ(restored.find_by_port_id(5), restored.find(make_port_name(2, 1)));
        }

        expect_snapshot_rejected(two_port_snapshot(4, 4, 6), "two ports sharing one id would alias every send-once link");
        expect_snapshot_rejected(two_port_snapshot(4, INVALID_PORT_ID, 6), "a live port with no identity cannot be resolved");
        expect_snapshot_rejected(two_port_snapshot(4, 6, 6), "an id at the counter would be handed out again");
        expect_snapshot_rejected(two_port_snapshot(4, 9, 6), "an id beyond the counter would be handed out again");
        expect_snapshot_rejected(two_port_snapshot(4, 5, INVALID_PORT_ID), "the next id must never be the null id");
        expect_snapshot_rejected(empty_snapshot(INVALID_PORT_ID), "an empty namespace must not restore a null counter either");

        {
            const auto well_formed = empty_snapshot(1);
            mach_port_namespace restored{};
            buffer_deserializer deserializer{well_formed.get_buffer()};
            ASSERT_NO_THROW(restored.deserialize(deserializer));
            EXPECT_EQ(restored.live_port_count(), 0u);
        }
    }
}
