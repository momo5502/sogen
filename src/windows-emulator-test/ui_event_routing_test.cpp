#include "emulation_test_utils.hpp"

#include <deque>
#include <ui_event_router.hpp>

namespace sogen::test
{
    namespace
    {
        constexpr hwnd host_window = 0x100000000;

        class queued_ui_backend final : public ui_backend
        {
          public:
            void set_event_sink(event_sink sink) override
            {
                this->sink_ = std::move(sink);
            }

            void pump_events() override
            {
                while (!this->events.empty())
                {
                    const auto event = this->events.front();
                    this->events.pop_front();
                    this->sink_(event);
                }
            }

            void reset() override
            {
                this->events.clear();
            }

            std::deque<ui_event> events{};

          private:
            event_sink sink_{};
        };

        class UiWindowEventRoutingTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                auto backend = std::make_unique<queued_ui_backend>();
                this->ui = backend.get();
                emulator_interfaces interfaces{};
                interfaces.ui = std::move(backend);
                interfaces.audio = std::make_unique<null_audio_backend>();
                interfaces.console = std::make_unique<null_console_backend>();
                this->emu = std::make_unique<windows_emulator>(
                    create_x86_64_emulator_from_environment(),
                    emulator_settings{.disable_logging = true, .emulation_root = get_emulator_root(), .load_registry = false},
                    emulator_callbacks{}, std::move(interfaces));

                const auto base = this->emu->memory.allocate_memory(0x10000, memory_permission::read_write);
                this->emu->process.base_allocator = emulator_allocator{this->emu->memory, base, 0x10000};
                this->emu->process.user_handles.setup(false);
                this->thread_handle =
                    this->emu->process.threads.store(emulator_thread{this->emu->memory, this->emu->process, 0, 0, 0x100000, 0, 8, true});
            }

            emulator_thread& thread()
            {
                return *this->emu->process.threads.get(this->thread_handle);
            }

            hwnd create_guest_window()
            {
                auto [handle, window] = this->emu->process.windows.create(this->emu->memory);
                window.handle = handle.bits;
                window.thread_id = this->thread().id;
                return window.handle;
            }

            void check_restoration(bool snapshot)
            {
                this->emu->process.kusd.setup(this->emu->version, {});
                size_t calls{};
                auto subscription = this->emu->subscribe_host_window_events(host_window, [&](const ui_event&) { ++calls; });
                this->emu->handle_ui_event({.window = host_window, .message = WM_CLOSE});
                ASSERT_EQ(1u, calls);
                if (snapshot)
                {
                    this->emu->save_snapshot();
                    this->emu->restore_snapshot();
                }
                else
                {
                    utils::buffer_serializer buffer{};
                    this->emu->serialize(buffer);
                    utils::buffer_deserializer reader{buffer.get_buffer()};
                    this->emu->deserialize(reader);
                }
                this->emu->handle_ui_event({.window = host_window, .message = WM_CLOSE});
                EXPECT_EQ(1u, calls);
                auto replacement = this->emu->subscribe_host_window_events(host_window, [&](const ui_event&) { ++calls; });
                subscription.reset();
                this->emu->handle_ui_event({.window = host_window, .message = WM_CLOSE});
                EXPECT_EQ(2u, calls);
            }

