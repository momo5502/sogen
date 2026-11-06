#include "native_presenter.hpp"
#include <devices/qt_raster.hpp>
#include <emulation_test_utils.hpp>

namespace sogen::test
{
    namespace
    {
        namespace wire = qt_raster;

        class recording_ui : public ui_backend
        {
          public:
            event_sink sink{};
            std::map<hwnd, ui_window_desc> windows{};
            std::map<hwnd, std::vector<uint8_t>> pixels{};
            size_t creations{};
            size_t destructions{};
            size_t geometry_changes{};
            std::vector<hwnd> activations{};
            std::vector<hwnd> destroyed{};
            std::optional<uint32_t> cursor{};
            bool cursor_visible{true};

            void set_event_sink(event_sink value) override
            {
                this->sink = std::move(value);
            }

            void pump_events() override
            {
            }

            void create_window(const ui_window_desc& value) override
            {
                this->windows[value.handle] = value;
                ++this->creations;
            }

            void destroy_window(const hwnd value) override
            {
                this->destroyed.push_back(value);
                this->windows.erase(value);
                this->pixels.erase(value);
                ++this->destructions;
            }

            void set_window_rect(const hwnd value, const RECT& rect) override
            {
                this->windows.at(value).rect = rect;
                ++this->geometry_changes;
            }

            void activate_window(const hwnd value) override
            {
                this->activations.push_back(value);
            }

            void set_window_enabled(const hwnd value, const bool enabled) override
            {
                this->windows.at(value).enabled = enabled;
            }

            void set_system_cursor(const uint32_t idc) override
            {
                this->cursor = idc;
            }

            void set_cursor_visibility(const bool visible) override
            {
                this->cursor_visible = visible;
            }

            void set_window_title(const hwnd value, const std::u16string_view text) override
            {
                this->windows.at(value).title = text;
            }

            void set_window_visible(const hwnd value, const bool visible) override
            {
                this->windows.at(value).visible = visible;
            }

            void present_surface(const hwnd value, const ui_surface_desc& surface) override
            {
                const auto* data = static_cast<const uint8_t*>(surface.pixels);
                this->pixels[value].assign(data, data + surface.stride * surface.height);
            }
        };

        class NativeRasterTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                auto backend = std::make_unique<recording_ui>();
                this->ui = backend.get();
                emulator_interfaces interfaces{};
                interfaces.ui = std::move(backend);
                this->emu = std::make_unique<windows_emulator>(
                    create_x86_64_emulator_from_environment(),
                    emulator_settings{.disable_logging = true, .emulation_root = get_emulator_root(), .load_registry = false},
                    emulator_callbacks{}, std::move(interfaces));
                this->device = this->emu->process.devices.store(io_device_container{u"SogenQtBridge", *this->emu, {}});
                this->session = &this->emu->process.devices.get(this->device)->get_internal_device<qt_raster_device>()->session();
                this->presenter = std::make_unique<wire::native_presenter>(*this->emu, *this->ui);
                this->send(wire::Hello);
                this->send(wire::Create, {10, 20, 320, 240, 1, 0, 0, 1000}, "Calculator");
                this->send(wire::Visible, {1});
                this->update();
                ASSERT_EQ(1u, this->ui->windows.size());
                this->host = this->ui->windows.begin()->first;
            }

            void send(const wire::Op op, const std::array<int32_t, 8>& args = {}, const std::string_view data = {})
            {
                this->send_to(1, op, args, data);
            }

            void send_to(const uint32_t id, const wire::Op op, const std::array<int32_t, 8>& args = {}, const std::string_view data = {})
            {
                wire::Packet packet{};
                packet.op = op;
                packet.id = op == wire::Hello || op == wire::End || op == wire::Poll ? 0 : id;
                packet.seq = ++this->sequence;
                packet.session = op == wire::Hello ? 0 : this->session->identity();
                packet.a = args;
                packet.size = static_cast<uint32_t>(data.size());
                const auto payload = std::span{reinterpret_cast<const uint8_t*>(data.data()), data.size()};
                ASSERT_EQ(wire::result::success,
                          this->session->exchange(packet, {payload.begin(), payload.end()}, 4096, [](auto) { return true; }));
            }

