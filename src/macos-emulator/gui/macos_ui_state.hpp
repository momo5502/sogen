#pragma once

#include "../std_include.hpp"

#include "macos_guest_call.hpp"
#include "macos_native_dispatch.hpp"
#include "macos_event_stream.hpp"
#include "macos_layer_contents.hpp"
#include "macos_objc_intercept.hpp"
#include "macos_window_server.hpp"
#include "../mach/io_surface_user_client.hpp"
#include "../module/macos_cache_symbols.hpp"
#include "../module/dyld_shared_cache.hpp"

#include <platform/ui_backend.hpp>

#include <functional>
#include <vector>

namespace sogen
{
    class macos_emulator;

    // Guest memory the emulator owns and the guest is handed a pointer into. Nothing here is ever
    // unmapped: sogen hands these addresses to CoreGraphics and CoreFoundation and is never told when
    // the guest has finished with them, so tearing a block down turns every surviving guest reference
    // into a write through freed memory. Blocks live in MACOS_GUI_ARENA_BASE, which the guest's own
    // allocator is kept out of, so the damage a stale reference can do stays inside the emulator's own
    // buffers instead of reaching the guest's heap.
    class macos_gui_arena
    {
      public:
        // MACOS_GUI_ARENA_BASE is also MACOS_DEFAULT_MMAP_BASE, so until this runs the arena is the
        // guest's own mmap arena: every bounds check against it is vacuous, and a block the arena hands
        // out can be handed to libmalloc as well. Moves the floor of the guest's unhinted search above
        // every range the emulator places at a fixed address, which is where it belongs whether or not
        // anything is drawn.
        static void exclude_guest(macos_emulator& emu);

        // Zero when the arena cannot hold the request. Rounded to whole pages, so a block is never a
        // slice of a page another block also owns.
        uint64_t acquire(macos_emulator& emu, size_t bytes);

        // The guest holds no reference any more, so the block may serve the next request.
        void recycle(uint64_t address);

        // The guest may still hold a pointer into the block. It stays mapped and is never handed out
        // again.
        void retire(uint64_t address);

        size_t block_count() const
        {
            return this->blocks_.size();
        }

        size_t retired_count() const;

        // How many bytes the block holding this address really covers, so a caller can tell whether the
        // block it was given still fits what it now wants to put in it. Zero for an address the arena
        // never handed out.
        size_t capacity(uint64_t address) const;

      private:
        struct block
        {
            uint64_t address{};
            size_t size{};
            bool in_use{};
            bool retired{};
        };

        // blocks_.size() when the arena never handed the address out.
        size_t index_of(uint64_t address) const;

        std::vector<block> blocks_{};
    };

    // Everything the GUI path owns, in one place the emulator can hold by value. Nothing here does
    // anything until bind() succeeds: an emulator that is never asked for a GUI keeps the null backend
    // and pays for none of it.
    class macos_ui_state
    {
      public:
        macos_window_server server{};
        macos_native_dispatch dispatch{};
        macos_guest_call_stack calls{};
        macos_event_stream events{};
        macos_layer_contents_resolver contents{};
        macos_gui_arena arena{};

        // IOSurfaceRoot is a kernel service rather than a GUI object, but every surface a guest makes is
        // a picture and it is the graphics path that keeps them alive, so it is held here with the rest.
        // Unlike everything else in this class it works with no backend bound: a guest can create a
        // surface without ever asking for a window.
        mach::io_surface_store surfaces{};

        bool enabled{false};
        int32_t desktop_width{1440};
        int32_t desktop_height{900};

        bool bind(macos_emulator& emu, const std::filesystem::path& host_cache_path);

        // The macOS side owns the backend's event sink, the way windows_emulator owns it for the Windows
        // side: a front-end that installed one of its own would take input away from the guest. What a
        // front-end wants instead is to watch, which is what the observer is for.
        using input_observer = std::function<void(const ui_event& event, bool delivered)>;
        void attach_input(macos_emulator& emu);
        void set_input_observer(input_observer observer);

        // Drains whatever the host backend has queued into the guest. Called from wherever the emulator
        // is idle enough to notice: nothing arrives while the guest is running, so a run loop that never
        // pumps is a guest that never sees a click.
        void pump_input(macos_emulator& emu);

        size_t delivered_input_count() const
        {
            return this->delivered_input_count_;
        }

        // ObjC method interception over the cache and dispatch the SkyLight path already set up.
        // Anything asked for before bind() succeeds has no cache behind it and reports nothing.
        std::vector<macos_objc_method_binding> bind_objc(macos_emulator& emu, const std::vector<macos_objc_method>& methods);

        // The layer-tree interception needs the pristine bytes of every method it patches, so it needs
        // the cache reader this class already owns rather than a second parse of a multi-gigabyte file.
        const dyld_shared_cache_reader* cache() const
        {
            return this->cache_ ? &*this->cache_ : nullptr;
        }

        const macos_cache_symbols* symbols() const
        {
            return this->symbols_ ? &*this->symbols_ : nullptr;
        }

        bool bound() const
        {
            return this->bound_;
        }

        ui_backend& host(macos_emulator& emu);

        void sync_window(macos_emulator& emu, const macos_window& window);
        void forget_window(macos_emulator& emu, uint32_t id);
        void apply_opacity(macos_emulator& emu, const macos_window& window);

        // CGWindowContextCreate hands back a recording context -- measured on 25G76: type 3, and
        // CGBitmapContextGetData returns NULL -- so there are no pixels behind it. sogen substitutes a
        // bitmap context over memory it owns, and the guest's own CoreGraphics rasterises into it. These
        // are the guest entry points that make one; they are called, never patched.
        uint64_t colorspace_create() const
        {
            return this->colorspace_create_;
        }

        uint64_t bitmap_context_create() const
        {
            return this->bitmap_context_create_;
        }

        // A CGSRegionRef is an opaque CoreGraphics object whose layout is private, so sogen never reads
        // one: it asks the guest's own CoreGraphics for the bounds, and lets the guest's own constructor
        // build the region in the first place. Zero on a release that renames either export, which
        // leaves the synthetic region table as the only source of a rect.
        uint64_t region_get_bounds() const
        {
            return this->region_get_bounds_;
        }

        uint64_t region_new_with_rect_list() const
        {
            return this->region_new_with_rect_list_;
        }

        // One CGRect of guest memory in the GUI arena, reused by every region-bounds call. The calls are
        // serialised by the guest-call stack, so one buffer is enough.
        uint64_t region_rect_scratch(macos_emulator& emu);

        bool ensure_backing_store(macos_emulator& emu, macos_window& window);
        void release_backing_store(macos_emulator& emu, macos_window& window);
        bool present(macos_emulator& emu, const macos_window& window, const RECT& dirty);

        size_t present_count() const
        {
            return this->present_count_;
        }

      private:
        std::optional<dyld_shared_cache_reader> cache_{};
        std::optional<macos_cache_symbols> symbols_{};
        bool bound_{false};
        uint64_t colorspace_create_{};
        uint64_t bitmap_context_create_{};
        uint64_t region_get_bounds_{};
        uint64_t region_new_with_rect_list_{};
        uint64_t region_rect_scratch_{};

        // ui_backend has no way to ask whether it already holds a window, and adding one would make every
        // Windows presenter implement a query only this path needs. Which ids have been mirrored is the
        // caller's state anyway.
        std::set<uint32_t> mirrored_{};

        size_t present_count_{};
        std::vector<uint8_t> present_scratch_{};

        input_observer input_observer_{};
        bool input_attached_{false};
        size_t delivered_input_count_{};
    };
}
