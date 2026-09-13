#include <gtest/gtest.h>

#include "macos_test_utils.hpp"

#include <gui/macos_ui_state.hpp>
#include <screenshot_ui_backend.hpp>

#include <array>
#include <vector>

namespace
{
    sogen::screenshot_ui_backend& attach_screenshot(sogen::macos_emulator& emu)
    {
        auto backend = sogen::create_screenshot_ui_backend();
        auto* raw = static_cast<sogen::screenshot_ui_backend*>(backend.get());
        emu.set_ui_backend(std::move(backend));
        return *raw;
    }

    TEST(WindowServer, CreatesWindowsAndMirrorsThemOntoTheBackend)
    {
        const auto emu = macos_test::make_emulator();
        auto& shot = attach_screenshot(*emu);
        shot.set_desktop_size(200, 200);

        auto* window = emu->ui.server.create_window(emu->ui.server.main_connection(), 20, 30, 64, 48);
        ASSERT_NE(window, nullptr);
        EXPECT_EQ(window->connection, sogen::MACOS_MAIN_CONNECTION_ID);
        EXPECT_NE(window->id, 0u);

        emu->ui.sync_window(*emu, *window);

        const auto* mirrored = shot.find_window(window->id);
        ASSERT_NE(mirrored, nullptr);
        EXPECT_EQ(mirrored->rect.left, 20);
        EXPECT_EQ(mirrored->rect.top, 30);
        EXPECT_EQ(mirrored->rect.right, 84);
        EXPECT_EQ(mirrored->rect.bottom, 78);
        EXPECT_FALSE(mirrored->visible) << "a window is not on screen until it is ordered in";

        window->ordered_in = true;
        emu->ui.sync_window(*emu, *window);
        EXPECT_TRUE(shot.find_window(window->id)->visible);
        EXPECT_EQ(shot.windows().size(), 1u) << "syncing twice updates rather than duplicates";

        const auto id = window->id;
        EXPECT_TRUE(emu->ui.server.destroy_window(id));
        emu->ui.forget_window(*emu, id);
        EXPECT_EQ(shot.find_window(id), nullptr);
    }

    TEST(WindowServer, AllocatesABackingStoreInsideTheGuiArena)
    {
        const auto emu = macos_test::make_emulator();
        attach_screenshot(*emu);

        auto* window = emu->ui.server.create_window(emu->ui.server.main_connection(), 0, 0, 32, 16);
        ASSERT_NE(window, nullptr);
        ASSERT_TRUE(emu->ui.ensure_backing_store(*emu, *window));

        EXPECT_GE(window->backing_address, sogen::MACOS_GUI_ARENA_BASE);
        EXPECT_LT(window->backing_address, sogen::MACOS_GUI_ARENA_BASE + sogen::MACOS_GUI_ARENA_SIZE);
        EXPECT_EQ(window->backing_stride, 32u * 4u);
        EXPECT_EQ(window->backing_bytes(), 32u * 4u * 16u);

        const auto info = emu->memory.get_region_info(window->backing_address);
        ASSERT_TRUE(info.has_value());
        EXPECT_TRUE((info->permissions & sogen::memory_permission::write) == sogen::memory_permission::write);

        const auto address = window->backing_address;
        emu->ui.release_backing_store(*emu, *window);
        EXPECT_EQ(window->backing_address, 0u);
        EXPECT_TRUE(emu->memory.get_region_info(address).has_value()) << "a released block keeps its mapping";
    }

    // sogen hands the guest's own CoreGraphics a raw pointer into the arena -- SLWindowContextCreate
    // builds a CGBitmapContext straight over a backing store -- and is never told when the guest lets go
    // of it. Unmapping the block would put whatever the guest draws next into whichever allocation took
    // the address, which is the libmalloc fault in
    TEST(GuiArena, AReleasedBlockKeepsItsMappingAndServesTheNextRequest)
    {
        const auto emu = macos_test::make_emulator();
        attach_screenshot(*emu);

        auto* first = emu->ui.server.create_window(emu->ui.server.main_connection(), 0, 0, 32, 16);
        ASSERT_NE(first, nullptr);
        ASSERT_TRUE(emu->ui.ensure_backing_store(*emu, *first));

        const auto address = first->backing_address;
        emu->ui.release_backing_store(*emu, *first);

        const auto info = emu->memory.get_region_info(address);
        ASSERT_TRUE(info.has_value());
        EXPECT_EQ(info->allocation_base, address);

        auto* second = emu->ui.server.create_window(emu->ui.server.main_connection(), 0, 0, 32, 16);
        ASSERT_NE(second, nullptr);
        ASSERT_TRUE(emu->ui.ensure_backing_store(*emu, *second));

        EXPECT_EQ(second->backing_address, address) << "the block is reused rather than remapped";
        EXPECT_EQ(emu->ui.arena.block_count(), 1u);
        EXPECT_EQ(emu->ui.arena.retired_count(), 0u);
    }

