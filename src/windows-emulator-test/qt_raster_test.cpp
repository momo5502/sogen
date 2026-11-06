#include "emulation_test_utils.hpp"
#include <devices/qt_raster.hpp>
#include <syscall_utils.hpp>
#include <cstring>

namespace sogen::syscalls
{
    NTSTATUS handle_NtOpenFile(const syscall_context& c, emulator_object<handle> file_handle, ACCESS_MASK desired_access,
                               emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                               emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block, ULONG share_access,
                               ULONG open_options);
}

namespace sogen::test
{
    namespace
    {
        namespace raster = qt_raster;

        windows_emulator make_raster_emulator()
        {
            emulator_interfaces interfaces{};
            interfaces.ui = std::make_unique<null_ui_backend>();
            interfaces.audio = std::make_unique<null_audio_backend>();
            interfaces.console = std::make_unique<null_console_backend>();
            return windows_emulator{
                create_x86_64_emulator_from_environment(),
                emulator_settings{.disable_logging = true, .emulation_root = get_emulator_root(), .load_registry = false},
                {},
                std::move(interfaces)};
        }

        struct client
        {
            raster::session session{};
            uint32_t sequence{};
            std::vector<uint8_t> reply{};

            raster::Packet packet(raster::Op op, uint32_t id = 0) const
            {
                raster::Packet value{};
                value.op = op;
                value.id = id;
                value.seq = sequence + 1;
                value.session = session.identity();
                return value;
            }

            raster::result send(raster::Packet packet, std::span<const uint8_t> payload = {}, size_t capacity = 4096, bool writable = true)
            {
                packet.size = static_cast<uint32_t>(payload.size());
                std::vector<uint8_t> request(sizeof(packet));
                std::memcpy(request.data(), &packet, sizeof(packet));
                request.insert(request.end(), payload.begin(), payload.end());
                const auto result = session.exchange(request, capacity, [&](std::span<const uint8_t> bytes) {
                    if (!writable)
                    {
                        return false;
                    }
                    reply.assign(bytes.begin(), bytes.end());
                    return true;
                });
                if (result == raster::result::success)
                {
                    sequence = packet.seq;
                }
                return result;
            }

            raster::Packet response() const
            {
                raster::Packet value{};
                std::memcpy(&value, reply.data(), sizeof(value));
                return value;
            }

            void start()
            {
                EXPECT_EQ(raster::result::success, send(packet(raster::Hello)));
                EXPECT_NE(0, session.identity());
            }

            raster::Packet creation(uint32_t id = 1) const
            {
                auto value = packet(raster::Create, id);
                value.a = {10, 20, 320, 240, 1, 0, 0, 1000};
                return value;
            }

            raster::Packet key(uint32_t id = 1) const
            {
                auto value = packet(raster::Key, id);
                value.a[0] = 6;
                value.a[1] = '2';
                value.size = 1;
                return value;
            }
        };
    }

    TEST(QtRasterSessionTest, RejectedRequestsDoNotConsumeSequenceOrMutateWindows)
    {
        client c;
        c.start();
        auto create = c.creation();
        create.a[2] = 0;
        EXPECT_EQ(raster::result::invalid_parameter, c.send(create));
        EXPECT_TRUE(c.session.windows().empty());
        create = c.creation();
        EXPECT_EQ(raster::result::access_violation, c.send(create, {}, 4096, false));
        EXPECT_TRUE(c.session.windows().empty());
        ASSERT_EQ(raster::result::success, c.send(create));
        auto geometry = c.packet(raster::Geometry, 1);
        geometry.a = {INT32_MAX, 20, 2, 240};
        EXPECT_EQ(raster::result::invalid_parameter, c.send(geometry));
        EXPECT_EQ(320, c.session.windows().at(1).geometry[2]);
        EXPECT_EQ(raster::result::invalid_parameter, c.send(c.packet(static_cast<raster::Op>(0xFFFFFFFF), 1)));
        auto title = c.packet(raster::Title, 1);
        const std::array<uint8_t, 3> surrogate{0xED, 0xA0, 0x80};
        EXPECT_EQ(raster::result::invalid_parameter, c.send(title, surrogate));
        const std::array<uint8_t, 2> text{'o', 'k'};
        EXPECT_EQ(raster::result::success, c.send(title, text));
        EXPECT_EQ("ok", c.session.windows().at(1).title);
        EXPECT_EQ(raster::result::invalid_parameter, c.send(title, text));
        auto focus = c.packet(raster::Focus, 1);
        focus.a[0] = 1;
        ASSERT_EQ(raster::result::success, c.send(focus));
        EXPECT_FALSE(c.session.windows().at(1).raised);
        EXPECT_TRUE(c.session.windows().at(1).focused);
        auto raise = c.packet(raster::Raise, 1);
        raise.a[0] = 1;
        ASSERT_EQ(raster::result::success, c.send(raise));
        focus.seq = c.sequence + 1;
        focus.a[0] = 0;
        ASSERT_EQ(raster::result::success, c.send(focus));
        EXPECT_TRUE(c.session.windows().at(1).raised);
        EXPECT_FALSE(c.session.windows().at(1).focused);
    }