            std::pair<wire::Packet, std::string> poll()
            {
                wire::Packet packet{};
                packet.op = wire::Poll;
                packet.seq = ++this->sequence;
                packet.session = this->session->identity();
                wire::Packet reply{};
                std::string text;
                EXPECT_EQ(wire::result::success, this->session->exchange(packet, {}, 4096, [&](const auto bytes) {
                    std::memcpy(&reply, bytes.data(), sizeof(reply));
                    text.assign(reinterpret_cast<const char*>(bytes.data() + sizeof(reply)), bytes.size() - sizeof(reply));
                    return true;
                }));
                return {reply, text};
            }

            void update()
            {
                this->emu->synchronize([&] { this->presenter->update(); });
            }

            void event(const uint32_t message, const uint64_t wparam = 0, const uint64_t lparam = 0)
            {
                this->emu->handle_ui_event({.window = this->host, .message = message, .wParam = wparam, .lParam = lparam});
            }

            void show_window(const uint32_t id, const int32_t type, const int32_t modality = 0, const uint32_t parent = 1,
                             const std::string_view title = "Overlay")
            {
                this->send_to(id, wire::Create, {11, 21, 2, 2, type, modality, static_cast<int32_t>(parent), 1000}, title);
                this->send_to(id, wire::Visible, {1, modality});
            }

            void frame(const uint32_t id, const int32_t width, const int32_t height, const std::array<uint8_t, 4>& color)
            {
                std::string bytes;
                for (int32_t pixel = 0; pixel < width * height; ++pixel)
                {
                    bytes.append(reinterpret_cast<const char*>(color.data()), color.size());
                }
                this->send_to(id, wire::Frame, {width, height, width * 4, 1000, 0, 0, width, height}, bytes);
            }

            hwnd named_host(const std::u16string_view name) const
            {
                for (const auto& [handle, value] : this->ui->windows)
                {
                    if (value.title == name)
                    {
                        return handle;
                    }
                }
                throw std::runtime_error("Missing host window");
            }

            void focus_owner()
            {
                this->event(WM_SETFOCUS);
                this->update();
                ASSERT_EQ(wire::Focus, this->poll().first.op);
            }

