#include <gtest/gtest.h>

#include "macos_test_utils.hpp"

#include <gui/macos_ui_state.hpp>
#include <gui/macos_window_server_mig.hpp>
#include <gui/skylight_routines.hpp>
#include <mach/mig_kernel_servers.hpp>

#include <algorithm>
#include <cstring>
#include <vector>

namespace
{
    using namespace sogen::mach; // NOLINT(google-build-using-namespace)

    mig_server_table& window_server_table()
    {
        static mig_server_table table = [] {
            mig_server_table built{};
            sogen::register_window_server_mig_routines(built);
            return built;
        }();

        return table;
    }

    std::vector<uint8_t> shmem_request_body(const uint32_t window)
    {
        std::vector<uint8_t> body(NDR_RECORD_SIZE + sizeof(uint32_t), 0);
        std::ranges::copy(NDR_RECORD, body.begin());
        write_u32(body, NDR_RECORD_SIZE, window);
        return body;
    }

    std::vector<uint8_t> ask_for_shmem(sogen::macos_emulator& emu, const uint32_t window)
    {
        const auto* routine = window_server_table().find(kernel_object_kind::window_server_connection,
                                                         sogen::MACOS_MIG_GET_WINDOW_SHMEM_REFERENCE);
        EXPECT_NE(routine, nullptr);
        if (routine == nullptr)
        {
            return {};
        }

        msg_call call{};
        call.header = {.bits = make_bits(disposition::copy_send, disposition::make_send_once),
                       .size = static_cast<uint32_t>(MSG_HEADER_SIZE),
                       .remote_port = 0x4103,
                       .local_port = 0x70b,
                       .voucher_port = 0,
                       .id = sogen::MACOS_MIG_GET_WINDOW_SHMEM_REFERENCE};

        const auto body = shmem_request_body(window);
        return (*routine)(emu, make_mig_request(call, body, kernel_object_kind::window_server_connection));
    }

    // CGSWindowConstructInternal type-checks this reply as hard as SLSNewConnection checks 32000:
    // msgh_size exactly 60, no remote port, exactly one descriptor, and the descriptor's
    // {disposition, type} pair read as a halfword at +0x26 has to be exactly 0x0011. Anything else and
    // the client builds no window, which makes AppKit's -[NSCGSWindow initWithConnectionID:flags:] call
    // NSCGSPanic and _exit(0). Measured on 25G76; see
    TEST(WindowShmemReference, AnswersTheShapeCgsWindowConstructInternalAccepts)
    {
        const auto emu = macos_test::make_emulator();

        auto* window = emu->ui.server.create_window(emu->ui.server.main_connection(), 40, 60, 320, 232);
        ASSERT_NE(window, nullptr);

        const auto reply = ask_for_shmem(*emu, window->id);
        ASSERT_EQ(reply.size(), 60u);

        const auto header = read_msg_header(reply);
        EXPECT_EQ(header.size, 60u);
        EXPECT_EQ(header.remote_port, PORT_NULL);
        EXPECT_EQ(header.id, sogen::MACOS_MIG_GET_WINDOW_SHMEM_REFERENCE + subsystem::reply_offset);
        EXPECT_NE(header.bits & BITS_COMPLEX, 0u);
        EXPECT_EQ(read_u32(reply, MSG_HEADER_SIZE), 1u) << "exactly one descriptor";

        const auto descriptor = read_port_descriptor(std::span{reply}.subspan(MSG_HEADER_SIZE + MSG_BODY_SIZE));
        EXPECT_EQ(descriptor.type, descriptor_type::port);
        EXPECT_NE(descriptor.name, PORT_NULL);

        const auto args = MSG_HEADER_SIZE + MSG_BODY_SIZE + PORT_DESCRIPTOR_SIZE + NDR_RECORD_SIZE;
        EXPECT_EQ(read_u32(reply, args), sogen::MACOS_MAIN_CONNECTION_ID) << "word 0 is the owning connection id";
        EXPECT_EQ(read_u32(reply, args + sizeof(uint32_t)), sogen::MACOS_WINDOW_SHMEM_LAYOUT_VERSION);
        EXPECT_EQ(read_u32(reply, args + 2 * sizeof(uint32_t)), 0u);
    }

    // The descriptor page has to survive mach_vm_map for 0xb0 bytes, and it carries the window's
    // geometry: SkyLight reads the origin and the window-to-screen affine straight out of it.
    TEST(WindowShmemReference, TheDescriptorPageCarriesTheWindowGeometry)
    {
        const auto emu = macos_test::make_emulator();

        auto* window = emu->ui.server.create_window(emu->ui.server.main_connection(), 40, 60, 320, 232);
        ASSERT_NE(window, nullptr);

        ASSERT_FALSE(ask_for_shmem(*emu, window->id).empty());

        const auto* record = emu->ui.server.find_window(window->id);
        ASSERT_NE(record, nullptr);
        ASSERT_NE(record->shmem_address, 0u);
        ASSERT_NE(record->shmem_entry, PORT_NULL);

        std::array<uint8_t, sogen::MACOS_WINDOW_SHMEM_BYTES> page{};
        ASSERT_TRUE(emu->memory.try_read_memory(record->shmem_address, page.data(), page.size()));

        const auto f32 = [&page](const size_t offset) {
            float value = 0;
            std::memcpy(&value, page.data() + offset, sizeof(value));
            return value;
        };

        const auto u32 = [&page](const size_t offset) {
            uint32_t value = 0;
            std::memcpy(&value, page.data() + offset, sizeof(value));
            return value;
        };

        EXPECT_FLOAT_EQ(f32(0x0c), 40.0f);
        EXPECT_FLOAT_EQ(f32(0x10), 60.0f);
        EXPECT_FLOAT_EQ(f32(0x14), 1.0f) << "the window-to-screen affine is the identity";
        EXPECT_FLOAT_EQ(f32(0x20), 1.0f);
        EXPECT_FLOAT_EQ(f32(0x24), -40.0f);
        EXPECT_FLOAT_EQ(f32(0x28), -60.0f);
        EXPECT_FLOAT_EQ(f32(0x2c), 1.0f) << "a zero resolution would divide by zero inside SkyLight";
        EXPECT_EQ(u32(0x98), sogen::MACOS_MAIN_CONNECTION_ID);

        // The page is refreshed from the window rather than frozen at the moment it was handed out.
        auto* mutable_window = emu->ui.server.find_window(window->id);
        mutable_window->x = 7;
        emu->ui.sync_window(*emu, *mutable_window);

        ASSERT_TRUE(emu->memory.try_read_memory(record->shmem_address, page.data(), page.size()));
        EXPECT_FLOAT_EQ(f32(0x0c), 7.0f);
        EXPECT_FLOAT_EQ(f32(0x24), -7.0f);
    }

    // Measured against the real WindowServer with a window the connection does not own: a simple
    // mig_reply_error_t carrying kCGErrorFailure, which the client reads as "there is no such window".
    TEST(WindowShmemReference, AnUnknownWindowGetsTheMeasuredFailureCode)
    {
        const auto emu = macos_test::make_emulator();

        const auto reply = ask_for_shmem(*emu, 0x4eb9);
        ASSERT_EQ(reply.size(), MSG_HEADER_SIZE + NDR_RECORD_SIZE + sizeof(uint32_t));

        const auto header = read_msg_header(reply);
        EXPECT_EQ(header.bits & BITS_COMPLEX, 0u);
        EXPECT_EQ(read_u32(reply, MSG_HEADER_SIZE + NDR_RECORD_SIZE), static_cast<uint32_t>(sogen::MACOS_CG_ERROR_FAILURE));
    }
}
