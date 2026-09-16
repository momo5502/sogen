#include "../std_include.hpp"
#include "macos_ui_state.hpp"

#include "macos_appkit_intercept.hpp"
#include "macos_process_manager_routines.hpp"
#include "macos_io_surface_routines.hpp"

#include "../mach/mig_kernel_servers.hpp"
#include "macos_layer_tree.hpp"

#include "macos_gui_exports.hpp"
#include "macos_window_server_mig.hpp"
#include "skylight_routines.hpp"
#include "../macos_emulator.hpp"

#include <screenshot_ui_backend.hpp>

#include "../host_range_reader.hpp"

#include <algorithm>
#include <array>
#include <vector>

namespace sogen
{
    namespace
    {
        // Exports whose result is an object SkyLight's own code dereferences afterwards. Replacing one
        // outright makes every routine sogen did not also replace fault on the handle it invented, so
        // these observe and then continue into the real implementation. macos_layer_tree_bind has to
        // have run first: it owns the trampoline page and drops it when it rebinds.
        struct pass_through_export
        {
            std::string_view symbol{};
            std::string_view consequence{};
        };

        void install_pass_through_exports(macos_emulator& emu, const dyld_shared_cache_reader& cache, const macos_cache_symbols& symbols,
                                          macos_native_dispatch& dispatch)
        {
            constexpr std::array exports{
                pass_through_export{"_SLSTransactionCreate", "SkyLight's own transaction object will not exist and every "
                                                             "SLSTransaction routine sogen does not intercept will fault on the "
                                                             "handle it invents"},
                pass_through_export{"_SLSServerPort", "SkyLight will never mark itself connected to the window server, and "
                                                      "CGSSnarfAndDispatchDatagrams returns without pulling, so no input event "
                                                      "ever reaches the guest"},
            };

            for (const auto& [symbol, consequence] : exports)
            {
                const auto entry = symbols.find_export(MACOS_SKYLIGHT_IMAGE_PATH, symbol);
                if (!entry)
                {
                    continue;
                }

                const auto name = std::string{symbol};
                if (!macos_layer_tree_reinstall_export(emu, cache, dispatch, *entry, name, dispatch.handler_for(*entry)))
                {
                    emu.log.warn("%s could not be made an observation point; %.*s\n", name.c_str(), static_cast<int>(consequence.size()),
                                 consequence.data());
                }
            }
        }
    }

