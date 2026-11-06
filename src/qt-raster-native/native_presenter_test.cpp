#include "native_presenter.hpp"
#include <emulation_test_utils.hpp>
#include <devices/qt_raster.hpp>

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
                this->windows.erase(value);
                this->pixels.erase(value);
                ++this->destructions;
            }

            void set_window_rect(const hwnd value, const RECT& rect) override
            {
                this->windows.at(value).rect = rect;
                ++this->geometry_changes;
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
                wire::Packet packet{};
                packet.op = op;
                packet.id = op == wire::Hello || op == wire::End || op == wire::Poll ? 0 : 1;
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

            std::unique_ptr<windows_emulator> emu{};
            recording_ui* ui{};
            wire::session* session{};
            std::unique_ptr<wire::native_presenter> presenter{};
            handle device{};
            hwnd host{};
            uint32_t sequence{};
        };
    }

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
        for (const int16_t delta : {120, -120})
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

}