    TEST(QtRasterSessionTest, FrameOwnsPixelsAndRejectsInvalidStrideAndDirtyRect)
    {
        client c;
        c.start();
        ASSERT_EQ(raster::result::success, c.send(c.creation()));
        auto frame = c.packet(raster::Frame, 1);
        frame.a = {2, 2, 8, 1000, 0, 0, 2, 2};
        std::vector<uint8_t> pixels(16, 0x42);
        ASSERT_EQ(raster::result::success, c.send(frame, pixels));
        const auto captured = c.session.latest_frame();
        ASSERT_TRUE(captured);
        pixels[0] = 0x99;
        EXPECT_EQ(0x42, captured->pixels->at(0));
        frame = c.packet(raster::Frame, 1);
        frame.a = {2, 2, 7, 1000, 0, 0, 2, 2};
        EXPECT_EQ(raster::result::invalid_parameter, c.send(frame, pixels));
        frame.a[2] = 8;
        frame.a[6] = 3;
        EXPECT_EQ(raster::result::invalid_parameter, c.send(frame, pixels));
        EXPECT_EQ(1, c.session.frame_count());
        frame.a[6] = 2;
        EXPECT_EQ(raster::result::access_violation, c.send(frame, pixels, 4096, false));
        EXPECT_EQ(1, c.session.frame_count());
        ASSERT_EQ(raster::result::success, c.send(frame, pixels));
        EXPECT_EQ(0x42, captured->pixels->at(0));
        EXPECT_EQ(0x99, c.session.latest_frame()->pixels->at(0));
        ASSERT_EQ(raster::result::success, c.send(c.packet(raster::Destroy, 1)));
        EXPECT_EQ(0x42, captured->pixels->at(0));
        EXPECT_EQ(raster::result::success, c.send(c.packet(raster::End)));
        EXPECT_FALSE(c.session.active());
    }

    TEST(QtRasterSessionTest, PollRetriesPreserveInputOrderAndReplyLength)
    {
        client c;
        c.start();
        ASSERT_EQ(raster::result::success, c.send(c.creation()));
        const std::array<uint8_t, 1> first{'2'}, second{'3'};
        ASSERT_TRUE(c.session.enqueue_input(c.key(), first));
        ASSERT_TRUE(c.session.enqueue_input(c.key(), second));
        const auto poll = c.packet(raster::Poll);
        EXPECT_EQ(raster::result::buffer_too_small, c.send(poll, {}, sizeof(raster::Packet)));
        EXPECT_EQ(raster::result::access_violation, c.send(poll, {}, 4096, false));
        ASSERT_EQ(raster::result::success, c.send(poll));
        EXPECT_EQ(raster::Key, c.response().op);
        EXPECT_EQ(poll.seq, c.response().seq);
        EXPECT_EQ(c.session.identity(), c.response().session);
        ASSERT_EQ(sizeof(raster::Packet) + 1, c.reply.size());
        EXPECT_EQ('2', c.reply.back());
        ASSERT_EQ(raster::result::success, c.send(c.packet(raster::Poll)));
        EXPECT_EQ('3', c.reply.back());
        ASSERT_EQ(raster::result::success, c.send(c.packet(raster::Poll)));
        EXPECT_EQ(raster::Op{}, c.response().op);
        EXPECT_EQ(sizeof(raster::Packet), c.reply.size());
    }