    bool macos_ui_state::bind(macos_emulator& emu, const std::filesystem::path& host_cache_path)
    {
        if (this->bound_)
        {
            return true;
        }

        if (!this->calls.prepare(emu))
        {
            return false;
        }

        if (!this->cache_)
        {
            try
            {
                // Through the same reader the cache pager uses, not plain file I/O. In the browser the
                // cache is a set of lazily fetched files and every other reader in the emulator reaches
                // them this way; opening them again independently would fetch what has already been
                // fetched, and on a 1.6 GiB subcache that is not a small difference.
                this->cache_.emplace(
                    dyld_shared_cache_reader::parse(host_cache_path, make_host_range_cache_opener(default_host_range_reader())));
            }
            catch (const std::exception& e)
            {
                emu.log.warn("GUI interception needs the shared cache and could not read %s: %s\n", host_cache_path.string().c_str(),
                             e.what());
                return false;
            }

            this->symbols_.emplace(*this->cache_);
        }

        const auto resolve = [&](const std::string_view symbol) {
            const auto address = this->symbols_->find_export(MACOS_CORE_GRAPHICS_IMAGE_PATH, symbol);
            if (!address)
            {
                emu.log.warn("CoreGraphics does not export %.*s on this system; window contents cannot be rasterised\n",
                             static_cast<int>(symbol.size()), symbol.data());
            }

            return address.value_or(0);
        };

        this->colorspace_create_ = resolve("_CGColorSpaceCreateDeviceRGB");
        this->bitmap_context_create_ = resolve("_CGBitmapContextCreate");

        const auto resolve_region = [&](const std::string_view symbol) {
            const auto address = this->symbols_->find_export(MACOS_CORE_GRAPHICS_IMAGE_PATH, symbol);
            if (!address)
            {
                emu.log.warn("CoreGraphics does not export %.*s on this system; a window shape can only be read from a region "
                             "sogen made itself\n",
                             static_cast<int>(symbol.size()), symbol.data());
            }

            return address.value_or(0);
        };

        this->region_get_bounds_ = resolve_region("_CGSGetRegionBounds");
        this->region_new_with_rect_list_ = resolve_region("_CGSNewRegionWithRectList");

        register_skylight_first_pixel_routines(this->dispatch);
        register_process_manager_routines(this->dispatch);
        register_io_surface_routines(this->dispatch);

        const auto bound = this->dispatch.bind(emu, *this->symbols_);
        emu.log.info("GUI: %zu of %zu routines intercepted, %zu unavailable on this system\n", bound, this->dispatch.registered_count(),
                     this->dispatch.unbound_symbols().size());
        emu.callbacks.on_gui_routines_bound(bound, this->dispatch.registered_count(), this->dispatch.unbound_symbols().size());

        emu.set_native_dispatch(&this->dispatch);
        emu.set_guest_call_stack(&this->calls);

        this->events.install(emu);
        this->attach_input(emu);
        register_event_stream_routines(mach::kernel_mig_servers());
        register_window_server_mig_routines(mach::kernel_mig_servers());

        // The CALayer setters carry the pixels of every app that does not paint into a window-sized
        // bitmap, which is every AppKit and SwiftUI app on this release. Observing them is what gives
        // the compositor a tree to rasterise.
        macos_layer_tree_bind(emu, *this->cache_, *this->symbols_, this->dispatch);
        macos_appkit_bind(emu, *this->cache_, *this->symbols_, this->dispatch);
        install_pass_through_exports(emu, *this->cache_, *this->symbols_, this->dispatch);
        this->contents.bind(emu, *this->symbols_);

        macos_gui_arena::exclude_guest(emu);

        this->bound_ = true;
        return true;
    }

    std::vector<macos_objc_method_binding> macos_ui_state::bind_objc(macos_emulator& emu, const std::vector<macos_objc_method>& methods)
    {
        if (!this->bound_)
        {
            return {};
        }

        return bind_objc_methods(emu, *this->cache_, *this->symbols_, this->dispatch, methods);
    }

    ui_backend& macos_ui_state::host(macos_emulator& emu)
    {
        return emu.ui_host();
    }

    void macos_ui_state::attach_input(macos_emulator& emu)
    {
        if (this->input_attached_)
        {
            return;
        }

        this->input_attached_ = true;
        this->host(emu).set_event_sink([this, &emu](const ui_event& event) {
            const auto delivered = macos_translate_ui_event(emu, event);
            this->delivered_input_count_ += delivered ? 1 : 0;

            if (this->input_observer_)
            {
                this->input_observer_(event, delivered);
            }
        });
    }

    void macos_ui_state::set_input_observer(input_observer observer)
    {
        this->input_observer_ = std::move(observer);
    }

    void macos_ui_state::pump_input(macos_emulator& emu)
    {
        if (!this->input_attached_)
        {
            return;
        }

        this->host(emu).pump_events();
    }

    void macos_ui_state::sync_window(macos_emulator& emu, const macos_window& window)
    {
        auto& backend = this->host(emu);

        macos_window_shmem_refresh(emu, window);

        if (this->mirrored_.insert(window.id).second)
        {
            backend.create_window(ui_window_desc{
                .handle = window.id,
                .rect = window.rect(),
                .title = window.title,
                .visible = window.ordered_in,
                .top_level = true,
            });

            this->apply_opacity(emu, window);
            return;
        }

        backend.set_window_rect(window.id, window.rect());
        backend.set_window_visible(window.id, window.ordered_in);
        backend.set_window_title(window.id, window.title);
        this->apply_opacity(emu, window);
    }

    void macos_ui_state::forget_window(macos_emulator& emu, const uint32_t id)
    {
        if (this->mirrored_.erase(id) > 0)
        {
            this->host(emu).destroy_window(id);
        }
    }