    // A raster is drawn by handing its block to CGBitmapContextCreate and compositing an image over it
    // source-over, so a recycled block still holding the previous picture shows that picture through
    // every transparent pixel of the next one.
    TEST(GuiArena, ARecycledBlockComesBackZeroed)
    {
        const auto emu = macos_test::make_emulator();
        attach_screenshot(*emu);

        const auto address = emu->ui.arena.acquire(*emu, 64);
        ASSERT_NE(address, 0u);

        const std::array<uint8_t, 8> written{1, 2, 3, 4, 5, 6, 7, 8};
        ASSERT_TRUE(emu->memory.try_write_memory(address, written.data(), written.size()));

        emu->ui.arena.recycle(address);
        EXPECT_EQ(emu->ui.arena.acquire(*emu, 64), address) << "the block is reused rather than remapped";

        std::array<uint8_t, 8> read{};
        ASSERT_TRUE(emu->memory.try_read_memory(address, read.data(), read.size()));
        EXPECT_EQ(read, (std::array<uint8_t, 8>{})) << "a fresh mapping arrives zeroed and a recycled block has to match it";
    }

    TEST(GuiArena, AStoreTheGuestHoldsABitmapContextOverIsNeverHandedOutAgain)
    {
        const auto emu = macos_test::make_emulator();
        attach_screenshot(*emu);

        auto* first = emu->ui.server.create_window(emu->ui.server.main_connection(), 0, 0, 32, 16);
        ASSERT_NE(first, nullptr);
        ASSERT_TRUE(emu->ui.ensure_backing_store(*emu, *first));

        const auto address = first->backing_address;
        first->context = 0xC0FFEE;
        emu->ui.release_backing_store(*emu, *first);
        EXPECT_EQ(first->context, 0u) << "the context no longer describes this window's pixels";
        EXPECT_EQ(emu->ui.arena.retired_count(), 1u);

        auto* second = emu->ui.server.create_window(emu->ui.server.main_connection(), 0, 0, 32, 16);
        ASSERT_NE(second, nullptr);
        ASSERT_TRUE(emu->ui.ensure_backing_store(*emu, *second));

        EXPECT_NE(second->backing_address, address);
        EXPECT_TRUE(emu->memory.get_region_info(address).has_value()) << "the retired block stays mapped for the stale context";
        EXPECT_EQ(emu->ui.arena.block_count(), 2u);
    }

    // Calculator opens its window at 460x52 and settles at 230x408 without the record changing hands.
    // Every reader of the store -- the compositor's write-back and present's read -- sizes itself from
    // backing_stride * height, so a store kept from the first size is addressed far past its own end.
    TEST(GuiArena, AResizedWindowGetsAStoreThatCoversItsNewExtent)
    {
        const auto emu = macos_test::make_emulator();
        attach_screenshot(*emu);

        auto* window = emu->ui.server.create_window(emu->ui.server.main_connection(), 0, 0, 460, 52);
        ASSERT_NE(window, nullptr);
        ASSERT_TRUE(emu->ui.ensure_backing_store(*emu, *window));

        const auto first = window->backing_address;
        EXPECT_EQ(window->backing_stride, 460u * 4u);

        window->width = 230;
        window->height = 408;
        ASSERT_TRUE(emu->ui.ensure_backing_store(*emu, *window));

        EXPECT_EQ(window->backing_stride, 230u * 4u);
        EXPECT_NE(window->backing_address, first) << "460x52 cannot hold 230x408";
        EXPECT_GE(emu->ui.arena.capacity(window->backing_address), window->backing_bytes());

        std::vector<uint8_t> pixels(window->backing_bytes(), 0x7F);
        EXPECT_TRUE(emu->memory.try_write_memory(window->backing_address, pixels.data(), pixels.size()))
            << "the whole extent every reader addresses is mapped";
        EXPECT_TRUE(emu->ui.present(*emu, *window, window->rect()));
    }

