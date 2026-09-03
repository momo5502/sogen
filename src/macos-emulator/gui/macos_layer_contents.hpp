#pragma once

#include "../std_include.hpp"

#include "../module/dyld_shared_cache.hpp"
#include "../module/macos_cache_symbols.hpp"
#include "macos_layer_tree.hpp"

#include <map>
#include <string>
#include <vector>

namespace sogen
{
    class macos_emulator;

    // Pixels for a layer's `contents`. The tree records the object a guest handed to
    // -[CALayer setContents:]; turning it into a raster the compositor can blit means asking the
    // guest's own CoreGraphics to draw it, because a CGImage is an opaque object whose backing may be
    // compressed, subsampled, in a colour space sogen does not decode, or owned by a CABackingStore
    // that has no pixels at all until it is asked for one.
    //
    // Every step is a call *into* the guest, so the whole thing is a chain of continuations over
    // macos_guest_call_stack -- a handler cannot re-enter the emulator. The result is cached by object
    // pointer, because contents change far more rarely than CATransaction commits (measured: 37
    // setContents: calls to a SwiftUI first frame, hundreds of commits).
    //
    // What a `contents` object may be is measured, not assumed (25G76, an AppKit window with one
    // button, src/tools/macos-gui-probe): a CGImage, a CABackingStore, or an ObjC instance that is
    // neither -- CATintedImage and NSViewBackingLayerContents both appear, and CFGetTypeID answers the
    // base CFType id for them because they are not CoreFoundation types at all. Which one it is has to
    // be settled before anything is called on it, because neither CGImageGetWidth nor
    // CABackingStoreCopyCGImage validates what it is handed: the first reads a field at a fixed offset
    // and the second dereferences its argument as a backing store and faults.
    //
    // A CFTypeID is an index into CoreFoundation's runtime table in registration order, so it is a
    // property of the process and not of the release -- the same probe built twice answered 110/112 and
    // 91/90 for CGImage/CABackingStore. Both are asked for at runtime and neither may be written down.
    class macos_layer_contents_resolver
    {
      public:
        struct symbols
        {
            uint64_t backing_store_copy_image{};
            uint64_t backing_store_get_type_id{};
            uint64_t image_get_width{};
            uint64_t image_get_height{};
            uint64_t image_get_type_id{};
            uint64_t image_release{};
            uint64_t colorspace_create_device_rgb{};
            uint64_t bitmap_context_create{};
            uint64_t context_draw_image{};
            uint64_t context_set_fill_color{};
            uint64_t context_release{};
            uint64_t cf_get_type_id{};

            bool complete() const;
        };

        // The ObjC runtime entry points the accessor route needs. Optional: a system that does not
        // export one of them leaves the route off and an ObjC contents object simply stays unresolved.
        struct objc_symbols
        {
            uint64_t sel_register_name{};
            uint64_t object_get_class{};
            uint64_t class_get_instance_method{};
            uint64_t method_get_type_encoding{};
            uint64_t class_get_name{};
            uint64_t msg_send{};

            bool complete() const;
        };

        // Resolves every export the chain needs and reports each missing one by name. False leaves the
        // resolver inert: a layer's contents stay unresolved and the compositor draws the rest of it.
        bool bind(macos_emulator& emu, const macos_cache_symbols& cache_symbols);

        // The already-resolved form: the chain is the same whether the addresses came out of the cache
        // or were measured somewhere else.
        void bind(const symbols& resolved);
        void bind_objc(const objc_symbols& resolved);

        bool bound() const
        {
            return this->symbols_.complete();
        }

        // Attaches every raster already resolved and starts a chain over the objects that have none.
        // False when there is nothing to do, when the resolver is not bound, or when a chain is already
        // in flight -- the caller composites with whatever rasters exist either way.
        bool resolve_one(macos_emulator& emu, macos_layer_tree& tree);

        // Drops the raster taken from a contents object the guest has drawn over, so the next commit
        // takes a fresh one. The block the old raster lived in goes back to the arena: an application
        // that redraws does so on every frame, and a new block per redraw exhausts it.
        void forget(macos_emulator& emu, macos_layer_tree& tree, uint64_t object);

        size_t resolved_count() const
        {
            return this->resolved_;
        }

        size_t failed_count() const
        {
            return this->failed_;
        }

        void reset();

      private:
        struct pending
        {
            uint64_t object{};
            uint64_t object_type_id{};
            uint64_t objc_class{};
            size_t accessor{};