    // ui_backend has no opacity entry point -- only the screenshot backend has a use for one, and adding
    // it to the shared interface would make every Windows presenter implement it.
    void macos_ui_state::apply_opacity(macos_emulator& emu, const macos_window& window)
    {
        if (auto* shot = dynamic_cast<screenshot_ui_backend*>(&this->host(emu)))
        {
            shot->set_window_opaque(window.id, window.opaque);
        }
    }

    // A GUI buffer released into the pool libmalloc draws from becomes guest heap while CoreGraphics
    // still holds a bitmap context over it, which is the fault measured in
 //. The floor only
    // ever moves forward, so whatever the guest owns below it stays the guest's; the workqueue arena is
    // in the bound because its slots are claimed at fixed addresses a guest allocation would otherwise
    // reach first.
    void macos_gui_arena::exclude_guest(macos_emulator& emu)
    {
        constexpr auto arenas_end =
            std::max(MACOS_GUI_ARENA_BASE + MACOS_GUI_ARENA_SIZE, MACOS_WORKQUEUE_ARENA_BASE + MACOS_WORKQUEUE_ARENA_SIZE);
        emu.memory.set_mmap_base(std::max(emu.memory.get_mmap_base(), arenas_end));
    }

    size_t macos_gui_arena::index_of(const uint64_t address) const
    {
        for (size_t i = 0; i < this->blocks_.size(); ++i)
        {
            if (this->blocks_.at(i).address == address)
            {
                return i;
            }
        }

        return this->blocks_.size();
    }

    size_t macos_gui_arena::retired_count() const
    {
        size_t retired = 0;
        for (const auto& entry : this->blocks_)
        {
            retired += entry.retired ? 1u : 0u;
        }

        return retired;
    }

    size_t macos_gui_arena::capacity(const uint64_t address) const
    {
        const auto index = this->index_of(address);
        return index < this->blocks_.size() ? this->blocks_.at(index).size : 0;
    }

    uint64_t macos_gui_arena::acquire(macos_emulator& emu, const size_t bytes)
    {
        if (bytes == 0)
        {
            return 0;
        }

        const auto rounded = (bytes + MACOS_PAGE_SIZE - 1) & ~(MACOS_PAGE_SIZE - 1);
        if (rounded < bytes)
        {
            return 0;
        }

        block* best = nullptr;
        for (auto& entry : this->blocks_)
        {
            if (entry.in_use || entry.retired || entry.size < rounded)
            {
                continue;
            }

            if (best == nullptr || entry.size < best->size)
            {
                best = &entry;
            }
        }

        if (best != nullptr)
        {
            best->in_use = true;

            // A fresh mapping arrives zeroed and a recycled block carries the last tenant's bytes, so
            // without this the two differ. It is not housekeeping: a raster is drawn by handing the
            // block to CGBitmapContextCreate, and CGContextDrawImage composites source-over, so whatever
            // an earlier picture left behind shows through the transparent parts of the new one.
            const std::vector<uint8_t> cleared(best->size, 0);
            emu.memory.try_write_memory(best->address, cleared.data(), cleared.size());

            return best->address;
        }

        const auto address = emu.memory.allocate_memory(rounded, memory_permission::read_write, MACOS_GUI_ARENA_BASE);
        if (address == 0)
        {
            return 0;
        }

        // The base is a search hint rather than a bound: find_free_allocation_base floors at it and then
        // walks forward past everything mapped without stopping at the arena's end.
        if (address + rounded > MACOS_GUI_ARENA_BASE + MACOS_GUI_ARENA_SIZE)
        {
            emu.memory.release_memory(address, rounded);
            return 0;
        }

        this->blocks_.push_back(block{.address = address, .size = rounded, .in_use = true, .retired = false});
        return address;
    }

    void macos_gui_arena::recycle(const uint64_t address)
    {
        const auto index = this->index_of(address);
        if (index < this->blocks_.size())
        {
            this->blocks_.at(index).in_use = false;
        }
    }

    void macos_gui_arena::retire(const uint64_t address)
    {
        const auto index = this->index_of(address);
        if (index < this->blocks_.size())
        {
            this->blocks_.at(index).in_use = false;
            this->blocks_.at(index).retired = true;
        }
    }