    TEST(GuiArena, PresentIsBoundedByTheStoreRatherThanTheWholeArena)
    {
        const auto emu = macos_test::make_emulator();
        attach_screenshot(*emu);

        auto* opened = emu->ui.server.create_window(emu->ui.server.main_connection(), 0, 0, 16, 16);
        ASSERT_NE(opened, nullptr);
        opened->ordered_in = true;
        ASSERT_TRUE(emu->ui.ensure_backing_store(*emu, *opened));

        const auto id = opened->id;
        const auto store = opened->backing_address;
        const auto store_size = emu->ui.arena.capacity(store);

        auto* neighbour = emu->ui.server.create_window(emu->ui.server.main_connection(), 0, 0, 16, 512);
        ASSERT_NE(neighbour, nullptr);
        ASSERT_TRUE(emu->ui.ensure_backing_store(*emu, *neighbour));
        ASSERT_EQ(neighbour->backing_address, store + store_size)
            << "the neighbour has to be mapped, or an out-of-bounds read would fail on its own";

        auto* window = emu->ui.server.find_window(id);
        ASSERT_NE(window, nullptr);
        EXPECT_TRUE(emu->ui.present(*emu, *window, window->rect()));

        // The record is the guest's to write and the store does not follow it on its own. An extent
        // taken from the new height reaches into the next window's pixels.
        window->height = 512;
        EXPECT_FALSE(emu->ui.present(*emu, *window, window->rect()));
        EXPECT_EQ(emu->ui.present_count(), 1u);
    }

    TEST(GuiArena, ExcludingTheGuestMovesItsAllocationsAboveEveryEmulatorArena)
    {
        const auto emu = macos_test::make_emulator();

        const auto overlapping = emu->memory.allocate_memory(sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);
        ASSERT_NE(overlapping, 0u);
        EXPECT_GE(overlapping, sogen::MACOS_GUI_ARENA_BASE) << "the guest's mmap arena is the GUI arena until it is separated";
        EXPECT_LT(overlapping, sogen::MACOS_GUI_ARENA_BASE + sogen::MACOS_GUI_ARENA_SIZE);

        sogen::macos_gui_arena::exclude_guest(*emu);

        const auto separated = emu->memory.allocate_memory(sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);
        ASSERT_NE(separated, 0u);
        EXPECT_GE(separated, sogen::MACOS_GUI_ARENA_BASE + sogen::MACOS_GUI_ARENA_SIZE);
        EXPECT_GE(separated, sogen::MACOS_WORKQUEUE_ARENA_BASE + sogen::MACOS_WORKQUEUE_ARENA_SIZE)
            << "workqueue slots are claimed at fixed addresses, so the guest has to start above them too";

        const auto emulator_owned =
            emu->memory.allocate_memory(sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write, sogen::MACOS_GUI_ARENA_BASE);
        EXPECT_GE(emulator_owned, sogen::MACOS_GUI_ARENA_BASE) << "the emulator still allocates from its own arena";
        EXPECT_LT(emulator_owned, sogen::MACOS_GUI_ARENA_BASE + sogen::MACOS_GUI_ARENA_SIZE);
    }

    // The arena is a fixed range, and allocate_memory treats its base as a hint rather than a bound, so
    // a request that does not fit can be satisfied *above* the arena -- where present would then refuse
    // to read from it, leaving a window that draws nothing and no explanation. MAX_WINDOW_DIMENSION
    // squared times four bytes is exactly the arena size, so two half-height maxima fill it.
    TEST(WindowServer, RefusesABackingStoreThatWouldNotFitTheArena)
    {
        const auto emu = macos_test::make_emulator();
        attach_screenshot(*emu);

        constexpr int32_t half = sogen::MACOS_GUI_MAX_WINDOW_DIMENSION / 2;

        for (int i = 0; i < 2; ++i)
        {
            auto* filler =
                emu->ui.server.create_window(emu->ui.server.main_connection(), 0, 0, sogen::MACOS_GUI_MAX_WINDOW_DIMENSION, half);
            ASSERT_NE(filler, nullptr);
            ASSERT_TRUE(emu->ui.ensure_backing_store(*emu, *filler)) << "half the arena, twice";
        }

        auto* overflow = emu->ui.server.create_window(emu->ui.server.main_connection(), 0, 0, 64, 64);
        ASSERT_NE(overflow, nullptr);
        EXPECT_FALSE(emu->ui.ensure_backing_store(*emu, *overflow)) << "the arena is full, so there is nowhere legal to put it";
        EXPECT_EQ(overflow->backing_address, 0u);
        EXPECT_EQ(emu->last_stop_reason(), sogen::stop_reason::none);
    }