    TEST(QtRasterSessionTest, SessionsRejectStalePacketsAndReinitializeAfterSequenceExhaustion)
    {
        client first, second;
        first.start();
        second.start();
        EXPECT_NE(first.session.identity(), second.session.identity());
        const auto stale = first.creation();
        EXPECT_EQ(raster::result::invalid_parameter, second.send(stale));
        auto poll = first.packet(raster::Poll);
        poll.seq = UINT32_MAX;
        ASSERT_EQ(raster::result::success, first.send(poll));
        poll.seq = 0;
        EXPECT_EQ(raster::result::invalid_parameter, first.send(poll));
        poll.seq = 1;
        EXPECT_EQ(raster::result::invalid_parameter, first.send(poll));
        const auto identity = first.session.identity();
        raster::Packet hello{};
        hello.op = raster::Hello;
        hello.seq = 1;
        ASSERT_EQ(raster::result::success, first.send(hello));
        EXPECT_NE(identity, first.session.identity());
        EXPECT_EQ(raster::result::invalid_parameter, first.send(stale));
        EXPECT_EQ(raster::result::success, first.send(first.creation()));
        EXPECT_EQ(raster::result::invalid_parameter, first.send(hello));
        EXPECT_TRUE(second.session.windows().empty());
    }

    TEST(QtRasterSessionTest, InputIsBoundedAndWindowDestructionDiscardsItsPendingEvents)
    {
        client c;
        c.start();
        ASSERT_EQ(raster::result::success, c.send(c.creation()));
        const std::array<uint8_t, 1> text{'2'};
        for (uint32_t i = 0; i < raster::max_events; ++i)
        {
            ASSERT_TRUE(c.session.enqueue_input(c.key(), text));
        }
        EXPECT_FALSE(c.session.enqueue_input(c.key(), text));
        EXPECT_EQ(raster::result::invalid_parameter, c.send(c.packet(raster::End)));
        ASSERT_EQ(raster::result::success, c.send(c.packet(raster::Destroy, 1)));
        EXPECT_FALSE(c.session.enqueue_input(c.key(), text));
        ASSERT_EQ(raster::result::success, c.send(c.packet(raster::Poll)));
        EXPECT_EQ(raster::Op{}, c.response().op);
        EXPECT_EQ(raster::result::success, c.send(c.packet(raster::End)));
    }

    TEST(QtRasterSessionTest, WireVersionAndExactLengthAreRequiredBeforeStartingASession)
    {
        raster::session session;
        raster::Packet hello{};
        hello.op = raster::Hello;
        hello.seq = 1;
        bool wrote{};
        const auto writer = [&](std::span<const uint8_t>) {
            wrote = true;
            return true;
        };
        auto bytes = std::span{reinterpret_cast<const uint8_t*>(&hello), sizeof(hello)};
        EXPECT_EQ(raster::result::invalid_parameter, session.exchange(bytes.first(63), 4096, writer));
        hello.protocol_version = 1;
        EXPECT_EQ(raster::result::invalid_parameter, session.exchange(bytes, 4096, writer));
        hello.protocol_version = raster::version;
        hello.size = 1;
        EXPECT_EQ(raster::result::invalid_parameter, session.exchange(bytes, 4096, writer));
        EXPECT_FALSE(session.active());
        EXPECT_FALSE(wrote);
        hello.size = 0;
        EXPECT_EQ(raster::result::success, session.exchange(bytes, 4096, writer));
        EXPECT_TRUE(session.active());
    }

    TEST(QtRasterSessionTest, WindowLimitAndParentLifetimeAreEnforcedWithoutLosingTheNextRequest)
    {
        client c;
        c.start();
        ASSERT_EQ(raster::result::success, c.send(c.creation()));
        for (uint32_t id = 2; id <= raster::max_windows; ++id)
        {
            auto child = c.creation(id);
            child.a[6] = 1;
            ASSERT_EQ(raster::result::success, c.send(child));
        }
        EXPECT_EQ(raster::result::resource_limit, c.send(c.creation(raster::max_windows + 1)));
        EXPECT_EQ(raster::result::invalid_parameter, c.send(c.packet(raster::Destroy, 1)));
        ASSERT_EQ(raster::result::success, c.send(c.packet(raster::Destroy, 2)));
        auto orphan = c.creation(2);
        orphan.a[6] = INT32_MAX;
        EXPECT_EQ(raster::result::invalid_parameter, c.send(orphan));
        EXPECT_EQ(raster::result::success, c.send(c.creation(2)));
        EXPECT_EQ(raster::max_windows, c.session.windows().size());
    }

