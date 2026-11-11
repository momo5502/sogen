#include <gtest/gtest.h>

#include <windows_objects.hpp>

namespace sogen::test
{
    TEST(HandleStoreTest, EachDuplicateHasItsOwnValueAndCloseDropsOneReference)
    {
        handle_store<handle_types::event, event> store{};

        const auto original = store.store(event{});
        const auto duplicate = store.duplicate(original);
        ASSERT_TRUE(duplicate);
        EXPECT_NE(original, *duplicate);
        EXPECT_EQ(2u, store.handle_count());

        auto* object = store.get(original);
        ASSERT_NE(nullptr, object);
        EXPECT_EQ(2u, object->ref_count);
        EXPECT_EQ(original, store.find_handle(object));

        ASSERT_TRUE(store.erase(original));
        EXPECT_EQ(nullptr, store.get(original));
        ASSERT_NE(nullptr, store.get(*duplicate));
        EXPECT_EQ(1u, store.get(*duplicate)->ref_count);
        EXPECT_EQ(*duplicate, store.find_handle(store.get(*duplicate)));

        ASSERT_TRUE(store.erase(*duplicate));
        EXPECT_EQ(nullptr, store.get(*duplicate));
        EXPECT_EQ(0u, store.size());
        EXPECT_EQ(0u, store.handle_count());
    }

    TEST(HandleStoreTest, InternalReferenceKeepsTheObjectAliveAfterItsLastHandleCloses)
    {
        using store_type = handle_store<handle_types::event, event>;
        store_type store{};

        const auto original = store.store(event{});
        auto reference = store.retain(original);
        ASSERT_TRUE(reference);
        EXPECT_GE(reference->index, store_type::first_object_index);
        EXPECT_NE(static_cast<uint64_t>(original.value.id), reference->index);

        ASSERT_TRUE(store.erase(original));
        EXPECT_EQ(nullptr, store.get(original));
        EXPECT_NE(nullptr, store.get_object(*reference));

        store.release(*reference);
        EXPECT_FALSE(*reference);
        EXPECT_EQ(0u, store.size());
    }

    TEST(HandleStoreTest, InternalHandleResolvesWithoutBeingAReference)
    {
        handle_store<handle_types::event, event> store{};

        const auto original = store.store(event{});
        auto* object = store.get(original);
        ASSERT_NE(nullptr, object);
        auto reference = store.retain(original);
        ASSERT_TRUE(reference);
        const auto internal = store.internal_handle(*object);
        ASSERT_NE(0u, internal.bits);
        EXPECT_EQ(object, store.get(internal));
        EXPECT_EQ(1u, store.handle_count());

        ASSERT_TRUE(store.erase(original));
        EXPECT_EQ(nullptr, store.get(original));
        EXPECT_EQ(object, store.get(internal));
        EXPECT_EQ(0u, store.handle_count());
        EXPECT_EQ(1u, object->ref_count);

        store.release(*reference);
        EXPECT_EQ(nullptr, store.get(internal));
    }

    TEST(HandleStoreTest, ClosingAnUnknownOrAlreadyClosedValueFails)
    {
        handle_store<handle_types::event, event> store{};

        const auto original = store.store(event{});
        ASSERT_TRUE(store.erase(original));
        EXPECT_FALSE(store.erase(original));
        EXPECT_FALSE(store.duplicate(original));
        EXPECT_FALSE(store.retain(original));
        EXPECT_EQ(nullptr, store.get(original));
    }
}