    TEST(WindowServer, PresentsGuestPixelsThroughTheBackend)
    {
        const auto emu = macos_test::make_emulator();
        auto& shot = attach_screenshot(*emu);
        shot.set_desktop_size(64, 64);
        shot.set_background(0, 0, 0);

        auto* window = emu->ui.server.create_window(emu->ui.server.main_connection(), 8, 8, 16, 16);
        ASSERT_NE(window, nullptr);
        window->ordered_in = true;
        ASSERT_TRUE(emu->ui.ensure_backing_store(*emu, *window));
        emu->ui.sync_window(*emu, *window);

        std::vector<uint8_t> pixels(window->backing_bytes(), 0);
        for (size_t i = 0; i < pixels.size(); i += 4)
        {
            pixels[i] = 0x2F;     // B
            pixels[i + 1] = 0x3B; // G
            pixels[i + 2] = 0xFF; // R
            pixels[i + 3] = 0xFF;
        }
        emu->memory.write_memory(window->backing_address, pixels.data(), pixels.size());

        ASSERT_TRUE(emu->ui.present(*emu, *window, window->rect()));
        EXPECT_EQ(emu->ui.present_count(), 1u);

        const auto image = shot.compose();
        const auto pixel = image.pixel_at(12, 12);
        EXPECT_EQ(pixel[0], 0xFFu) << "the guest wrote BGRA and the composite is RGBA";
        EXPECT_EQ(pixel[1], 0x3Bu);
        EXPECT_EQ(pixel[2], 0x2Fu);
    }

    TEST(WindowServer, RefusesHostileWindowGeometry)
    {
        const auto emu = macos_test::make_emulator();
        attach_screenshot(*emu);

        EXPECT_EQ(emu->ui.server.create_window(emu->ui.server.main_connection(), 0, 0, 0, 16), nullptr);
        EXPECT_EQ(emu->ui.server.create_window(emu->ui.server.main_connection(), 0, 0, 16, -1), nullptr);
        EXPECT_EQ(emu->ui.server.create_window(emu->ui.server.main_connection(), 0, 0, 1 << 20, 1 << 20), nullptr);
        EXPECT_EQ(emu->ui.server.create_window(0xDEADBEEF, 0, 0, 16, 16), nullptr) << "an unknown connection owns nothing";

        EXPECT_TRUE(emu->ui.server.windows().empty());
        EXPECT_EQ(emu->last_stop_reason(), sogen::stop_reason::none);
    }

