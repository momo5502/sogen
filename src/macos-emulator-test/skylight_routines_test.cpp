#include <gtest/gtest.h>

#include "macos_test_utils.hpp"

#include <gui/macos_gui_exports.hpp>
#include <module/dyld_shared_cache.hpp>
#include <module/macos_cache_symbols.hpp>
#include <gui/macos_ui_state.hpp>
#include <gui/macos_io_surface_routines.hpp>
#include <gui/macos_process_manager_routines.hpp>
#include <gui/skylight_routines.hpp>
#include <screenshot_ui_backend.hpp>

#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <vector>

namespace
{
    constexpr uint64_t stub_base = 0x100010000ULL;
    constexpr uint64_t scratch = 0x340000000ULL;

    sogen::screenshot_ui_backend& attach_screenshot(sogen::macos_emulator& emu)
    {
        auto backend = sogen::create_screenshot_ui_backend();
        auto* raw = static_cast<sogen::screenshot_ui_backend*>(backend.get());
        emu.set_ui_backend(std::move(backend));
        return *raw;
    }

    struct routine_harness
    {
        std::unique_ptr<sogen::macos_emulator> emu;
        sogen::macos_native_dispatch dispatch{};
        std::map<std::string, uint64_t> entries{};

        // Binds every registered routine at a synthetic address, so the handlers can be driven without a
        // shared cache present. Each gets its own word in one page.
        void prepare()
        {
            this->emu->memory.allocate_memory(stub_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::all);
            this->emu->memory.allocate_memory(scratch, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

            sogen::register_skylight_first_pixel_routines(this->dispatch);

            uint64_t cursor = stub_base;
            for (const auto& entry : sogen::macos_gui_first_pixel_exports())
            {
                this->entries[std::string{entry.symbol}] = cursor;
                cursor += 4;
            }

            this->emu->set_native_dispatch(&this->dispatch);
        }

        uint64_t call(const std::string& symbol, const std::vector<uint64_t>& args, const std::vector<float>& floats = {})
        {
            const auto found = this->entries.find(symbol);
            EXPECT_NE(found, this->entries.end()) << symbol << " is not in the export table";
            if (found == this->entries.end())
            {
                return 0;
            }

            // register_routine holds handlers by image+symbol; the harness reaches them by binding the
            // same handler at a synthetic entry, which is what bind() would do with a real cache.
            this->dispatch.bind_entry(found->second, symbol, this->handler_for(symbol));

            for (size_t i = 0; i < args.size(); ++i)
            {
                this->emu->emu().reg(static_cast<sogen::arm64_register>(static_cast<uint32_t>(sogen::arm64_register::x0) + i), args[i]);
            }

            for (size_t i = 0; i < floats.size(); ++i)
            {
                uint32_t raw = 0;
                std::memcpy(&raw, &floats[i], sizeof(raw));
                this->emu->emu().reg(static_cast<sogen::arm64_register>(static_cast<uint32_t>(sogen::arm64_register::s0) + i), raw);
            }

            this->emu->emu().reg(sogen::arm64_register::pc, found->second + 4);
            this->dispatch.invoke(*this->emu, found->second);

            return this->emu->emu().reg(sogen::arm64_register::x0);
        }

        sogen::macos_native_handler handler_for(const std::string& symbol);
    };

    sogen::macos_native_handler routine_harness::handler_for(const std::string& symbol)
    {
        for (const auto& routine : this->dispatch.routines())
        {
            if (routine.symbol == symbol)
            {
                return routine.handler;
            }
        }

        return nullptr;
    }

    TEST(SkylightRoutines, ClampsHostileCgDimensions)
    {
        EXPECT_EQ(sogen::clamp_cg_dimension(std::nan("")), 0) << "casting NaN to an integer is undefined";
        EXPECT_EQ(sogen::clamp_cg_dimension(std::numeric_limits<double>::infinity()), sogen::MACOS_GUI_MAX_WINDOW_DIMENSION);
        EXPECT_EQ(sogen::clamp_cg_dimension(-std::numeric_limits<double>::infinity()), -sogen::MACOS_GUI_MAX_WINDOW_DIMENSION);
        EXPECT_EQ(sogen::clamp_cg_dimension(1e300), sogen::MACOS_GUI_MAX_WINDOW_DIMENSION);
        EXPECT_EQ(sogen::clamp_cg_dimension(180.75), 180);
        EXPECT_EQ(sogen::clamp_cg_dimension(-3.5), -3);
    }

