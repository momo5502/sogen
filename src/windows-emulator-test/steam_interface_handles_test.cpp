#include "std_include.hpp"

#include "../steam-host-backend/steam_interface_handles.hpp"
#include "../samples/steam-shim/sub_interface_proxies.hpp"

namespace sogen::test
{
    namespace
    {
        using steam_host::interface_handles;
        using steam_host::interface_owner;

        int first_object{};
        int second_object{};
    }

    // A game that releases its pipe and creates another gets new interface objects from Steam.
    TEST(SteamInterfaceHandlesTest, AnInterfaceOfANewPipeGetsItsOwnHandle)
    {
        interface_handles handles{};
        const auto first = handles.add("SteamUtils011", &first_object, {.pipe = 1, .user = 1});
        EXPECT_EQ(first, handles.add("SteamUtils011", &first_object, {.pipe = 1, .user = 1}));

        handles.release_pipe(1);
        const auto second = handles.add("SteamUtils011", &second_object, {.pipe = 2, .user = 1});

        EXPECT_NE(first, second);
        ASSERT_NE(nullptr, handles.find(second));
        EXPECT_EQ(&second_object, handles.find(second)->iface);
    }

    TEST(SteamInterfaceHandlesTest, AReleasedPipesInterfacesNoLongerResolve)
    {
        interface_handles handles{};
        const auto utils = handles.add("SteamUtils011", &first_object, {.pipe = 1, .user = 1});
        const auto user = handles.add("SteamUser023", &second_object, {.pipe = 1, .user = 1});

        handles.release_pipe(1);

        EXPECT_EQ(nullptr, handles.find(utils));
        EXPECT_EQ(nullptr, handles.find(user));
    }

    TEST(SteamInterfaceHandlesTest, ReleasingOnePipeLeavesAnotherUsable)
    {
        interface_handles handles{};
        const auto on_first = handles.add("SteamUtils011", &first_object, {.pipe = 1, .user = 1});
        const auto on_second = handles.add("SteamUtils011", &first_object, {.pipe = 2, .user = 1});
        const auto unowned = handles.add("SteamClient023", &second_object, {});

        handles.release_pipe(1);

        EXPECT_NE(on_first, on_second);
        EXPECT_EQ(nullptr, handles.find(on_first));
        EXPECT_NE(nullptr, handles.find(on_second));
        EXPECT_NE(nullptr, handles.find(unowned));
    }

    // Steam can hand out the same address again once the old object is gone; the old handle must not alias it.
    TEST(SteamInterfaceHandlesTest, AReusedAddressAfterAReleaseGetsANewHandle)
    {
        interface_handles handles{};
        const auto before = handles.add("SteamUtils011", &first_object, {.pipe = 1, .user = 1});
        handles.release_pipe(1);

        const auto after = handles.add("SteamUtils011", &first_object, {.pipe = 3, .user = 1});

        EXPECT_NE(before, after);
        EXPECT_EQ(nullptr, handles.find(before));
    }

    TEST(SteamInterfaceHandlesTest, ReleasingAUserLeavesOtherUsersOfThePipe)
    {
        interface_handles handles{};
        const auto first_user = handles.add("SteamUser023", &first_object, {.pipe = 1, .user = 1});
        const auto second_user = handles.add("SteamUser023", &first_object, {.pipe = 1, .user = 2});

        handles.release_user(1, 1);

        EXPECT_NE(first_user, second_user);
        EXPECT_EQ(nullptr, handles.find(first_user));
        EXPECT_NE(nullptr, handles.find(second_user));
    }

    // A top-level interface belongs to the pipe and user the backend opened for itself; if they go away, its handle
    // must stop resolving like any other.
    TEST(SteamInterfaceHandlesTest, AnInterfaceOfTheBackendsOwnPipeIsDroppedWithIt)
    {
        constexpr interface_owner backend{.pipe = 1, .user = 1};
        interface_handles handles{};
        const auto top_level = handles.add("SteamUtils010", &first_object, backend);
        const auto games = handles.add("SteamUtils011", &first_object, {.pipe = 2, .user = 3});

        handles.release_pipe(backend.pipe);

        EXPECT_EQ(nullptr, handles.find(top_level));
        EXPECT_NE(nullptr, handles.find(games));
    }

    // The guest never holds the backend's own pipe or user, so releasing them on its behalf is refused before Steam.
    TEST(SteamInterfaceHandlesTest, TheBackendsOwnPipeAndUserMayNotBeReleased)
    {
        interface_handles handles{};
        handles.set_backend_owner({.pipe = 1, .user = 1});

        EXPECT_FALSE(handles.may_release_pipe(1));
        EXPECT_FALSE(handles.may_release_user(1, 1));
        EXPECT_TRUE(handles.may_release_pipe(2));
        EXPECT_TRUE(handles.may_release_user(2, 1));
        EXPECT_TRUE(handles.may_release_user(1, 2));
    }

    // The game obtains the same interface version again after reconnecting; the proxy it gets must reach the new
    // host object, not the one that went away with the old pipe.
    TEST(SteamSubInterfaceProxiesTest, TheSameVersionWithANewHandleGetsAProxyForThatHandle)
    {
        steam_shim::sub_interface_proxies proxies{};
        std::vector<uint64_t> created_for{};
        std::map<uint64_t, int> proxy_objects{};
        const auto resolve = [&](const uint64_t handle) {
            return proxies.resolve(handle, [&] {
                created_for.push_back(handle);
                return static_cast<void*>(&proxy_objects[handle]);
            });
        };

        void* first = resolve(3);
        EXPECT_EQ(first, resolve(3));
        void* second = resolve(9);

        EXPECT_NE(first, second);
        EXPECT_EQ((std::vector<uint64_t>{3, 9}), created_for);
    }
}