    // The backing address lives in a record the guest's own handlers write into, so a present that read
    // from wherever it pointed would be a way to copy arbitrary guest memory into a screenshot.
    TEST(WindowServer, PresentIsBoundedByTheArenaAndTheSurfaceExtent)
    {
        const auto emu = macos_test::make_emulator();
        attach_screenshot(*emu);

        auto* window = emu->ui.server.create_window(emu->ui.server.main_connection(), 0, 0, 16, 16);
        ASSERT_NE(window, nullptr);
        window->ordered_in = true;
        ASSERT_TRUE(emu->ui.ensure_backing_store(*emu, *window));

        const auto good_address = window->backing_address;

        // Mapped and readable, and outside the arena. An unmapped address would prove nothing here: the
        // read would fail on its own and the bounds check would never be what refused it.
        constexpr uint64_t outside = 0x260000000ULL;
        ASSERT_TRUE(emu->memory.allocate_memory(outside, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        const std::vector<uint8_t> secret(sogen::MACOS_PAGE_SIZE, 0x5A);
        emu->memory.write_memory(outside, secret.data(), secret.size());

        window->backing_address = outside;
        EXPECT_FALSE(emu->ui.present(*emu, *window, window->rect())) << "readable guest memory outside the arena is still refused";

        window->backing_address = sogen::MACOS_GUI_ARENA_BASE + sogen::MACOS_GUI_ARENA_SIZE - 16;
        EXPECT_FALSE(emu->ui.present(*emu, *window, window->rect())) << "running off the end of the arena";

        window->backing_address = good_address;
        window->backing_stride = 4;
        EXPECT_FALSE(emu->ui.present(*emu, *window, window->rect())) << "a stride narrower than one row";

        window->backing_stride = 16u * 4u;
        window->height = sogen::MACOS_GUI_MAX_WINDOW_DIMENSION + 1;
        EXPECT_FALSE(emu->ui.present(*emu, *window, window->rect())) << "an extent past the dimension limit";

        EXPECT_EQ(emu->ui.present_count(), 0u);
        EXPECT_EQ(emu->last_stop_reason(), sogen::stop_reason::none);
    }

    TEST(WindowServer, HitTestingFollowsTheOrderTheCompositorPaintsIn)
    {
        const auto emu = macos_test::make_emulator();
        auto& server = emu->ui.server;

        const auto behind = server.create_window(server.main_connection(), 0, 0, 100, 100)->id;
        const auto front = server.create_window(server.main_connection(), 50, 50, 100, 100)->id;

        EXPECT_EQ(server.window_at(60, 60), nullptr) << "neither window is on screen yet";

        server.find_window(behind)->ordered_in = true;
        ASSERT_NE(server.window_at(60, 60), nullptr);
        EXPECT_EQ(server.window_at(60, 60)->id, behind);

        server.find_window(front)->ordered_in = true;
        EXPECT_EQ(server.window_at(60, 60)->id, front) << "creation order is the z-order";
        EXPECT_EQ(server.window_at(10, 10)->id, behind);
        EXPECT_EQ(server.window_at(140, 140)->id, front);

        EXPECT_EQ(server.window_at(0, 0)->id, behind) << "the top-left corner is inside";
        EXPECT_EQ(server.window_at(100, 100)->id, front) << "the bottom-right corner is not";
        EXPECT_EQ(server.window_at(150, 150), nullptr) << "one past the right edge";
        EXPECT_EQ(server.window_at(-1, 60), nullptr);
    }

    TEST(WindowServer, TheKeyConnectionIsTheFrontmostVisibleWindowsOwner)
    {
        const auto emu = macos_test::make_emulator();
        auto& server = emu->ui.server;

        EXPECT_EQ(server.key_connection(), 0u) << "nothing is on screen";

        const auto first = server.create_window(server.main_connection(), 0, 0, 10, 10)->id;
        EXPECT_EQ(server.key_connection(), 0u) << "a window that is not ordered in owns no keystroke";

        server.find_window(first)->ordered_in = true;
        EXPECT_EQ(server.key_connection(), sogen::MACOS_MAIN_CONNECTION_ID);

        // The first create_connection() hands out MACOS_MAIN_CONNECTION_ID itself, which the window
        // above already uses; a second one is needed for the two windows to have different owners.
        EXPECT_EQ(server.create_connection(), sogen::MACOS_MAIN_CONNECTION_ID);
        const auto second = server.create_connection();
        ASSERT_NE(second, sogen::MACOS_MAIN_CONNECTION_ID);

        auto* later = server.create_window(second, 0, 0, 10, 10);
        ASSERT_NE(later, nullptr);
        later->ordered_in = true;
        EXPECT_EQ(server.key_connection(), second) << "the window in front owns the keyboard";
    }

    TEST(WindowServer, RegionsRoundTrip)
    {
        const auto emu = macos_test::make_emulator();

        const auto region = emu->ui.server.create_region(1, 2, 300, 180);
        ASSERT_NE(region, 0u);

        const auto* found = emu->ui.server.find_region(region);
        ASSERT_NE(found, nullptr);
        EXPECT_EQ(found->x, 1);
        EXPECT_EQ(found->width, 300);
        EXPECT_EQ(found->height, 180);

        EXPECT_EQ(emu->ui.server.find_region(region + 1), nullptr);
        EXPECT_EQ(emu->ui.server.create_region(0, 0, -1, 10), 0u);
    }

    TEST(WindowServer, SerializesAcrossASnapshot)
    {
        const auto emu = macos_test::make_emulator();

        auto* window = emu->ui.server.create_window(emu->ui.server.main_connection(), 5, 6, 40, 20);
        ASSERT_NE(window, nullptr);
        window->title = u"Probe";
        window->ordered_in = true;

        sogen::utils::buffer_serializer serializer{};
        emu->ui.server.serialize(serializer);

        sogen::macos_window_server restored{};
        sogen::utils::buffer_deserializer deserializer{serializer.get_buffer()};
        restored.deserialize(deserializer);

        const auto* copy = restored.find_window(window->id);
        ASSERT_NE(copy, nullptr);
        EXPECT_EQ(copy->x, 5);
        EXPECT_EQ(copy->width, 40);
        EXPECT_TRUE(copy->ordered_in);
        EXPECT_EQ(copy->title, u"Probe");
    }

    TEST(WindowServer, TransactionsApplyShapesAtCommitAndSurviveIt)
    {
        const auto emu = macos_test::make_emulator();

        auto* window = emu->ui.server.create_window(emu->ui.server.main_connection(), 0, 0, 16, 16);
        ASSERT_NE(window, nullptr);

        const auto transaction = emu->ui.server.create_transaction(emu->ui.server.main_connection());
        ASSERT_NE(transaction, 0u);
        EXPECT_EQ(emu->ui.server.create_transaction(0xDEAD), 0u) << "an unknown connection owns no transactions";

        auto* found = emu->ui.server.find_transaction(transaction);
        ASSERT_NE(found, nullptr);
        found->shapes.push_back(sogen::macos_pending_shape{.window_id = window->id, .x = 40, .y = 50, .width = 300, .height = 200});

        EXPECT_EQ(window->width, 16) << "set is staged, not applied";
        ASSERT_TRUE(emu->ui.server.commit_transaction(transaction));
        EXPECT_EQ(window->x, 40);
        EXPECT_EQ(window->width, 300);
        EXPECT_TRUE(found->shapes.empty());

        EXPECT_TRUE(emu->ui.server.commit_transaction(transaction)) << "the ref survives being committed -- the host re-commits one ref";
        EXPECT_FALSE(emu->ui.server.commit_transaction(0xFFFF)) << "an unknown transaction";
    }

    TEST(WindowServer, SerializesTransactionsPortsAndRegistrationAcrossASnapshot)
    {
        const auto emu = macos_test::make_emulator();

        auto* window = emu->ui.server.create_window(emu->ui.server.main_connection(), 0, 0, 16, 16);
        ASSERT_NE(window, nullptr);
        window->event_mask = 0x1234;
        window->perceived_type = 5;

        const auto transaction = emu->ui.server.create_transaction(emu->ui.server.main_connection());
        ASSERT_NE(transaction, 0u);
        emu->ui.server.find_transaction(transaction)
            ->shapes.push_back(sogen::macos_pending_shape{.window_id = window->id, .x = 1, .y = 2, .width = 3, .height = 4});

        emu->ui.server.server_port = 0x1107;
        emu->ui.server.render_server_port = 0x1307;
        emu->ui.server.event_port = 0x1507;
        emu->ui.server.process_registered = true;
        emu->ui.server.main_application_connection = 0x42;
        emu->ui.server.front_process_set = true;

        sogen::utils::buffer_serializer serializer{};
        emu->ui.server.serialize(serializer);

        sogen::macos_window_server restored{};
        sogen::utils::buffer_deserializer deserializer{serializer.get_buffer()};
        restored.deserialize(deserializer);

        const auto* copy = restored.find_transaction(transaction);
        ASSERT_NE(copy, nullptr);
        ASSERT_EQ(copy->shapes.size(), 1u);
        EXPECT_EQ(copy->shapes[0].width, 3);
        EXPECT_EQ(restored.find_window(window->id)->event_mask, 0x1234u);
        EXPECT_EQ(restored.server_port, 0x1107u);
        EXPECT_EQ(restored.render_server_port, 0x1307u);
        EXPECT_EQ(restored.event_port, 0x1507u);
        EXPECT_TRUE(restored.process_registered);
        EXPECT_EQ(restored.main_application_connection, 0x42u);
        EXPECT_TRUE(restored.front_process_set);

        // The id counters must survive too: a fresh transaction may not reuse the snapshotted id.
        EXPECT_NE(restored.create_transaction(sogen::MACOS_MAIN_CONNECTION_ID), transaction);
    }
}