            std::unique_ptr<windows_emulator> emu{};
            recording_ui* ui{};
            wire::session* session{};
            std::unique_ptr<wire::native_presenter> presenter{};
            handle device{};
            hwnd host{};
            uint32_t sequence{};
        };
    } // namespace

    TEST_F(NativeRasterTest, GuestPixelsAndWindowMetadataReachTheHost)
    {
        const std::string bytes(16, '\x42');
        this->send(wire::Frame, {2, 2, 8, 1000, 0, 0, 2, 2}, bytes);
        this->update();
        EXPECT_EQ(std::vector<uint8_t>(16, 0x42), this->ui->pixels.at(this->host));
        this->update();
        EXPECT_EQ(1u, this->ui->windows.size());
        this->send(wire::Title, {}, "Changed");
        this->send(wire::Geometry, {30, 40, 400, 300});
        this->send(wire::Visible, {0});
        this->update();
        EXPECT_EQ(u"Changed", this->ui->windows.at(this->host).title);
        EXPECT_EQ(430, this->ui->windows.at(this->host).rect.right);
        EXPECT_FALSE(this->ui->windows.at(this->host).visible);
    }

    TEST_F(NativeRasterTest, TextPairsWithItsPressAndReleaseIncludingUnicodeAndRepeats)
    {
        this->event(WM_KEYDOWN, 0xBB, uint64_t{1} << 30);
        this->event(WM_CHAR, '+');
        this->event(WM_KEYUP, 0xBB);
        this->event(WM_CHAR, 0xD83D);
        this->event(WM_CHAR, 0xDE42);
        this->update();
        const auto press = this->poll();
        EXPECT_EQ(wire::Key, press.first.op);
        EXPECT_EQ(6, press.first.a[0]);
        EXPECT_EQ('+', press.first.a[1]);
        EXPECT_EQ(1, press.first.a[3]);
        EXPECT_EQ("+", press.second);
        const auto release = this->poll();
        EXPECT_EQ(7, release.first.a[0]);
        EXPECT_EQ('+', release.first.a[1]);
        EXPECT_EQ("", release.second);
        EXPECT_EQ("\xF0\x9F\x99\x82", this->poll().second);
        EXPECT_EQ(7, this->poll().first.a[0]);
        EXPECT_EQ(wire::Empty, this->poll().first.op);
    }

    TEST_F(NativeRasterTest, SurrogatePairsRetainThePhysicalKeyRepeatAndRelease)
    {
        this->event(WM_KEYDOWN, 'A', uint64_t{1} << 30);
        this->event(WM_CHAR, 0xD83D);
        this->update();
        EXPECT_EQ(wire::Empty, this->poll().first.op);
        this->event(WM_CHAR, 0xDE42);
        this->event(WM_KEYUP, 'A');
        this->update();
        const auto press = this->poll();
        EXPECT_EQ(wire::Key, press.first.op);
        EXPECT_EQ(6, press.first.a[0]);
        EXPECT_EQ(0x1F642, press.first.a[1]);
        EXPECT_EQ(1, press.first.a[3]);
        EXPECT_EQ("\xF0\x9F\x99\x82", press.second);
        const auto release = this->poll();
        EXPECT_EQ(7, release.first.a[0]);
        EXPECT_EQ(0x1F642, release.first.a[1]);
        EXPECT_EQ("", release.second);
        EXPECT_EQ(wire::Empty, this->poll().first.op);
    }

    TEST_F(NativeRasterTest, UnpairedSurrogatesDoNotLeakAcrossTextOrFocusChanges)
    {
        this->event(WM_CHAR, 0xDE42);
        this->event(WM_CHAR, 0xD83D);
        this->event(WM_CHAR, 'a');
        this->event(WM_CHAR, 0xD83D);
        this->event(WM_CHAR, 0xD834);
        this->event(WM_CHAR, 0xDD1E);
        this->event(WM_CHAR, 0xD83D);
        this->event(WM_KILLFOCUS);
        this->event(WM_CHAR, 0xDE42);
        this->update();
        const auto letter = this->poll();
        EXPECT_EQ("a", letter.second);
        EXPECT_EQ('A', letter.first.a[1]);
        EXPECT_EQ(7, this->poll().first.a[0]);
        const auto music = this->poll();
        EXPECT_EQ("\xF0\x9D\x84\x9E", music.second);
        EXPECT_EQ(0x1D11E, music.first.a[1]);
        EXPECT_EQ(7, this->poll().first.a[0]);
        EXPECT_EQ(wire::Focus, this->poll().first.op);
        EXPECT_EQ(wire::Empty, this->poll().first.op);
    }

    TEST_F(NativeRasterTest, WheelAxesPreserveCoordinatesModifiersAndQtDirection)
    {
        this->event(WM_KEYDOWN, VK_CONTROL);
        for (const int32_t delta : {120, -120})
        {
            const auto wparam = uint64_t{static_cast<uint16_t>(delta)} << 16;
            const auto point = (uint64_t{34} << 16) | static_cast<uint16_t>(-12);
            this->event(WM_MOUSEWHEEL, wparam, point);
            this->event(WM_MOUSEHWHEEL, wparam, point);
        }
        this->update();
        this->poll();
        for (const int32_t delta : {120, -120})
        {
            for (const auto expected :
                 {std::array<int32_t, 8>{-12, 34, 0, delta, 0x04000000}, std::array<int32_t, 8>{-12, 34, -delta, 0, 0x04000000}})
            {
                const auto packet = this->poll().first;
                EXPECT_EQ(wire::Wheel, packet.op);
                EXPECT_EQ(expected, packet.a);
            }
        }
        EXPECT_EQ(wire::Empty, this->poll().first.op);
    }

    TEST_F(NativeRasterTest, BackAndForwardButtonsPreserveTransitionsAndReleaseOnFocusLoss)
    {
        const auto point = (uint64_t{34} << 16) | uint16_t{12};
        this->event(WM_XBUTTONDOWN, uint64_t{1} << 16, point);
        this->event(WM_XBUTTONDOWN, uint64_t{2} << 16, point);
        this->event(WM_XBUTTONUP, uint64_t{1} << 16, point);
        this->event(WM_XBUTTONUP, uint64_t{2} << 16, point);
        this->event(WM_XBUTTONDOWN, uint64_t{1} << 16, point);
        this->event(WM_XBUTTONDOWN, uint64_t{2} << 16, point);
        this->event(WM_KILLFOCUS);
        this->update();
        for (const auto expected :
             {std::array<int32_t, 8>{12, 34, 8, 8, 2}, std::array<int32_t, 8>{12, 34, 24, 16, 2}, std::array<int32_t, 8>{12, 34, 16, 8, 3},
              std::array<int32_t, 8>{12, 34, 0, 16, 3}, std::array<int32_t, 8>{12, 34, 8, 8, 2}, std::array<int32_t, 8>{12, 34, 24, 16, 2},
              std::array<int32_t, 8>{0, 0, 16, 8, 3}, std::array<int32_t, 8>{0, 0, 0, 16, 3}})
        {
            const auto packet = this->poll().first;
            EXPECT_EQ(wire::Mouse, packet.op);
            EXPECT_EQ(expected, packet.a);
        }
        EXPECT_EQ(wire::Focus, this->poll().first.op);
        EXPECT_EQ(wire::Empty, this->poll().first.op);
    }

    TEST_F(NativeRasterTest, MouseCoordinatesButtonsAndCloseReachTheGuestWithoutChangingGuestKeyState)
    {
        this->event(WM_LBUTTONDOWN, 0, (uint64_t{21} << 16) | uint16_t{12});
        this->event(WM_LBUTTONUP, 0, (uint64_t{21} << 16) | uint16_t{12});
        this->event(WM_KEYDOWN, '2');
        this->event(WM_CHAR, '2');
        this->event(WM_KEYUP, '2');
        this->event(WM_CLOSE);
        this->update();
        const auto down = this->poll().first;
        EXPECT_EQ(wire::Mouse, down.op);
        EXPECT_EQ((std::array<int32_t, 6>{12, 21, 1, 1, 2, 0}),
                  (std::array<int32_t, 6>{down.a[0], down.a[1], down.a[2], down.a[3], down.a[4], down.a[5]}));
        const auto up = this->poll().first;
        EXPECT_EQ(0, up.a[2]);
        EXPECT_EQ(3, up.a[4]);
        this->poll();
        this->poll();
        EXPECT_EQ(wire::Close, this->poll().first.op);
        EXPECT_EQ(0, this->emu->process.key_state['2']);
    }

    TEST_F(NativeRasterTest, AFullGuestQueueRetainsKeyReleasesUntilItDrains)
    {
        wire::Packet filler{};
        filler.op = wire::Focus;
        filler.id = 1;
        filler.session = this->session->identity();
        for (size_t i = 0; i < wire::max_events; ++i)
        {
            ASSERT_TRUE(this->session->enqueue_input(filler));
        }
        this->event(WM_KEYDOWN, '2');
        this->event(WM_CHAR, '2');
        this->event(WM_KEYUP, '2');
        this->update();
        for (size_t i = 0; i < wire::max_events; ++i)
        {
            EXPECT_EQ(wire::Focus, this->poll().first.op);
        }
        this->update();
        EXPECT_EQ(6, this->poll().first.a[0]);
        EXPECT_EQ(7, this->poll().first.a[0]);
    }

    TEST_F(NativeRasterTest, FocusLossReleasesPressedKeysAndButtons)
    {
        this->event(WM_KEYDOWN, VK_CONTROL);
        this->event(WM_LBUTTONDOWN);
        this->event(WM_KILLFOCUS);
        this->update();
        EXPECT_EQ(6, this->poll().first.a[0]);
        EXPECT_EQ(2, this->poll().first.a[4]);
        EXPECT_EQ(7, this->poll().first.a[0]);
        EXPECT_EQ(3, this->poll().first.a[4]);
        const auto focus = this->poll().first;
        EXPECT_EQ(wire::Focus, focus.op);
        EXPECT_EQ(0, focus.a[0]);
    }

    TEST_F(NativeRasterTest, HostResizeIsQueuedOnceWithoutPushingTheOldGuestGeometryBack)
    {
        this->event(WM_SIZE, 0, (uint64_t{480} << 16) | 640);
        this->update();
        const auto resize = this->poll().first;
        EXPECT_EQ(wire::Resize, resize.op);
        EXPECT_EQ(640, resize.a[2]);
        EXPECT_EQ(480, resize.a[3]);
        this->update();
        EXPECT_EQ(wire::Empty, this->poll().first.op);
        EXPECT_EQ(0u, this->ui->geometry_changes);
        this->send(wire::Geometry, {10, 20, 640, 480});
        this->update();
        EXPECT_EQ(650, this->ui->windows.at(this->host).rect.right);
    }

    TEST_F(NativeRasterTest, ClosingTheDeviceDestroysHostWindowsAndDropsPendingInput)
    {
        this->event(WM_CLOSE);
        this->emu->process.devices.erase(this->device);
        this->update();
        EXPECT_TRUE(this->ui->windows.empty());
        EXPECT_EQ(this->ui->creations, this->ui->destructions);
    }

    TEST_F(NativeRasterTest, WindowRecreationDoesNotReuseAnOldHostRoute)
    {
        const auto previous = this->host;
        this->send(wire::Destroy);
        this->update();
        this->send(wire::Create, {10, 20, 320, 240, 1, 0, 0, 1000});
        this->send(wire::Visible, {1});
        this->update();
        EXPECT_NE(previous, this->ui->windows.begin()->first);
        EXPECT_EQ(2u, this->ui->creations);
        EXPECT_EQ(1u, this->ui->destructions);
    }

    TEST_F(NativeRasterTest, RepeatedSessionsReleaseWindowsAndQueuedInput)
    {
        for (size_t cycle = 0; cycle < 16; ++cycle)
        {
            this->event(WM_CLOSE);
            this->send(wire::Destroy);
            this->update();
            EXPECT_TRUE(this->ui->windows.empty());
            this->send(wire::End);
            this->sequence = 0;
            this->send(wire::Hello);
            this->send(wire::Create, {10, 20, 320, 240, 1, 0, 0, 1000});
            this->send(wire::Visible, {1});
            this->update();
            ASSERT_EQ(1u, this->ui->windows.size());
            this->host = this->ui->windows.begin()->first;
        }
        this->presenter->clear();
        EXPECT_EQ(this->ui->creations, this->ui->destructions);
    }

    TEST_F(NativeRasterTest, AMenuCompositesIntoItsOwnerAndReceivesTranslatedInput)
    {
        this->send(wire::Geometry, {10, 20, 4, 4});
        this->frame(1, 4, 4, {10, 20, 30, 255});
        this->show_window(2, 9);
        this->frame(2, 2, 2, {20, 40, 60, 128});
        this->update();
        EXPECT_EQ(1u, this->ui->windows.size());
        std::vector<uint8_t> expected;
        for (size_t pixel = 0; pixel < 16; ++pixel)
        {
            expected.insert(expected.end(), {10, 20, 30, 255});
        }
        for (const size_t pixel : {5u, 6u, 9u, 10u})
        {
            const std::array<uint8_t, 4> blended{25, 50, 75, 255};
            std::copy(blended.begin(), blended.end(), expected.begin() + pixel * 4);
        }
        EXPECT_EQ(expected, this->ui->pixels.at(this->host));
        this->event(WM_LBUTTONDOWN, 0, (uint64_t{2} << 16) | 2);
        this->event(WM_KEYDOWN, VK_ESCAPE);
        this->update();
        const auto mouse = this->poll().first;
        EXPECT_EQ(wire::Mouse, mouse.op);
        EXPECT_EQ(2u, mouse.id);
        EXPECT_EQ(1, mouse.a[0]);
        EXPECT_EQ(1, mouse.a[1]);
        const auto key = this->poll();
        EXPECT_EQ(wire::Key, key.first.op);
        EXPECT_EQ(2u, key.first.id);
        EXPECT_EQ(0x01000000, key.first.a[1]);
    }

    TEST_F(NativeRasterTest, HidingAMenuRestoresTheBaseFrameAndOwnerFocus)
    {
        this->focus_owner();
        this->frame(1, 2, 2, {10, 20, 30, 255});
        this->show_window(2, 9);
        this->send_to(2, wire::Focus, {1});
        this->frame(2, 2, 2, {50, 60, 70, 255});
        this->update();
        this->ui->activations.clear();
        this->send_to(2, wire::Visible, {0});
        this->update();
        EXPECT_EQ((std::vector<uint8_t>{10, 20, 30, 255, 10, 20, 30, 255, 10, 20, 30, 255, 10, 20, 30, 255}),
                  this->ui->pixels.at(this->host));
        const auto focus = this->poll().first;
        EXPECT_EQ(wire::Focus, focus.op);
        EXPECT_EQ(1u, focus.id);
        EXPECT_EQ(1, focus.a[0]);
        EXPECT_EQ((std::vector<hwnd>{this->host}), this->ui->activations);
        this->send_to(2, wire::Destroy);
        this->update();
        EXPECT_EQ(wire::Empty, this->poll().first.op);
    }

    TEST_F(NativeRasterTest, TooltipsDoNotOwnHostFocusOrInterceptOwnerClicks)
    {
        this->focus_owner();
        this->show_window(2, 13);
        this->send_to(2, wire::Focus, {1});
        this->update();
        ASSERT_EQ(1u, this->ui->windows.size());
        this->event(WM_LBUTTONDOWN);
        this->event(WM_LBUTTONUP);
        this->update();
        EXPECT_EQ(1u, this->poll().first.id);
        EXPECT_EQ(1u, this->poll().first.id);
        this->send_to(2, wire::Visible, {0});
        this->update();
        EXPECT_EQ(wire::Focus, this->poll().first.op);
        this->event(WM_KEYDOWN, 0x75);
        this->event(WM_KEYUP, 0x75);
        this->update();
        const auto key = this->poll().first;
        EXPECT_EQ(1u, key.id);
        EXPECT_EQ(0x01000035, key.a[1]);
        EXPECT_EQ(7, this->poll().first.a[0]);
    }

    TEST_F(NativeRasterTest, DismissingATooltipAfterDeactivationDoesNotStealFocusBack)
    {
        this->focus_owner();
        this->show_window(2, 13);
        this->send_to(2, wire::Focus, {1});
        this->update();
        this->event(WM_KILLFOCUS);
        this->update();
        ASSERT_EQ(wire::Focus, this->poll().first.op);
        this->ui->activations.clear();
        this->send_to(2, wire::Visible, {0});
        this->update();
        EXPECT_TRUE(this->ui->activations.empty());
        EXPECT_EQ(wire::Empty, this->poll().first.op);
    }

    TEST_F(NativeRasterTest, WindowModalityBlocksTheOwnerAndKeepsUnrelatedWindowsUsable)
    {
        this->show_window(2, 1, 0, 0, "Other");
        this->show_window(3, 3, 1, 1, "Modal");
        this->send_to(3, wire::Focus, {1});
        this->update();
        const auto other = this->named_host(u"Other");
        const auto modal = this->named_host(u"Modal");
        EXPECT_EQ(this->host, this->ui->windows.at(modal).owner);
        EXPECT_FALSE(this->ui->windows.at(this->host).enabled);
        EXPECT_TRUE(this->ui->windows.at(other).enabled);
        this->event(WM_LBUTTONDOWN);
        this->update();
        const auto focus = this->poll().first;
        EXPECT_EQ(wire::Focus, focus.op);
        EXPECT_EQ(3u, focus.id);
        EXPECT_EQ(wire::Empty, this->poll().first.op);
        this->event(WM_KEYDOWN, '7');
        this->event(WM_CHAR, '7');
        this->event(WM_KEYUP, '7');
        this->update();
        const auto press = this->poll();
        EXPECT_EQ(3u, press.first.id);
        EXPECT_EQ("7", press.second);
        EXPECT_EQ(3u, this->poll().first.id);
        this->emu->handle_ui_event({.window = other, .message = WM_LBUTTONDOWN});
        this->update();
        const auto mouse = this->poll().first;
        EXPECT_EQ(wire::Mouse, mouse.op);
        EXPECT_EQ(2u, mouse.id);
    }

    TEST_F(NativeRasterTest, ApplicationModalityBlocksPeersAndClosingItRestoresTheOwner)
    {
        this->show_window(2, 1, 0, 0, "Other");
        this->show_window(3, 3, 2, 1, "Modal");
        this->send_to(3, wire::Focus, {1});
        this->update();
        const auto other = this->named_host(u"Other");
        const auto modal = this->named_host(u"Modal");
        EXPECT_FALSE(this->ui->windows.at(other).enabled);
        this->emu->handle_ui_event({.window = other, .message = WM_CLOSE});
        this->update();
        EXPECT_EQ(wire::Empty, this->poll().first.op);
        this->emu->handle_ui_event({.window = modal, .message = WM_SETFOCUS});
        this->update();
        ASSERT_EQ(wire::Focus, this->poll().first.op);
        this->ui->activations.clear();
        this->send_to(3, wire::Destroy);
        this->update();
        EXPECT_TRUE(this->ui->windows.at(this->host).enabled);
        EXPECT_TRUE(this->ui->windows.at(other).enabled);
        const auto focus = this->poll().first;
        EXPECT_EQ(wire::Focus, focus.op);
        EXPECT_EQ(1u, focus.id);
        ASSERT_FALSE(this->ui->activations.empty());
        EXPECT_EQ(this->host, this->ui->activations.back());
    }

    TEST_F(NativeRasterTest, TheNewestModalRemainsUsableWhenAnotherModalIsOpen)
    {
        this->show_window(2, 3, 2, 1, "Older");
        this->show_window(3, 3, 2, 1, "Newer");
        this->update();
        EXPECT_FALSE(this->ui->windows.at(this->named_host(u"Older")).enabled);
        EXPECT_TRUE(this->ui->windows.at(this->named_host(u"Newer")).enabled);
    }

    TEST_F(NativeRasterTest, DeviceTeardownDestroysTransientHostWindowsBeforeTheirOwner)
    {
        this->show_window(2, 3, 0, 1, "Owned");
        this->update();
        const auto child = this->named_host(u"Owned");
        this->emu->process.devices.erase(this->device);
        this->update();
        EXPECT_EQ((std::vector<hwnd>{child, this->host}), this->ui->destroyed);
        EXPECT_TRUE(this->ui->windows.empty());
    }

    TEST_F(NativeRasterTest, RaisingAndLoweringMenusChangesTheirPixelsAndInputTarget)
    {
        this->send(wire::Geometry, {10, 20, 2, 2});
        this->frame(1, 2, 2, {0, 0, 0, 255});
        for (const uint32_t id : {2u, 3u})
        {
            this->show_window(id, 9);
            this->send_to(id, wire::Geometry, {10, 20, 2, 2});
        }
        this->frame(2, 2, 2, {10, 0, 0, 255});
        this->frame(3, 2, 2, {20, 0, 0, 255});
        this->update();
        EXPECT_EQ(20, this->ui->pixels.at(this->host)[0]);
        this->send_to(3, wire::Raise, {0});
        this->update();
        EXPECT_EQ(10, this->ui->pixels.at(this->host)[0]);
        this->event(WM_KEYDOWN, VK_ESCAPE);
        this->event(WM_KEYUP, VK_ESCAPE);
        this->update();
        EXPECT_EQ(2u, this->poll().first.id);
        this->poll();
        this->send_to(3, wire::Raise, {1});
        this->update();
        EXPECT_EQ(20, this->ui->pixels.at(this->host)[0]);
        this->event(WM_KEYDOWN, VK_ESCAPE);
        this->update();
        EXPECT_EQ(3u, this->poll().first.id);
    }

    TEST_F(NativeRasterTest, OverlayCompositionClipsAndMapsLogicalCoordinatesToPixels)
    {
        this->send(wire::Geometry, {10, 20, 2, 2});
        this->frame(1, 4, 4, {10, 20, 30, 255});
        this->show_window(2, 9);
        this->frame(2, 2, 2, {20, 40, 60, 128});
        this->update();
        auto pixels = this->ui->pixels.at(this->host);
        ASSERT_EQ(64u, pixels.size());
        EXPECT_EQ(10, pixels[0]);
        EXPECT_EQ(25, pixels[10 * 4]);
        EXPECT_EQ(50, pixels[10 * 4 + 1]);
        EXPECT_EQ(75, pixels[15 * 4 + 2]);
        EXPECT_EQ(255, pixels[15 * 4 + 3]);
        this->send_to(2, wire::Geometry, {9, 19, 2, 2});
        this->update();
        pixels = this->ui->pixels.at(this->host);
        EXPECT_EQ(25, pixels[0]);
        EXPECT_EQ(10, pixels[15 * 4]);
    }

    TEST_F(NativeRasterTest, HostCursorRequestsFollowTheActiveMenuAndBlankVisibility)
    {
        this->send(wire::Cursor, {4});
        this->event(WM_MOUSEMOVE);
        this->update();
        EXPECT_EQ(32513u, this->ui->cursor);
        EXPECT_TRUE(this->ui->cursor_visible);
        this->show_window(2, 9);
        this->send_to(2, wire::Cursor, {13});
        this->update();
        EXPECT_EQ(32649u, this->ui->cursor);
        this->send_to(2, wire::Cursor, {10});
        this->update();
        EXPECT_FALSE(this->ui->cursor_visible);
        this->send_to(2, wire::Visible, {0});
        this->update();
        EXPECT_EQ(32513u, this->ui->cursor);
        EXPECT_TRUE(this->ui->cursor_visible);
    }

} // namespace sogen::test