    TEST(QtRasterSessionTest, FrameTakesOwnershipOfTheAlreadyReadPixelBuffer)
    {
        client c;
        c.start();
        ASSERT_EQ(raster::result::success, c.send(c.creation()));
        auto packet = c.packet(raster::Frame, 1);
        packet.a = {2, 2, 8, 1000, 0, 0, 2, 2};
        std::vector<uint8_t> pixels(16, 0x42);
        packet.size = static_cast<uint32_t>(pixels.size());
        const auto original = reinterpret_cast<uintptr_t>(pixels.data());
        ASSERT_EQ(raster::result::success,
                  c.session.exchange(packet, std::move(pixels), 4096, [](std::span<const uint8_t>) { return true; }));
        c.sequence = packet.seq;
        const auto retained = c.session.latest_frame();
        ASSERT_TRUE(retained);
        EXPECT_EQ(original, reinterpret_cast<uintptr_t>(retained->pixels->data()));
        EXPECT_EQ(16, retained->pixels->size());
        EXPECT_EQ(0x42, retained->pixels->at(15));
        ASSERT_EQ(raster::result::success, c.send(c.packet(raster::Destroy, 1)));
        ASSERT_EQ(raster::result::success, c.send(c.packet(raster::End)));
        EXPECT_EQ(0x42, retained->pixels->at(15));
    }

    TEST(QtRasterDeviceTest, ProtectedBuffersRejectAccessWithoutStartingTheSession)
    {
        auto emu = make_raster_emulator();
        const auto storage = emu.memory.allocate_memory(0x5000, memory_permission::read_write);
        auto device = create_qt_raster({});
        auto* raster_device = dynamic_cast<qt_raster_device*>(device.get());
        ASSERT_NE(nullptr, raster_device);
        io_device_context request{emu.memory};
        request.io_control_code = raster::ioctl;
        request.io_status_block = {emu.memory, storage};
        request.input_buffer = storage + 0x1000;
        request.input_buffer_length = sizeof(raster::Packet);
        request.output_buffer = storage + 0x3000;
        request.output_buffer_length = sizeof(raster::Packet);
        raster::Packet hello{};
        hello.op = raster::Hello;
        hello.seq = 1;
        emu.memory.write_memory(request.input_buffer, &hello, sizeof(hello));
        ASSERT_TRUE(emu.memory.protect_memory(storage + 0x1000, 0x1000, memory_permission::none));
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, device->io_control(emu, request));
        EXPECT_FALSE(raster_device->session().active());
        EXPECT_EQ(0, request.io_status_block.read().Information);
        raster_device->session().reset();
        ASSERT_TRUE(emu.memory.protect_memory(storage + 0x1000, 0x1000, memory_permission::read_write));