            std::unique_ptr<windows_emulator> emu{};
            queued_ui_backend* ui{};
            handle thread_handle{};
        };
    }

    TEST(UiEventRouterTest, RoutesOnlyTheSubscribedWindowAndRetainsHandlerState)
    {
        ui_event_router router{};
        size_t seen{};
        size_t other{};
        auto first = router.subscribe(host_window, [&, count = size_t{}](const ui_event&) mutable { seen = ++count; });
        auto second = router.subscribe(host_window + 1, [&](const ui_event&) { ++other; });
        EXPECT_TRUE(router.dispatch({.window = host_window}));
        EXPECT_TRUE(router.dispatch({.window = host_window}));
        EXPECT_EQ(2u, seen);
        EXPECT_EQ(0u, other);
        EXPECT_FALSE(router.dispatch({.window = host_window + 2}));
        first.reset();
        EXPECT_FALSE(router.dispatch({.window = host_window}));
        EXPECT_TRUE(router.dispatch({.window = host_window + 1}));
        EXPECT_EQ(1u, other);
    }

    TEST(UiEventRouterTest, MovingASubscriptionReleasesItsPreviousWindow)
    {
        ui_event_router router{};
        auto first = router.subscribe(host_window, [](const ui_event&) {});
        auto second = router.subscribe(host_window + 1, [](const ui_event&) {});
        first = std::move(second);
        EXPECT_FALSE(router.dispatch({.window = host_window}));
        EXPECT_TRUE(router.dispatch({.window = host_window + 1}));
        auto moved = std::move(first);
        first.reset();
        EXPECT_TRUE(router.dispatch({.window = host_window + 1}));
        moved.reset();
        EXPECT_FALSE(router.dispatch({.window = host_window + 1}));
    }

    TEST(UiEventRouterTest, AHandlerCanUnsubscribeWithoutDestroyingItsActiveCallable)
    {
        ui_event_router router{};
        ui_event_router::subscription subscription{};
        auto owner = std::make_shared<int>(42);
        const std::weak_ptr<int> lifetime = owner;
        subscription = router.subscribe(host_window, [&, owner](const ui_event&) {
            subscription.reset();
            EXPECT_FALSE(lifetime.expired());
            EXPECT_EQ(42, *owner);
        });
        owner.reset();
        EXPECT_TRUE(router.dispatch({.window = host_window}));
        EXPECT_TRUE(lifetime.expired());
        EXPECT_FALSE(router.dispatch({.window = host_window}));
    }

    TEST(UiEventRouterTest, SubscriptionsMayOutliveTheirRouter)
    {
        ui_event_router::subscription subscription{};
        auto owner = std::make_shared<int>();
        const std::weak_ptr<int> lifetime = owner;
        {
            ui_event_router router{};
            subscription = router.subscribe(host_window, [owner](const ui_event&) {});
            owner.reset();
            EXPECT_FALSE(lifetime.expired());
        }
        EXPECT_TRUE(lifetime.expired());
        subscription.reset();
    }

    TEST(UiEventRouterTest, AnOldSubscriptionCannotRemoveAReplacementAfterReset)
    {
        ui_event_router router{};
        auto old = router.subscribe(host_window, [](const ui_event&) {});
        router.clear();
        size_t calls{};
        auto replacement = router.subscribe(host_window, [&](const ui_event&) { ++calls; });
        old.reset();
        EXPECT_TRUE(router.dispatch({.window = host_window}));
        EXPECT_EQ(1u, calls);
    }

    TEST(UiEventRouterTest, RejectsMissingWindowsHandlersAndDuplicateOwnership)
    {
        ui_event_router router{};
        EXPECT_THROW((void)router.subscribe(0, [](const ui_event&) {}), std::invalid_argument);
        EXPECT_THROW((void)router.subscribe(host_window, {}), std::invalid_argument);
        size_t calls{};
        auto subscription = router.subscribe(host_window, [&](const ui_event&) { ++calls; });
        EXPECT_THROW((void)router.subscribe(host_window, [](const ui_event&) {}), std::invalid_argument);
        EXPECT_TRUE(router.dispatch({.window = host_window}));
        EXPECT_EQ(1u, calls);
    }

    TEST_F(UiWindowEventRoutingTest, DeliversHostEventsBeforeGuestHooksAndKeepsGuestEventsQueued)
    {
        this->thread().low_level_hooks.push_back({.handle = 123, .id = 13, .proc = 0x1000});
        const auto guest = this->create_guest_window();
        this->emu->handle_ui_event({.window = guest, .message = WM_KEYDOWN, .wParam = 'A'});
        ASSERT_EQ(1u, this->emu->process.pending_low_level_input.size());
        this->emu->process.drop_low_level_characters = true;

        std::vector<uint32_t> messages{};
        auto subscription = this->emu->subscribe_host_window_events(host_window, [&](const ui_event& event) {
            EXPECT_EQ(host_window, event.window);
            messages.push_back(event.message);
        });
        for (const uint32_t message : {WM_KEYDOWN, WM_CHAR, WM_MOUSEMOVE, WM_SIZE, WM_CLOSE})
        {
            this->ui->events.push_back({.window = host_window, .message = message});
        }
        this->ui->pump_events();
        EXPECT_EQ((std::vector<uint32_t>{WM_KEYDOWN, WM_CHAR, WM_MOUSEMOVE, WM_SIZE, WM_CLOSE}), messages);
        EXPECT_EQ(1u, this->emu->process.pending_low_level_input.size());
        EXPECT_EQ(guest, this->emu->process.pending_low_level_input.front().event.window);
        EXPECT_TRUE(this->emu->process.drop_low_level_characters);
    }

    TEST_F(UiWindowEventRoutingTest, OtherWindowsStillReceiveTheirGuestMessages)
    {
        size_t calls{};
        auto subscription = this->emu->subscribe_host_window_events(host_window, [&](const ui_event&) { ++calls; });
        const auto guest = this->create_guest_window();
        this->emu->handle_ui_event({.window = guest, .message = WM_CLOSE, .wParam = 42, .lParam = 123});
        ASSERT_EQ(1u, this->thread().message_queue.size());
        const auto& event = this->thread().message_queue.front();
        EXPECT_EQ(guest, event.window);
        EXPECT_EQ(WM_CLOSE, event.message);
        EXPECT_EQ(42u, event.wParam);
        EXPECT_EQ(123u, event.lParam);
        EXPECT_EQ(0u, calls);
        EXPECT_THROW((void)this->emu->subscribe_host_window_events(guest, [](const ui_event&) {}), std::invalid_argument);
    }

    TEST_F(UiWindowEventRoutingTest, QueuedEventsDoNotCallADestroyedConsumer)
    {
        size_t calls{};
        auto owner = std::make_shared<int>();
        const std::weak_ptr<int> lifetime = owner;
        {
            auto subscription = this->emu->subscribe_host_window_events(host_window, [&, owner](const ui_event&) { ++calls; });
            owner.reset();
            this->ui->events.push_back({.window = host_window, .message = WM_CLOSE});
        }
        EXPECT_TRUE(lifetime.expired());
        this->ui->pump_events();
        EXPECT_EQ(0u, calls);
    }

    TEST_F(UiWindowEventRoutingTest, AHandlerCanReplaceItsRegistrationInsideTheKernelLock)
    {
        size_t calls{};
        ui_event_router::subscription first{};
        ui_event_router::subscription second{};
        first = this->emu->subscribe_host_window_events(host_window, [&](const ui_event&) {
            first.reset();
            second = this->emu->subscribe_host_window_events(host_window + 1, [&](const ui_event&) { ++calls; });
        });
        this->emu->handle_ui_event({.window = host_window, .message = WM_CLOSE});
        this->emu->handle_ui_event({.window = host_window + 1, .message = WM_CLOSE});
        EXPECT_EQ(1u, calls);
    }

    TEST_F(UiWindowEventRoutingTest, ASubscriptionCanOutliveTheEmulator)
    {
        auto owner = std::make_shared<int>();
        const std::weak_ptr<int> lifetime = owner;
        auto subscription = this->emu->subscribe_host_window_events(host_window, [owner](const ui_event&) {});
        owner.reset();
        this->emu.reset();
        EXPECT_TRUE(lifetime.expired());
        subscription.reset();
    }

    TEST_F(UiWindowEventRoutingTest, RestoringASnapshotInvalidatesHostRegistrations)
    {
        this->check_restoration(true);
    }

    TEST_F(UiWindowEventRoutingTest, DeserializingInvalidatesHostRegistrations)
    {
        this->check_restoration(false);
    }
}