    TEST(SkylightRoutines, ReadsACgRectOrRefusesOne)
    {
        const auto emu = macos_test::make_emulator();
        ASSERT_TRUE(emu->memory.allocate_memory(scratch, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        const std::array<double, 4> rect{1.0, 2.0, 300.0, 180.0};
        emu->memory.write_memory(scratch, rect.data(), sizeof(rect));

        const auto read = sogen::read_cg_rect(*emu, scratch);
        ASSERT_TRUE(read.has_value());
        EXPECT_DOUBLE_EQ(read->width, 300.0);
        EXPECT_DOUBLE_EQ(read->height, 180.0);

        EXPECT_FALSE(sogen::read_cg_rect(*emu, 0).has_value());
        EXPECT_FALSE(sogen::read_cg_rect(*emu, 0x900000000ULL).has_value());
    }

    TEST(SkylightRoutines, TheConnectAndWindowHandshakeReachesAVisibleWindow)
    {
        routine_harness harness{.emu = macos_test::make_emulator()};
        auto& shot = attach_screenshot(*harness.emu);
        shot.set_desktop_size(640, 480);
        shot.set_background(0, 0, 0);
        harness.prepare();

        // SLSMainConnectionID is no longer intercepted: SkyLight's own bring-up runs and sogen answers
        // it at the MIG layer. A handler test reaches the same connection through the server directly.
        const auto connection = uint64_t{harness.emu->ui.server.main_connection()};

        const std::array<double, 4> bounds{0.0, 0.0, 300.0, 180.0};
        harness.emu->memory.write_memory(scratch, bounds.data(), sizeof(bounds));

        ASSERT_EQ(harness.call("_CGSNewRegionWithRect", {scratch, scratch + 0x100}), 0u);

        uint64_t region = 0;
        harness.emu->memory.read_memory(scratch + 0x100, &region, sizeof(region));
        ASSERT_NE(region, 0u);

        ASSERT_EQ(harness.call("_SLSNewWindow", {connection, 2, region, scratch + 0x200}, {200.0f, 150.0f}), 0u);

        uint32_t window = 0;
        harness.emu->memory.read_memory(scratch + 0x200, &window, sizeof(window));
        ASSERT_NE(window, 0u);

        const auto* record = harness.emu->ui.server.find_window(window);
        ASSERT_NE(record, nullptr);
        EXPECT_EQ(record->x, 200) << "x came from s0, not from an integer argument";
        EXPECT_EQ(record->y, 150);
        EXPECT_EQ(record->width, 300);
        EXPECT_EQ(record->height, 180);
        EXPECT_NE(record->backing_address, 0u);

        EXPECT_EQ(harness.call("_SLSSetWindowLevel", {connection, window, 3}), 0u);
        EXPECT_EQ(harness.emu->ui.server.find_window(window)->level, 3);

        EXPECT_FALSE(harness.emu->ui.server.find_window(window)->opaque);
        EXPECT_EQ(harness.call("_SLSSetWindowOpacity", {connection, window, 1}), 0u);
        EXPECT_TRUE(harness.emu->ui.server.find_window(window)->opaque) << "opacity is recorded, not merely accepted";

        EXPECT_FALSE(shot.find_window(window)->visible);
        EXPECT_EQ(harness.call("_SLSOrderWindow", {connection, window, 1, 0}), 0u);
        EXPECT_TRUE(shot.find_window(window)->visible);

        std::vector<uint8_t> pixels(record->backing_bytes(), 0);
        for (size_t i = 0; i < pixels.size(); i += 4)
        {
            pixels[i] = 0x30;
            pixels[i + 1] = 0x3B;
            pixels[i + 2] = 0xFF;
            pixels[i + 3] = 0xFF;
        }
        harness.emu->memory.write_memory(record->backing_address, pixels.data(), pixels.size());

        EXPECT_EQ(harness.call("_SLSFlushWindowContentRegion", {connection, window, region}), 0u);

        const auto image = shot.compose();
        EXPECT_EQ(image.pixel_at(250, 200), (std::array<uint8_t, 4>{0xFF, 0x3B, 0x30, 0xFF}));
        EXPECT_EQ(image.pixel_at(10, 10), (std::array<uint8_t, 4>{0, 0, 0, 0xFF})) << "outside the window";
    }

    // The bitmap info the window context is created with decides the byte order the guest's own
    // CoreGraphics writes, and the present path reads it as ui_surface_format::bgra8. Measured on 25G76
    // by filling a 1x1 context and reading the bytes back: 0x2002 (PremultipliedFirst | ByteOrder32Little)
    // puts opaque red at `00 00 ff ff` and opaque blue at `ff 00 00 ff`, while 0x2001 (PremultipliedLast)
    // puts them at `ff 00 00 ff` and `ff ff 00 00` -- A, B, G, R, which read as BGRA rotates every
    // channel. A pixel count cannot see a permutation, so the two ends are pinned together here.
    TEST(SkylightRoutines, TheWindowBitmapLayoutSurvivesThePresentPath)
    {
        EXPECT_EQ(sogen::MACOS_CG_BITMAP_INFO_BGRA_PREMULTIPLIED, 0x2002u);

        routine_harness harness{.emu = macos_test::make_emulator()};
        auto& shot = attach_screenshot(*harness.emu);
        shot.set_desktop_size(64, 64);
        shot.set_background(0, 0, 0);
        harness.prepare();

        auto* window = harness.emu->ui.server.create_window(sogen::MACOS_MAIN_CONNECTION_ID, 0, 0, 4, 4);
        ASSERT_NE(window, nullptr);
        window->ordered_in = true;
        ASSERT_TRUE(harness.emu->ui.ensure_backing_store(*harness.emu, *window));

        // paintprobe's first bar, in the order 0x2002 lays it down: blue, green, red, alpha.
        constexpr std::array<uint8_t, 4> bgra{255, 217, 0, 255};
        std::vector<uint8_t> pixels{};
        for (size_t i = 0; i < window->backing_bytes(); i += 4)
        {
            pixels.insert(pixels.end(), bgra.begin(), bgra.end());
        }

        harness.emu->memory.write_memory(window->backing_address, pixels.data(), pixels.size());
        harness.emu->ui.sync_window(*harness.emu, *window);
        ASSERT_TRUE(harness.emu->ui.present(*harness.emu, *window, window->rect()));

        EXPECT_EQ(shot.compose().pixel_at(2, 2), (std::array<uint8_t, 4>{0, 217, 255, 255}))
            << "the bar reaches the screen as the colour paintprobe asked for";
    }

    TEST(SkylightRoutines, RefuseAnUnknownConnectionOrWindow)
    {
        routine_harness harness{.emu = macos_test::make_emulator()};
        attach_screenshot(*harness.emu);
        harness.prepare();

        // A window that really exists, so a wrong connection id is the only thing left to refuse it. An
        // absent window would fail the lookup on its own and prove nothing about the connection check.
        auto* owned = harness.emu->ui.server.create_window(sogen::MACOS_MAIN_CONNECTION_ID, 0, 0, 16, 16);
        ASSERT_NE(owned, nullptr);

        EXPECT_EQ(harness.call("_SLSSetWindowLevel", {0xDEAD, owned->id, 3}), sogen::MACOS_CG_ERROR_ILLEGAL_ARGUMENT)
            << "another connection may not touch this connection's window";
        EXPECT_EQ(harness.emu->ui.server.find_window(owned->id)->level, 0) << "and the refused call changed nothing";

        EXPECT_EQ(harness.call("_SLSOrderWindow", {sogen::MACOS_MAIN_CONNECTION_ID, 999, 1, 0}), sogen::MACOS_CG_ERROR_ILLEGAL_ARGUMENT);
        EXPECT_EQ(harness.call("_SLSNewWindow", {sogen::MACOS_MAIN_CONNECTION_ID, 2, 12345, scratch}, {0.0f, 0.0f}),
                  sogen::MACOS_CG_ERROR_ILLEGAL_ARGUMENT)
            << "an unknown region";

        EXPECT_EQ(harness.emu->last_stop_reason(), sogen::stop_reason::none);
    }

    // The end-to-end check that the interception mechanism, the measured table and the handlers agree:
    // every routine has to resolve in the real cache and get its entry patched. A symbol this release
    // renamed shows up here as an unbound one rather than as a window that silently never appears.
    TEST(SkylightRoutines, BindsEveryRoutineAgainstTheHostCache)
    {
        const std::filesystem::path cache_path{MACOS_DYLD_CACHE_HOST_PATH};
        if (!std::filesystem::exists(cache_path))
        {
            GTEST_SKIP() << "no shared cache on this host";
        }

        const auto emu = macos_test::make_emulator();
        attach_screenshot(*emu);

        // The routines live inside the cache, which is not mapped in a bare emulator, so the pages the
        // patcher writes to are staged here. Only that the mechanism reaches every entry is under test.
        const auto cache = sogen::dyld_shared_cache_reader::open(cache_path);
        const sogen::macos_cache_symbols symbols{cache};

        // The export table is one list; the handlers behind it are registered by group, and every group
        // has to be in this check or an export can be listed with nothing behind it.
        sogen::macos_native_dispatch dispatch{};
        sogen::register_skylight_first_pixel_routines(dispatch);
        sogen::register_process_manager_routines(dispatch);
        sogen::register_io_surface_routines(dispatch);
        EXPECT_EQ(dispatch.registered_count(), sogen::macos_gui_first_pixel_exports().size()) << "every measured export has a handler";

        for (const auto& entry : sogen::macos_gui_first_pixel_exports())
        {
            const auto address = symbols.find_export(entry.image, entry.symbol);
            ASSERT_TRUE(address.has_value()) << entry.symbol;
            ASSERT_TRUE(emu->memory.allocate_memory(*address & ~(sogen::MACOS_PAGE_SIZE - 1), sogen::MACOS_PAGE_SIZE,
                                                    sogen::memory_permission::read_exec) ||
                        emu->memory.get_region_info(*address).has_value());
        }

        EXPECT_EQ(dispatch.bind(*emu, symbols), sogen::macos_gui_first_pixel_exports().size());
        EXPECT_TRUE(dispatch.unbound_symbols().empty()) << "a symbol this release renamed would be listed here";

        for (const auto& entry : sogen::macos_gui_first_pixel_exports())
        {
            const auto address = *symbols.find_export(entry.image, entry.symbol);
            EXPECT_TRUE(dispatch.handles(address)) << entry.symbol << " was not bound";

            uint32_t word = 0;
            ASSERT_TRUE(emu->memory.try_read_memory(address, &word, sizeof(word)));
            EXPECT_EQ(word, sogen::MACOS_ARM64_SVC_80) << entry.symbol << " kept its original instruction";
        }
    }

    // Reported, not merely logged. With no routine bound the guest calls the real SkyLight, which talks
    // to a window server that is not there, and the failure surfaces somewhere else entirely -- inside
    // TCC, in the case that prompted this. Nothing watching the guest can tell the two apart.
    TEST(SkylightRoutines, BindingReportsWhatItIntercepted)
    {
        const std::filesystem::path cache_path{MACOS_DYLD_CACHE_HOST_PATH};
        if (!std::filesystem::exists(cache_path))
        {
            GTEST_SKIP() << "no shared cache on this host";
        }

        const auto emu = macos_test::make_emulator();
        attach_screenshot(*emu);

        size_t reports = 0;
        size_t reported_bound = 0;
        size_t reported_registered = 0;

        emu->callbacks.on_gui_routines_bound = [&](const size_t bound, const size_t registered, size_t) {
            ++reports;
            reported_bound = bound;
            reported_registered = registered;
        };

        emu->ui.enabled = true;
        ASSERT_TRUE(emu->ui.bind(*emu, cache_path));

        EXPECT_EQ(reports, 1u);
        EXPECT_EQ(reported_registered, sogen::macos_gui_first_pixel_exports().size());
        EXPECT_EQ(reported_bound, 0u) << "nothing is patched until the guest has mapped the cache into its own address space";
    }

    // The measured 25G76 window-creation path: SLSNewWindowWithOpaqueShape hands the id back through its
    // eighth argument (x7), not through the return value, and the frame arrives in a transaction.
    TEST(SkylightRoutines, TransactionPathCreatesShapesAndCommitsAWindow)
    {
        routine_harness harness{.emu = macos_test::make_emulator()};
        auto& shot = attach_screenshot(*harness.emu);
        shot.set_desktop_size(640, 480);
        harness.prepare();

        const auto connection = uint64_t{harness.emu->ui.server.main_connection()};
        ASSERT_EQ(connection, sogen::MACOS_MAIN_CONNECTION_ID);

        const std::array<double, 4> shape_bounds{0.0, 0.0, 640.0, 480.0};
        harness.emu->memory.write_memory(scratch, shape_bounds.data(), sizeof(shape_bounds));
        ASSERT_EQ(harness.call("_CGSNewRegionWithRect", {scratch, scratch + 0x100}), 0u);
        uint64_t opaque_shape = 0;
        harness.emu->memory.read_memory(scratch + 0x100, &opaque_shape, sizeof(opaque_shape));
        ASSERT_NE(opaque_shape, 0u);

        EXPECT_EQ(harness.call("_SLSNewWindowWithOpaqueShape",
                               {connection, 5, opaque_shape, opaque_shape, 0x10000, 0, 0x40, scratch + 0x200}, {0.0f, 0.0f}),
                  0u);
        uint32_t window = 0;
        harness.emu->memory.read_memory(scratch + 0x200, &window, sizeof(window));
        ASSERT_NE(window, 0u) << "the id comes from *x7";

        const auto* record = harness.emu->ui.server.find_window(window);
        ASSERT_NE(record, nullptr);
        EXPECT_EQ(record->width, 640);
        EXPECT_TRUE(shot.find_window(window) != nullptr) << "creation mirrors onto the backend";

        const auto transaction = harness.call("_SLSTransactionCreate", {connection});
        ASSERT_NE(transaction, 0u);
        EXPECT_EQ(harness.call("_SLSTransactionCreate", {0xDEAD}), 0u) << "an unknown connection gets a null ref";

        const std::array<double, 4> frame{300.0, 200.0, 320.0, 232.0};
        harness.emu->memory.write_memory(scratch, frame.data(), sizeof(frame));
        ASSERT_EQ(harness.call("_CGSNewRegionWithRect", {scratch, scratch + 0x108}), 0u);
        uint64_t frame_region = 0;
        harness.emu->memory.read_memory(scratch + 0x108, &frame_region, sizeof(frame_region));

        EXPECT_EQ(harness.call("_SLSTransactionSetWindowShape", {transaction, window, frame_region}), 0u);
        EXPECT_EQ(harness.emu->ui.server.find_window(window)->width, 640) << "set is staged until commit";
        EXPECT_EQ(harness.call("_SLSTransactionSetWindowShape", {0xFFFF, window, frame_region}), sogen::MACOS_CG_ERROR_ILLEGAL_ARGUMENT);

        EXPECT_EQ(harness.call("_SLSTransactionCommitUsingMethod", {transaction, 1, 0, 0}), 0u);

        record = harness.emu->ui.server.find_window(window);
        EXPECT_EQ(record->x, 300);
        EXPECT_EQ(record->y, 200);
        EXPECT_EQ(record->width, 320);
        EXPECT_EQ(record->height, 232);

        const auto* mirrored = shot.find_window(window);
        ASSERT_NE(mirrored, nullptr);
        EXPECT_EQ(mirrored->rect.left, 300);
        EXPECT_EQ(mirrored->rect.right, 620) << "commit mirrors the new frame onto the backend";

        EXPECT_EQ(harness.call("_SLSTransactionCommit", {transaction, 0}), 0u) << "the ref survives being committed";
        EXPECT_EQ(harness.call("_SLSTransactionCommitUsingMethod", {transaction, 9, 0, 0}), sogen::MACOS_CG_ERROR_ILLEGAL_ARGUMENT)
            << "the host aborts on an unknown method; the emulator answers with an error";
        EXPECT_EQ(harness.call("_SLSTransactionCommit", {0xFFFF, 0}), sogen::MACOS_CG_ERROR_ILLEGAL_ARGUMENT);
    }

    // Measured on 25G76 (lldb, /tmp/inp2/orderprobe): AppKit never calls SLSOrderWindow.
    // makeKeyAndOrderFront: reaches SLSTransactionOrderWindowGroupFrontConditionally(t, 33752, ...),
    // orderBack: reaches SLSTransactionOrderWindowGroup(t, 33753, -1, 0) and orderOut: the same with 0.
    TEST(SkylightRoutines, TheTransactionOrderingPathIsWhatAppKitActuallyCalls)
    {
        routine_harness harness{.emu = macos_test::make_emulator()};
        auto& shot = attach_screenshot(*harness.emu);
        shot.set_desktop_size(640, 480);
        harness.prepare();

        const auto connection = uint64_t{harness.emu->ui.server.main_connection()};
        auto* owned = harness.emu->ui.server.create_window(connection, 0, 0, 64, 64);
        ASSERT_NE(owned, nullptr);
        const uint64_t window = owned->id;

        const auto transaction = harness.call("_SLSTransactionCreate", {connection});
        ASSERT_NE(transaction, 0u);

        EXPECT_EQ(harness.call("_SLSTransactionOrderWindowGroupFrontConditionally", {transaction, window, 0}), 0u);
        EXPECT_TRUE(harness.emu->ui.server.find_window(window)->ordered_in) << "front-conditionally is always on screen";
        EXPECT_TRUE(shot.find_window(window)->visible) << "and it reaches the backend, which is what composes";

        EXPECT_EQ(harness.call("_SLSTransactionOrderWindowGroup", {transaction, window, 0, 0}), 0u);
        EXPECT_FALSE(harness.emu->ui.server.find_window(window)->ordered_in) << "order 0 is orderOut:";

        EXPECT_EQ(harness.call("_SLSTransactionOrderWindowGroup", {transaction, window, static_cast<uint64_t>(-1), 0}), 0u);
        EXPECT_TRUE(harness.emu->ui.server.find_window(window)->ordered_in) << "order -1 is orderBack:, still on screen";

        EXPECT_EQ(harness.call("_SLSTransactionOrderWindowGroup", {0xFFFF, window, 1, 0}), sogen::MACOS_CG_ERROR_ILLEGAL_ARGUMENT)
            << "an unknown transaction";
        EXPECT_EQ(harness.call("_SLSTransactionOrderWindowGroupFrontConditionally", {transaction, 999, 0}),
                  sogen::MACOS_CG_ERROR_ILLEGAL_ARGUMENT)
            << "an unknown window";
    }

    TEST(SkylightRoutines, NewWindowWithOpaqueShapeToleratesTheSingletonShapeRegion)
    {
        routine_harness harness{.emu = macos_test::make_emulator()};
        attach_screenshot(*harness.emu);
        harness.prepare();

        // SkyLight's full-screen opaque-shape singleton is built client-side, not through
        // CGSNewRegionWithRect, so it is not a known region: the window must still be created (its real
        // frame arrives via the shape transaction), starting at 1x1 rather than failing creation.
        EXPECT_EQ(harness.call("_SLSNewWindowWithOpaqueShape",
                               {sogen::MACOS_MAIN_CONNECTION_ID, 5, 0xABC, 0xABC, 0x10000, 0, 0x40, scratch + 0x200}, {50.0f, 60.0f}),
                  0u);

        uint32_t window = 0;
        harness.emu->memory.read_memory(scratch + 0x200, &window, sizeof(window));
        ASSERT_NE(window, 0u);

        const auto* record = harness.emu->ui.server.find_window(window);
        ASSERT_NE(record, nullptr);
        EXPECT_EQ(record->x, 50);
        EXPECT_EQ(record->y, 60);
        EXPECT_EQ(record->width, 1);
        EXPECT_EQ(record->height, 1);
    }

    TEST(SkylightRoutines, LayerContextTitleAndEventMaskAreRecordedOnTheWindow)
    {
        routine_harness harness{.emu = macos_test::make_emulator()};
        auto& shot = attach_screenshot(*harness.emu);
        shot.set_desktop_size(640, 480);
        harness.prepare();

        const auto connection = uint64_t{harness.emu->ui.server.main_connection()};
        auto* owned = harness.emu->ui.server.create_window(connection, 0, 0, 64, 64);
        ASSERT_NE(owned, nullptr);
        const uint64_t window = owned->id;

        EXPECT_EQ(harness.call("_SLSSetWindowLayerContext", {connection, window, 0xCAFE}), 0u);
        EXPECT_EQ(harness.emu->ui.server.find_window(window)->layer_context, 0xCAFEu);
        EXPECT_EQ(harness.call("_SLSSetWindowLayerContext", {connection, 999, 0xCAFE}), sogen::MACOS_CG_ERROR_ILLEGAL_ARGUMENT);

        // A CFConstantString, as measured: {isa, info = 0x7c8, const char* utf8, byte length}.
        const char title[] = "probe";
        harness.emu->memory.write_memory(scratch + 0x400, title, sizeof(title) - 1);
        const std::array<uint64_t, 4> fields{0x1000, 0x7c8, scratch + 0x400, sizeof(title) - 1};
        harness.emu->memory.write_memory(scratch + 0x300, fields.data(), sizeof(fields));

        EXPECT_EQ(harness.call("_SLSSetWindowTitle", {connection, window, scratch + 0x300}), 0u);
        EXPECT_EQ(harness.emu->ui.server.find_window(window)->title, u"probe");
        EXPECT_EQ(shot.find_window(window)->title, u"probe") << "the backend mirror sees the title";

        // Any other CFString representation reports by name and leaves the title alone.
        const std::array<uint64_t, 4> heap_form{0x1000, 0x90, scratch + 0x400, sizeof(title) - 1};
        harness.emu->memory.write_memory(scratch + 0x380, heap_form.data(), sizeof(heap_form));
        EXPECT_EQ(harness.call("_SLSSetWindowTitle", {connection, window, scratch + 0x380}), 0u);
        EXPECT_EQ(harness.emu->ui.server.find_window(window)->title, u"probe") << "an unhandled form must not smash the title";

        EXPECT_EQ(harness.call("_SLSSetEventMask", {connection, 0x1F4EE5CFFDEULL, window}), 0u);
        EXPECT_EQ(harness.emu->ui.server.find_window(window)->event_mask, 0x1F4EE5CFFDEULL);

        EXPECT_EQ(harness.call("_SLSSetWindowClientPerceivedType", {connection, window, 5}), 0u);
        EXPECT_EQ(harness.emu->ui.server.find_window(window)->perceived_type, 5u);

        EXPECT_EQ(harness.call("_SLSWindowIsOrderedIn", {connection, window, scratch + 0x500}), 0u);
        uint8_t ordered = 0xAA;
        harness.emu->memory.read_memory(scratch + 0x500, &ordered, sizeof(ordered));
        EXPECT_EQ(ordered, 0u);

        EXPECT_EQ(harness.call("_SLSOrderWindow", {connection, window, 1, 0}), 0u);
        EXPECT_EQ(harness.call("_SLSWindowIsOrderedIn", {connection, window, scratch + 0x500}), 0u);
        harness.emu->memory.read_memory(scratch + 0x500, &ordered, sizeof(ordered));
        EXPECT_EQ(ordered, 1u);
    }

    TEST(SkylightRoutines, ServerAndRenderPortsAreRealStableAndAllocationFreeWhenHot)
    {
        routine_harness harness{.emu = macos_test::make_emulator()};
        attach_screenshot(*harness.emu);
        harness.prepare();

        const auto server_port = harness.call("_SLSServerPort", {});
        ASSERT_NE(server_port, 0u);

        const auto ports_before = harness.emu->mach.ports.live_port_count();
        EXPECT_EQ(harness.call("_SLSServerPort", {}), server_port) << "same port every call";
        EXPECT_EQ(harness.emu->mach.ports.live_port_count(), ports_before) << "the hot path allocates nothing";

        const auto* entry = harness.emu->mach.ports.find(static_cast<sogen::mach::port_name_t>(server_port));
        ASSERT_NE(entry, nullptr);
        EXPECT_EQ(entry->object.kind, sogen::mach::kernel_object_kind::window_server);

        const auto render_port = harness.call("_CARenderServerGetServerPort", {});
        ASSERT_NE(render_port, 0u);
        EXPECT_NE(render_port, server_port);
        EXPECT_EQ(harness.call("_CARenderServerGetServerPort", {}), render_port);
        EXPECT_EQ(harness.emu->mach.ports.find(static_cast<sogen::mach::port_name_t>(render_port))->object.kind,
                  sogen::mach::kernel_object_kind::render_server);

        EXPECT_EQ(harness.call("_CARenderServerGetNeededAlignment", {render_port}), 0x10u);
        EXPECT_EQ(harness.call("_CARenderServerGetMaxRenderableIOSurfaceSize", {render_port, scratch, scratch + 0x10, scratch + 0x20}), 1u);
        uint32_t max_size = 0;
        harness.emu->memory.read_memory(scratch + 0x20, &max_size, sizeof(max_size));
        EXPECT_EQ(max_size, 0x8000u);

        EXPECT_EQ(harness.emu->last_stop_reason(), sogen::stop_reason::none);
    }

    TEST(SkylightRoutines, RegistrationHandshakeIsRecorded)
    {
        routine_harness harness{.emu = macos_test::make_emulator()};
        attach_screenshot(*harness.emu);
        harness.prepare();

        EXPECT_FALSE(harness.emu->ui.server.process_registered);
        EXPECT_EQ(harness.call("_SLPSRegisterWithServer", {0}), 0u);
        EXPECT_TRUE(harness.emu->ui.server.process_registered);

        EXPECT_EQ(harness.call("_SLPSSetMainApplicationConnection", {sogen::MACOS_MAIN_CONNECTION_ID}), 0u);
        EXPECT_EQ(harness.emu->ui.server.main_application_connection, sogen::MACOS_MAIN_CONNECTION_ID);

        EXPECT_EQ(harness.call("_SLSSetFrontProcessWithInfo", {scratch, 0, 1}), 0u);
        EXPECT_TRUE(harness.emu->ui.server.front_process_set);

        EXPECT_EQ(harness.call("_SLSGetAppearanceThemeLegacy", {1}), 0u);
        EXPECT_EQ(harness.call("_SLSGetLastUsedKeyboardID", {}), 0x5cu);
        EXPECT_EQ(harness.call("_SLSCopyDisplayColorSpace", {1, scratch}), 0u);

        EXPECT_EQ(
            harness.call("_SLSGetDockRectWithOrientation", {sogen::MACOS_MAIN_CONNECTION_ID, scratch, scratch + 0x40, scratch + 0x80}), 0u);
        std::array<double, 4> dock_rect{1.0, 1.0, 1.0, 1.0};
        harness.emu->memory.write_memory(scratch + 0x40, dock_rect.data(), sizeof(dock_rect));
        EXPECT_EQ(
            harness.call("_SLSGetDockRectWithOrientation", {sogen::MACOS_MAIN_CONNECTION_ID, scratch, scratch + 0x40, scratch + 0x80}), 0u);
        harness.emu->memory.read_memory(scratch + 0x40, dock_rect.data(), sizeof(dock_rect));
        EXPECT_DOUBLE_EQ(dock_rect[2], 0.0) << "no dock lives on this desktop";
    }
}