            // Why the contents object yielded nothing, kept while the guest is asked for its class name
            // so the refusal can report both at once.
            std::string fallback_reason{};
            uint64_t image{};
            macos_layer_color tint{};
            bool owns_image{};
            uint32_t width{};
            uint32_t height{};
            uint64_t pixels{};
            uint32_t stride{};
            uint64_t context{};
        };

        struct release_task
        {
            uint64_t context{};
            uint64_t image{};
        };

        void start_type_ids(macos_emulator& emu);
        void on_image_type_id(macos_emulator& emu, uint64_t type_id);
        void on_backing_store_type_id(macos_emulator& emu, uint64_t type_id);

        void start_selectors(macos_emulator& emu);
        void on_selector(macos_emulator& emu, uint64_t selector);

        void start_next(macos_emulator& emu);
        bool start_chain(macos_emulator& emu, uint64_t object);
        void on_object_type_id(macos_emulator& emu, uint64_t type_id);
        void on_copied_image(macos_emulator& emu, uint64_t image);
        void on_copied_image_type_id(macos_emulator& emu, uint64_t type_id);

        void ask_objc_for_image(macos_emulator& emu);
        void on_objc_class(macos_emulator& emu, uint64_t objc_class);
        void try_next_accessor(macos_emulator& emu);
        void on_objc_method(macos_emulator& emu, uint64_t method);
        void on_objc_encoding(macos_emulator& emu, uint64_t encoding);
        void on_objc_image(macos_emulator& emu, uint64_t image);
        void on_objc_image_type_id(macos_emulator& emu, uint64_t type_id);
        void ask_objc_for_tint(macos_emulator& emu);
        void on_tint_method(macos_emulator& emu, uint64_t method);
        void on_tint_encoding(macos_emulator& emu, uint64_t encoding);
        void on_tint(macos_emulator& emu, uint64_t tint);
        void draw_into_context(macos_emulator& emu);
        void on_fill_colour_set(macos_emulator& emu, uint64_t result);
        void refuse_objc(macos_emulator& emu);
        void on_objc_class_name(macos_emulator& emu, uint64_t name);
        std::string describe_empty_backing_store(macos_emulator& emu, uint64_t store) const;
        void measure_image(macos_emulator& emu);
        void on_width(macos_emulator& emu, uint64_t width);
        void on_height(macos_emulator& emu, uint64_t height);
        void on_colorspace(macos_emulator& emu, uint64_t colorspace);
        void on_context(macos_emulator& emu, uint64_t context);
        void finish(macos_emulator& emu);
        void on_context_released(macos_emulator& emu, uint64_t result);
        void on_image_released(macos_emulator& emu, uint64_t result);
        void abandon(macos_emulator& emu, const std::string& reason);
        void refuse(macos_emulator& emu, uint64_t object, const std::string& reason);

        bool call(macos_emulator& emu, uint64_t function, std::array<uint64_t, 8> args,
                  void (macos_layer_contents_resolver::*next)(macos_emulator&, uint64_t));

        void attach(macos_layer_tree& tree, uint64_t object, const macos_layer_raster& raster);

        // Ends a drain. A commit composites before the drain it starts can finish, so a contents object
        // first seen on this commit is missing from the frame the guest just asked for; presenting again
        // once the drain settles is what puts it on screen without waiting for a commit that, after a
        // click, may never come.
        void settle(macos_emulator& emu);

        symbols symbols_{};
        objc_symbols objc_{};

        // Held only for as long as a drain lasts, so a raster reaches the layers that asked for it on
        // the commit that produced it rather than the one after. Cleared the moment the queue empties.
        macos_layer_tree* tree_{};

        uint64_t colorspace_{};
        uint64_t image_type_id_{};
        uint64_t backing_store_type_id_{};
        bool type_ids_known_{};

        uint64_t accessor_names_{};
        std::vector<uint64_t> accessors_{};
        size_t accessors_registered_{};
        bool accessors_known_{};
        std::optional<pending> in_flight_{};
        release_task release_{};
        std::vector<uint64_t> queue_{};
        std::map<uint64_t, macos_layer_raster> cache_{};
        std::set<uint64_t> refused_{};
        bool attached_{};
        size_t resolved_{};
        size_t failed_{};
    };

    // Guest memory the rasters live in, carved out of the GUI arena above the window backing stores so
    // a present can bounds-check a raster the same way it checks a backing store.
    uint64_t macos_layer_raster_allocate(macos_emulator& emu, size_t bytes);
}