    uint64_t macos_ui_state::region_rect_scratch(macos_emulator& emu)
    {
        if (this->region_rect_scratch_ == 0)
        {
            this->region_rect_scratch_ = this->arena.acquire(emu, sizeof(double) * 4);
        }

        return this->region_rect_scratch_;
    }

    bool macos_ui_state::ensure_backing_store(macos_emulator& emu, macos_window& window)
    {
        if (window.width <= 0 || window.height <= 0 || window.width > MACOS_GUI_MAX_WINDOW_DIMENSION ||
            window.height > MACOS_GUI_MAX_WINDOW_DIMENSION)
        {
            return false;
        }

        const auto stride = static_cast<uint32_t>(window.width) * 4u;
        const auto bytes = static_cast<size_t>(stride) * static_cast<size_t>(window.height);

        if (window.backing_address != 0)
        {
            // A window keeps its record across a resize, and every reader of the store sizes itself from
            // backing_stride * height. Calculator opens 460x52 and settles at 230x408, so a store kept
            // from the first size is read and written 650 KiB past its own end -- which only ever worked
            // because the guest's heap shared the arena and happened to have that span mapped.
            if (window.backing_stride == stride && this->arena.capacity(window.backing_address) >= bytes)
            {
                return true;
            }

            this->release_backing_store(emu, window);
        }

        const auto address = this->arena.acquire(emu, bytes);
        if (address == 0)
        {
            emu.log.warn("No room in the GUI arena for a %dx%d backing store\n", window.width, window.height);
            return false;
        }

        window.backing_address = address;
        window.backing_stride = stride;
        return true;
    }

    void macos_ui_state::release_backing_store(macos_emulator&, macos_window& window)
    {
        if (window.backing_address == 0)
        {
            return;
        }

        // SLWindowContextCreate builds a CGBitmapContext straight over the backing store and hands it to
        // the guest, and nothing tells sogen when the guest lets that context go. Every CGContext draw
        // afterwards writes through the address, so a store a context was made over can never serve
        // another window; one that was only ever written by the compositor can.
        if (window.context != 0)
        {
            this->arena.retire(window.backing_address);
        }
        else
        {
            this->arena.recycle(window.backing_address);
        }

        window.backing_address = 0;
        window.backing_stride = 0;
        window.context = 0;
    }

    bool macos_ui_state::present(macos_emulator& emu, const macos_window& window, const RECT& dirty)
    {
        (void)dirty;

        if (window.width <= 0 || window.height <= 0 || window.width > MACOS_GUI_MAX_WINDOW_DIMENSION ||
            window.height > MACOS_GUI_MAX_WINDOW_DIMENSION)
        {
            return false;
        }

        const auto row_bytes = static_cast<size_t>(window.width) * 4u;
        if (window.backing_stride < row_bytes)
        {
            return false;
        }

        const auto bytes = window.backing_bytes();
        if (bytes == 0)
        {
            return false;
        }

        // The extent is checked against the store rather than trusted: it is computed from a record the
        // guest's own handlers write into, and a present that read past the store would copy whatever
        // the arena put next to it -- another window's pixels -- into a screenshot. The arena bound is
        // kept alongside it because an address the arena never handed out has no capacity to check.
        if (window.backing_address < MACOS_GUI_ARENA_BASE || window.backing_address + bytes > MACOS_GUI_ARENA_BASE + MACOS_GUI_ARENA_SIZE ||
            this->arena.capacity(window.backing_address) < bytes)
        {
            return false;
        }

        this->present_scratch_.resize(bytes);
        if (!emu.memory.try_read_memory(window.backing_address, this->present_scratch_.data(), this->present_scratch_.size()))
        {
            return false;
        }

        this->host(emu).present_surface(window.id, ui_surface_desc{
                                                       .width = window.width,
                                                       .height = window.height,
                                                       .stride = static_cast<int>(window.backing_stride),
                                                       .format = ui_surface_format::bgra8,
                                                       .pixels = this->present_scratch_.data(),
                                                   });

        ++this->present_count_;
        return true;
    }
}