        request.input_buffer = storage + 0x2000 - sizeof(hello) + 1;
        emu.memory.write_memory(request.input_buffer, &hello, sizeof(hello));
        ASSERT_TRUE(emu.memory.protect_memory(storage + 0x2000, 0x1000, memory_permission::none));
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, device->io_control(emu, request));
        EXPECT_FALSE(raster_device->session().active());
        raster_device->session().reset();
        ASSERT_TRUE(emu.memory.protect_memory(storage + 0x2000, 0x1000, memory_permission::read_write));

        std::array<uint8_t, sizeof(raster::Packet)> sentinel{};
        sentinel.fill(0xA5);
        emu.memory.write_memory(request.output_buffer, sentinel.data(), sentinel.size());
        ASSERT_TRUE(emu.memory.protect_memory(storage + 0x3000, 0x1000, memory_permission::read));
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, device->io_control(emu, request));
        EXPECT_FALSE(raster_device->session().active());
        EXPECT_EQ(0, request.io_status_block.read().Information);
        std::array<uint8_t, sizeof(raster::Packet)> output{};
        emu.memory.read_memory(request.output_buffer, output.data(), output.size());
        EXPECT_EQ(sentinel, output);
        raster_device->session().reset();
        ASSERT_TRUE(emu.memory.protect_memory(storage + 0x3000, 0x1000, memory_permission::read_write));
        EXPECT_EQ(STATUS_SUCCESS, device->io_control(emu, request));
        EXPECT_TRUE(raster_device->session().active());
    }

    TEST(QtRasterDeviceTest, Wow64OpenReturnsAnErrorWithoutAnExceptionOrHandle)
    {
        auto emu = make_raster_emulator();
        emu.process.is_wow64_process = true;
        const auto storage = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        const syscall_context context{.win_emu = emu, .emu = emu.vcpu(0).cpu, .vcpu = emu.vcpu(0), .proc = emu.process};
        for (const auto path : {u"\\Device\\SogenQtBridge", u"\\??\\SogenQtBridge"})
        {
            emulator_allocator allocator{emu.memory, storage, 0x1000};
            const auto name = allocator.make_unicode_string(path);
            const auto attributes = allocator.reserve<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>>();
            OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>> value{};
            value.Length = sizeof(value);
            value.ObjectName = name.value();
            attributes.write(value);
            const auto output = allocator.reserve<handle>();
            constexpr handle sentinel{.bits = 0xDEADBEEF};
            output.write(sentinel);
            const auto status = allocator.reserve<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>();
            NTSTATUS result = STATUS_SUCCESS;
            EXPECT_NO_THROW(result = syscalls::handle_NtOpenFile(context, output, GENERIC_READ, attributes, status, 0, 0));
            EXPECT_EQ(STATUS_NOT_SUPPORTED, result);
            EXPECT_EQ(sentinel, output.read());
            EXPECT_FALSE(emu.process.exit_status.has_value());
        }
    }

    TEST(QtRasterDeviceTest, GuestBufferFailuresAndActiveSnapshotsDoNotLoseSessionState)
    {
        emulator_interfaces interfaces{};
        interfaces.ui = std::make_unique<null_ui_backend>();
        interfaces.audio = std::make_unique<null_audio_backend>();
        interfaces.console = std::make_unique<null_console_backend>();
        windows_emulator emu{create_x86_64_emulator_from_environment(),
                             emulator_settings{.disable_logging = true, .emulation_root = get_emulator_root(), .load_registry = false},
                             {},
                             std::move(interfaces)};
        emu.process.kusd.setup(emu.version, {});
        const auto storage = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        const auto handle = emu.process.devices.store(io_device_container{u"SogenQtBridge", emu, {}});
        auto* device = emu.process.devices.get(handle);
        io_device_context request{emu.memory};
        request.io_control_code = raster::ioctl;
        request.io_status_block = {emu.memory, storage};
        request.input_buffer = storage + 0x100;
        request.input_buffer_length = sizeof(raster::Packet);
        request.output_buffer = storage + 0x200;
        request.output_buffer_length = sizeof(raster::Packet);
        raster::Packet packet{};
        packet.op = raster::Hello;
        packet.seq = 1;
        emu.memory.write_memory(request.input_buffer, &packet, sizeof(packet));
        request.output_buffer_length = sizeof(packet) - 1;
        EXPECT_EQ(STATUS_BUFFER_TOO_SMALL, device->io_control(emu, request));
        EXPECT_TRUE(device->supports_serialization());
        EXPECT_EQ(0, request.io_status_block.read().Information);
        request.output_buffer_length = sizeof(packet);
        request.output_buffer = 0;
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, device->io_control(emu, request));
        EXPECT_TRUE(device->supports_serialization());
        request.output_buffer = storage + 0x200;
        ASSERT_EQ(STATUS_SUCCESS, device->io_control(emu, request));
        EXPECT_EQ(sizeof(packet), request.io_status_block.read().Information);
        EXPECT_FALSE(device->supports_serialization());
        EXPECT_THROW(emu.save_snapshot(), std::runtime_error);
        packet = emu.memory.read_memory<raster::Packet>(request.output_buffer);
        const auto identity = packet.session;
        packet.op = raster::End;
        packet.seq = 2;
        emu.memory.write_memory(request.input_buffer, &packet, sizeof(packet));
        request.input_buffer = 0;
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, device->io_control(emu, request));
        EXPECT_EQ(0, request.io_status_block.read().Information);
        request.input_buffer = storage + 0x100;
        ASSERT_EQ(STATUS_SUCCESS, device->io_control(emu, request));
        EXPECT_TRUE(device->supports_serialization());
        EXPECT_NO_THROW(emu.save_snapshot());
        EXPECT_NO_THROW(emu.restore_snapshot());
        device = emu.process.devices.get(handle);
        ASSERT_NE(nullptr, device);
        EXPECT_EQ(0, device->get_internal_device<qt_raster_device>()->session().identity());
        packet.op = raster::Hello;
        packet.seq = 1;
        packet.session = 0;
        emu.memory.write_memory(request.input_buffer, &packet, sizeof(packet));
        ASSERT_EQ(STATUS_SUCCESS, device->io_control(emu, request));
        EXPECT_NE(identity, emu.memory.read_memory<raster::Packet>(request.output_buffer).session);
    }
}
