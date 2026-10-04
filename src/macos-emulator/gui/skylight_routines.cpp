#include "../std_include.hpp"
#include "skylight_routines.hpp"

#include "macos_cf_bridge.hpp"
#include "macos_event_stream.hpp"
#include "macos_layer_tree.hpp"

#include "macos_gui_exports.hpp"
#include "macos_window_server_mig.hpp"
#include "macos_ui_state.hpp"
#include "../macos_emulator.hpp"

#include <platform/unicode.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <optional>
#include <set>

namespace sogen
{
    namespace
    {
        macos_ui_state& ui_of(const macos_native_call& call)
        {
            return call.emu_ref.ui;
        }

        void succeed(const macos_native_call& call)
        {
            call.ret(static_cast<uint64_t>(static_cast<uint32_t>(MACOS_CG_ERROR_SUCCESS)));
        }

        void fail(const macos_native_call& call, const int32_t error)
        {
            call.ret(static_cast<uint64_t>(static_cast<uint32_t>(error)));
        }

        // The answer a continuation leaves behind. Once a guest call has run, the macos_native_call the
        // handler was given is gone, and x0 as the last continuation leaves it is what the original
        // caller receives.
        void set_result(macos_emulator& emu, const int32_t error)
        {
            emu.emu().reg(arm64_register::x0, static_cast<uint64_t>(static_cast<uint32_t>(error)));
        }

        macos_window* window_for(const macos_native_call& call, const uint64_t connection, const uint64_t id)
        {
            auto& ui = ui_of(call);
            if (!ui.server.has_connection(static_cast<uint32_t>(connection)))
            {
                return nullptr;
            }

            return ui.server.find_window(static_cast<uint32_t>(id));
        }

        // A region is built by the guest's own CoreGraphics whenever that is reachable, because every
        // other region call an app makes -- CGSDiffRegion, CGSUnionRegionWithRect, CGRegionRelease --
        // is real CoreGraphics code that dereferences the handle. CGSNewRegionWithRectList is the
        // exported one-line wrapper around the same constructor and is not itself intercepted, so
        // forwarding one rect to it produces exactly the object CGSNewRegionWithRect would have. The
        // synthetic handle below is the answer for a configuration with no CoreGraphics behind it.
        void cgs_new_region_with_rect(const macos_native_call& call)
        {
            auto& ui = ui_of(call);
            const auto rect = read_cg_rect(call.emu_ref, call.arg(0));
            const auto out = call.arg(1);

            if (!rect || out == 0)
            {
                fail(call, MACOS_CG_ERROR_ILLEGAL_ARGUMENT);
                return;
            }

            if (ui.region_new_with_rect_list() != 0 && ui.calls.begin(call.emu_ref, macos_guest_call_request{
                                                                                        .function = ui.region_new_with_rect_list(),
                                                                                        .args = {call.arg(0), 1, out},
                                                                                    }))
            {
                return;
            }

            const auto id = ui.server.create_region(clamp_cg_dimension(rect->x), clamp_cg_dimension(rect->y),
                                                    clamp_cg_dimension(rect->width), clamp_cg_dimension(rect->height));
            if (id == 0)
            {
                fail(call, MACOS_CG_ERROR_ILLEGAL_ARGUMENT);
                return;
            }

            const uint64_t handle = id;
            if (!call.emu_ref.memory.try_write_memory(out, &handle, sizeof(handle)))
            {
                fail(call, MACOS_CG_ERROR_ILLEGAL_ARGUMENT);
                return;
            }

            succeed(call);
        }

        using region_rect_handler = std::function<void(macos_emulator&, const std::optional<macos_cg_rect>&)>;

        // Hands `then` the region's bounds and leaves it to write x0. True means the answer will arrive
        // from a guest call, so the caller must return without touching the register file; false means
        // `then` has already run.
        bool resolve_region_rect(const macos_native_call& call, const uint64_t region, region_rect_handler then)
        {
            auto& ui = ui_of(call);

            if (const auto* known = ui.server.find_region(static_cast<uint32_t>(region));
                known != nullptr && static_cast<uint64_t>(static_cast<uint32_t>(region)) == region)
            {
                then(call.emu_ref, macos_cg_rect{.x = static_cast<double>(known->x),
                                                 .y = static_cast<double>(known->y),
                                                 .width = static_cast<double>(known->width),
                                                 .height = static_cast<double>(known->height)});
                return false;
            }

            const auto scratch = ui.region_rect_scratch(call.emu_ref);
            if (region == 0 || ui.region_get_bounds() == 0 || scratch == 0)
            {
                then(call.emu_ref, std::nullopt);
                return false;
            }

            const auto started =
                ui.calls.begin(call.emu_ref, macos_guest_call_request{
                                                 .function = ui.region_get_bounds(),
                                                 .args = {region, scratch},
                                                 .on_return =
                                                     [scratch, then = std::move(then)](macos_emulator& emu, const uint64_t result) {
                                                         const auto rect = read_cg_rect(emu, scratch);
                                                         then(emu, result == 0 && rect ? rect : std::nullopt);
                                                     },
                                             });

            if (!started)
            {
                then(call.emu_ref, std::nullopt);
            }

            return started;
        }

        void sls_new_window(const macos_native_call& call)
        {
            // SLSNewWindow(cid, type, float x, float y, CGSRegionRef region, CGSWindowID* out). x and y
            // are C floats and travel in the vector bank, so they are not arg(2) and arg(3): those hold
            // the region and the out pointer.
            const auto connection = static_cast<uint32_t>(call.arg(0));
            const auto x = static_cast<double>(call.arg_float(0));
            const auto y = static_cast<double>(call.arg_float(1));
            const auto region = call.arg(2);
            const auto out = call.arg(3);

            resolve_region_rect(call, region, [connection, x, y, out](macos_emulator& emu, const std::optional<macos_cg_rect>& rect) {
                auto& state = emu.ui;
                if (!rect)
                {
                    set_result(emu, MACOS_CG_ERROR_ILLEGAL_ARGUMENT);
                    return;
                }

                auto* window = state.server.create_window(connection, clamp_cg_dimension(x), clamp_cg_dimension(y),
                                                          clamp_cg_dimension(rect->width), clamp_cg_dimension(rect->height));
                if (window == nullptr)
                {
                    set_result(emu, MACOS_CG_ERROR_INVALID_CONNECTION);
                    return;
                }

                if (!state.ensure_backing_store(emu, *window))
                {
                    state.server.destroy_window(window->id);
                    set_result(emu, MACOS_CG_ERROR_FAILURE);
                    return;
                }

                state.sync_window(emu, *window);

                const uint32_t id = window->id;
                if (out != 0 && !emu.memory.try_write_memory(out, &id, sizeof(id)))
                {
                    set_result(emu, MACOS_CG_ERROR_ILLEGAL_ARGUMENT);
                    return;
                }

                set_result(emu, MACOS_CG_ERROR_SUCCESS);
            });
        }

        // Measured on 25G76: SLSReleaseWindow is CGSWindowGetMappedImpl(create=0) plus
        // CGSWindowReleaseExplicit, which removes the window from SkyLight's client-side retained set
        // under __CGSLocalWindowsLock. It never reaches the wire and it always returns
        // kCGErrorSuccess, even for a window id the client has no record of. AppKit calls it on every
        // window it makes, immediately after -[NSCGSWindow initWithConnectionID:flags:] takes the
        // reference CGSDeviceCreate handed it, so destroying the server-side window here deleted every
        // AppKit window the instant it was created.
        void sls_release_window(const macos_native_call& call)
        {
            static bool reported = false;
            if (!reported)
            {
                reported = true;
                call.emu_ref.log.info("SLSReleaseWindow drops only SkyLight's client-side reference; sogen has no measured "
                                      "server-side window-destroy routine, so the window lives until the process exits\n");
            }

            succeed(call);
        }

        void sls_set_window_opacity(const macos_native_call& call)
        {
            auto& ui = ui_of(call);
            auto* window = window_for(call, call.arg(0), call.arg(1));
            if (window == nullptr)
            {
                fail(call, MACOS_CG_ERROR_ILLEGAL_ARGUMENT);
                return;
            }

            window->opaque = call.arg(2) != 0;
            ui.sync_window(call.emu_ref, *window);
            succeed(call);
        }

        void sls_set_window_level(const macos_native_call& call)
        {
            auto* window = window_for(call, call.arg(0), call.arg(1));
            if (window == nullptr)
            {
                fail(call, MACOS_CG_ERROR_ILLEGAL_ARGUMENT);
                return;
            }

            window->level = static_cast<int32_t>(call.arg(2));
            succeed(call);
        }

        void sls_get_window_level(const macos_native_call& call)
        {
            const auto* window = window_for(call, call.arg(0), call.arg(1));
            const auto out = call.arg(2);

            if (window == nullptr || out == 0)
            {
                fail(call, MACOS_CG_ERROR_ILLEGAL_ARGUMENT);
                return;
            }

            const auto level = window->level;
            if (!call.emu_ref.memory.try_write_memory(out, &level, sizeof(level)))
            {
                fail(call, MACOS_CG_ERROR_ILLEGAL_ARGUMENT);
                return;
            }

            succeed(call);
        }

        void sls_order_window(const macos_native_call& call)
        {
            auto& ui = ui_of(call);
            auto* window = window_for(call, call.arg(0), call.arg(1));
            if (window == nullptr)
            {
                fail(call, MACOS_CG_ERROR_ILLEGAL_ARGUMENT);
                return;
            }

            // Mode is -1 below, 0 out, 1 above. Anything other than 0 puts the window on screen.
            window->ordered_in = static_cast<int32_t>(call.arg(2)) != 0;
            ui.sync_window(call.emu_ref, *window);
            succeed(call);
        }

        void sls_move_window(const macos_native_call& call)
        {
            auto& ui = ui_of(call);
            auto* window = window_for(call, call.arg(0), call.arg(1));
            const auto point = call.arg(2);

            if (window == nullptr || point == 0)
            {
                fail(call, MACOS_CG_ERROR_ILLEGAL_ARGUMENT);
                return;
            }

            std::array<double, 2> coordinates{};
            if (!call.emu_ref.memory.try_read_memory(point, coordinates.data(), sizeof(coordinates)))
            {
                fail(call, MACOS_CG_ERROR_ILLEGAL_ARGUMENT);
                return;
            }

            window->x = clamp_cg_dimension(coordinates[0]);
            window->y = clamp_cg_dimension(coordinates[1]);
            ui.sync_window(call.emu_ref, *window);
            succeed(call);
        }

        // The emulated display. SkyLight answers all of these client-side out of the display shmem 34006
        // hands over, whose layout is not measured, so sogen answers the exports instead: with an
        // all-zero shmem AppKit sees a screen of no size and clamps every window it is asked to make
        // down to the smallest frame a titled window can have (measured: a 320x232 window came out
        // 1x32). One display, at the origin, the size the emulator was configured for.
        void sls_main_display_id(const macos_native_call& call)
        {
            call.ret(MACOS_MAIN_DISPLAY_ID);
        }

        // SLSGetDisplayList(uint32_t capacity, CGDirectDisplayID *displays, uint32_t *count) -- measured
        // on 25G76: the ids come back in the array and the count in *x2, exactly the CGGetDisplayList
        // contract, so a null array is a count query.
        void sls_get_display_list(const macos_native_call& call)
        {
            const auto capacity = static_cast<uint32_t>(call.arg(0));
            const auto displays = call.arg(1);
            const auto out_count = call.arg(2);

            uint32_t reported = 1;
            if (displays != 0)
            {
                reported = capacity == 0 ? 0u : 1u;
                const uint32_t id = MACOS_MAIN_DISPLAY_ID;
                if (reported != 0 && !call.emu_ref.memory.try_write_memory(displays, &id, sizeof(id)))
                {
                    fail(call, MACOS_CG_ERROR_ILLEGAL_ARGUMENT);
                    return;
                }
            }

            if (out_count != 0 && !call.emu_ref.memory.try_write_memory(out_count, &reported, sizeof(reported)))
            {
                fail(call, MACOS_CG_ERROR_ILLEGAL_ARGUMENT);
                return;
            }

            succeed(call);
        }

        // SLSGetDisplayBounds(CGDirectDisplayID, CGRect *out) -- measured: the rect is four doubles
        // through x1 and the return value is a CGError, not the rect.
        void sls_get_display_bounds(const macos_native_call& call)
        {
            auto& ui = ui_of(call);
            const auto out = call.arg(1);

            if (static_cast<uint32_t>(call.arg(0)) != MACOS_MAIN_DISPLAY_ID || out == 0)
            {
                fail(call, MACOS_CG_ERROR_ILLEGAL_ARGUMENT);
                return;
            }

            const std::array<double, 4> bounds{0.0, 0.0, static_cast<double>(ui.desktop_width), static_cast<double>(ui.desktop_height)};
            if (!call.emu_ref.memory.try_write_memory(out, bounds.data(), sizeof(bounds)))
            {
                fail(call, MACOS_CG_ERROR_ILLEGAL_ARGUMENT);
                return;
            }

            succeed(call);
        }

        void sls_get_window_bounds(const macos_native_call& call)
        {
            const auto* window = window_for(call, call.arg(0), call.arg(1));
            const auto out = call.arg(2);

            if (window == nullptr || out == 0)
            {
                fail(call, MACOS_CG_ERROR_ILLEGAL_ARGUMENT);
                return;
            }

            const std::array<double, 4> bounds{static_cast<double>(window->x), static_cast<double>(window->y),
                                               static_cast<double>(window->width), static_cast<double>(window->height)};

            if (!call.emu_ref.memory.try_write_memory(out, bounds.data(), sizeof(bounds)))
            {
                fail(call, MACOS_CG_ERROR_ILLEGAL_ARGUMENT);
                return;
            }

            succeed(call);
        }

        void sls_flush_window_content_region(const macos_native_call& call)
        {
            auto& ui = ui_of(call);
            auto* window = window_for(call, call.arg(0), call.arg(1));
            if (window == nullptr)
            {
                fail(call, MACOS_CG_ERROR_ILLEGAL_ARGUMENT);
                return;
            }

            if (!ui.present(call.emu_ref, *window, window->rect()))
            {
                fail(call, MACOS_CG_ERROR_FAILURE);
                return;
            }

            succeed(call);
        }

        // The substitution. CGWindowContextCreate hands back a recording context with no pixels behind
        // it, so this makes a bitmap context over the window's backing store instead and lets the guest's
        // own CoreGraphics rasterise into it -- every CGContextFillRect and CGContextShowText afterwards
        // is Apple's code drawing real pixels, the way gdi32 does on the Windows side.
        //
        // Two guest calls in sequence, so the handler starts the first and the continuation starts the
        // second. Returning from here without a result is correct: the value the caller sees is whatever
        // the last continuation leaves in x0.
        void sl_window_context_create(const macos_native_call& call)
        {
            auto& ui = ui_of(call);
            auto& emu = call.emu_ref;

            auto* window = window_for(call, call.arg(0), call.arg(1));
            if (window == nullptr || ui.colorspace_create() == 0 || ui.bitmap_context_create() == 0)
            {
                call.ret(0);
                return;
            }

            if (!ui.ensure_backing_store(emu, *window))
            {
                call.ret(0);
                return;
            }

            const auto id = window->id;

            const auto started = ui.calls.begin(
                emu, macos_guest_call_request{
                         .function = ui.colorspace_create(),
                         .on_return =
                             [id](macos_emulator& inner, const uint64_t colorspace) {
                                 auto* target = inner.ui.server.find_window(id);
                                 if (target == nullptr || colorspace == 0)
                                 {
                                     inner.emu().reg(arm64_register::x0, 0);
                                     return;
                                 }

                                 inner.ui.calls.begin(inner, macos_guest_call_request{
                                                                 .function = inner.ui.bitmap_context_create(),
                                                                 .args = {target->backing_address, static_cast<uint64_t>(target->width),
                                                                          static_cast<uint64_t>(target->height), 8, target->backing_stride,
                                                                          colorspace, MACOS_CG_BITMAP_INFO_BGRA_PREMULTIPLIED},
                                                                 .on_return =
                                                                     [id](macos_emulator& innermost, const uint64_t context) {
                                                                         if (auto* found = innermost.ui.server.find_window(id))
                                                                         {
                                                                             found->context = context;
                                                                         }
                                                                     },
                                                             });
                             },
                     });

            if (!started)
            {
                call.ret(0);
            }
        }

        // Accepted and recorded nowhere, because nothing downstream reads them yet. Reporting success is
        // the truthful answer: the guest asked for a property of a window sogen owns, and sogen has no
        // reason to refuse. Refusing instead would abort start-up in callers that check.
        void accept_silently(const macos_native_call& call)
        {
            if (window_for(call, call.arg(0), call.arg(1)) == nullptr)
            {
                fail(call, MACOS_CG_ERROR_ILLEGAL_ARGUMENT);
                return;
            }

            succeed(call);
        }

        void accept_without_window(const macos_native_call& call)
        {
            succeed(call);
        }

        mach::port_name_t ensure_send_port(const macos_native_call& call, const mach::kernel_object_kind kind, uint32_t& slot)
        {
            if (slot != 0)
            {
                return slot;
            }

            auto& ports = call.emu_ref.mach.ports;
            const auto receive = ports.allocate_receive_right({.kind = kind, .id = 1});
            if (receive == mach::PORT_NULL)
            {
                return mach::PORT_NULL;
            }

            slot = ports.insert_send_right(receive);
            return slot;
        }

        // The real routine is what marks SkyLight connected to the window server -- it stores 1 in a
        // private byte after 29010 and 29000 answer (SkyLight 0x186f28bf0 on 25G76). Every datagram pull
        // reads that byte first: CGSSnarfAndDispatchDatagrams returns at its second instruction while it
        // is clear (0x186cfb8ac), so an intercepted SLSServerPort silently costs the guest every input
        // event. Substituting the session port is only the answer with no cache behind the export.
        void sls_server_port(const macos_native_call& call)
        {
            if (macos_layer_tree_continue_into_original(call))
            {
                return;
            }

            call.ret(macos_window_server_session_port(call.emu_ref));
        }

        void ca_render_server_get_server_port(const macos_native_call& call)
        {
            call.ret(ensure_send_port(call, mach::kernel_object_kind::render_server, ui_of(call).server.render_server_port));
        }

        // SLSTransactionRef is a CFRuntime object SkyLight builds client-side -- no wire traffic --
        // and every SLSTransaction* routine sogen does not intercept dereferences it. AppKit reaches
        // SLSTransactionAddPostDecodeAction on the window's display cycle, so a handle sogen invented
        // is a wild pointer there. The real constructor therefore runs whenever the shared cache is
        // behind the export, and sogen records the transaction the first time the handle comes back to
        // an intercepted routine. Substituting it outright is only the answer with no cache present.
        void sls_transaction_create(const macos_native_call& call)
        {
            if (macos_layer_tree_continue_into_original(call))
            {
                return;
            }

            call.ret(ui_of(call).server.create_transaction(static_cast<uint32_t>(call.arg(0))));
        }

        // A handle sogen did not mint is SkyLight's own transaction object. Adopting it needs a
        // readable guest address behind it: that is the difference between a real transaction and the
        // garbage a caller can pass.
        macos_transaction* transaction_for(const macos_native_call& call, const uint64_t handle)
        {
            auto& ui = ui_of(call);
            if (auto* known = ui.server.find_transaction(handle))
            {
                return known;
            }

            uint32_t probe = 0;
            if (handle == 0 || !call.emu_ref.memory.try_read_memory(handle, &probe, sizeof(probe)))
            {
                return nullptr;
            }

            return &ui.server.adopt_transaction(handle, ui.server.main_connection());
        }

        // Measured 25G76: AppKit never calls SLSOrderWindow. -[NSWindow makeKeyAndOrderFront:] reaches
        // SLSTransactionOrderWindowGroupFrontConditionally(t, wid, ...), orderOut: reaches
        // SLSTransactionOrderWindowGroup(t, wid, 0, 0) and orderBack: the same with -1 -- wid in arg 1
        // and the order in arg 2, the slots SLSOrderWindow uses, with a transaction where it takes a
        // connection id. The host stages the op and applies it when the transaction commits; sogen
        // applies it at once, because unlike a frame change an ordering has nothing to be consistent
        // with and a transaction AppKit abandons is not observable any other way.
        void sls_transaction_order_window_group(const macos_native_call& call)
        {
            auto& ui = ui_of(call);
            auto* window = ui.server.find_window(static_cast<uint32_t>(call.arg(1)));

            if (transaction_for(call, call.arg(0)) == nullptr || window == nullptr)
            {
                fail(call, MACOS_CG_ERROR_ILLEGAL_ARGUMENT);
                return;
            }

            window->ordered_in = static_cast<int32_t>(call.arg(2)) != 0;
            ui.sync_window(call.emu_ref, *window);
            succeed(call);
        }

        void sls_transaction_order_window_group_front_conditionally(const macos_native_call& call)
        {
            auto& ui = ui_of(call);
            auto* window = ui.server.find_window(static_cast<uint32_t>(call.arg(1)));

            if (transaction_for(call, call.arg(0)) == nullptr || window == nullptr)
            {
                fail(call, MACOS_CG_ERROR_ILLEGAL_ARGUMENT);
                return;
            }

            window->ordered_in = true;
            ui.sync_window(call.emu_ref, *window);
            succeed(call);
        }

        void sls_transaction_set_window_shape(const macos_native_call& call)
        {
            // (t, wid, CGSRegionRef) measured against the runtime build; x3/x4 carry stale values. The
            // region resolves at set time on the host (CGSGetRegionData runs inside the routine).
            auto& ui = ui_of(call);
            const auto transaction_id = call.arg(0);
            const auto window_id = static_cast<uint32_t>(call.arg(1));

            if (transaction_for(call, transaction_id) == nullptr || ui.server.find_window(window_id) == nullptr)
            {
                fail(call, MACOS_CG_ERROR_ILLEGAL_ARGUMENT);
                return;
            }

            resolve_region_rect(call, call.arg(2),
                                [transaction_id, window_id](macos_emulator& emu, const std::optional<macos_cg_rect>& rect) {
                                    auto* transaction = emu.ui.server.find_transaction(transaction_id);
                                    if (transaction == nullptr || !rect)
                                    {
                                        set_result(emu, MACOS_CG_ERROR_ILLEGAL_ARGUMENT);
                                        return;
                                    }

                                    transaction->shapes.push_back(macos_pending_shape{
                                        .window_id = window_id,
                                        .x = clamp_cg_dimension(rect->x),
                                        .y = clamp_cg_dimension(rect->y),
                                        .width = clamp_cg_dimension(rect->width),
                                        .height = clamp_cg_dimension(rect->height),
                                    });

                                    set_result(emu, MACOS_CG_ERROR_SUCCESS);
                                });
        }

        void commit_transaction_call(const macos_native_call& call)
        {
            auto& ui = ui_of(call);
            const auto* transaction = transaction_for(call, call.arg(0));
            if (transaction == nullptr)
            {
                fail(call, MACOS_CG_ERROR_ILLEGAL_ARGUMENT);
                return;
            }

            // The ids of the windows being reshaped, gathered before commit clears the list, so each can
            // be mirrored onto the backend afterwards. Fixed stack cap: past it, sync everything rather
            // than allocate -- commit is the hot path and stays allocation-free.
            std::array<uint32_t, 32> touched{};
            size_t touched_count = 0;
            for (const auto& shape : transaction->shapes)
            {
                if (touched_count < touched.size())
                {
                    touched[touched_count++] = shape.window_id;
                }
            }
            const bool overflowed = transaction->shapes.size() > touched.size();

            for (const auto& shape : transaction->shapes)
            {
                const auto* window = ui.server.find_window(shape.window_id);
                if (window != nullptr &&
                    (window->x != shape.x || window->y != shape.y || window->width != shape.width || window->height != shape.height))
                {
                    call.emu_ref.log.info("window %u takes the shape (%d, %d, %d, %d)\n", shape.window_id, shape.x, shape.y, shape.width,
                                          shape.height);
                }
            }

            ui.server.commit_transaction(call.arg(0));

            if (overflowed)
            {
                for (const auto& window : ui.server.windows())
                {
                    ui.sync_window(call.emu_ref, window);
                }
            }
            else
            {
                for (size_t i = 0; i < touched_count; ++i)
                {
                    if (const auto* window = ui.server.find_window(touched[i]))
                    {
                        ui.sync_window(call.emu_ref, *window);
                    }
                }
            }

            succeed(call);

            // A committed transaction is the point where the window's frame is final and the layer tree
            // is whatever the app just built, which is the cadence a frame arrives at. +[CATransaction
            // flush] is not: AppKit's update cycle calls CA::Transaction::flush_as_runloop_observer
            // directly and never goes through the ObjC class method, so an AppKit app reaches its first
            // frame without a single +[CATransaction flush] (measured under sogen, 25G76).
            macos_layer_tree_present(call.emu_ref);

            // A layer's contents become pixels through a chain of calls into the guest's own
            // CoreGraphics, and only a handler that returns straight to its caller may start one: an
            // ObjC interception that continues into the original implementation wants pc for its
            // trampoline, and the two writes would fight. Commit is the per-frame export that
            // qualifies -- and the one whose return value is measured to be unused by every caller,
            // which matters because the chain's last call leaves its own result in x0.
            macos_layer_tree_resolve_contents(call.emu_ref);
        }

        void sls_transaction_commit_using_method(const macos_native_call& call)
        {
            // Measured methods: 1, 2, 3. The host abort()s on anything else; an error plus a named log
            // is the emulator's version of that.
            if (call.arg(1) > 3)
            {
                call.emu_ref.log.warn("SLSTransactionCommitUsingMethod: invalid method 0x%" PRIx64 "\n", call.arg(1));
                fail(call, MACOS_CG_ERROR_ILLEGAL_ARGUMENT);
                return;
            }

            commit_transaction_call(call);
        }

        // SLSNewWindowWithOpaqueShape(cid, type, float x, float y, shape, shape, flags, out_struct,
        // out_struct_size, CGSWindowID* out) -- the AppKit window-creation entry on 25G76; SLSNewWindow
        // never fires there anymore. The window id goes to *x7 and w0 carries the CGError (measured:
        // x7 points at the new CGSWindow object + 0x18, which -[NSCGSWindow initWithConnectionID:]
        // reads; the caller's error path is `cbnz w0`).
        void sls_new_window_with_opaque_shape(const macos_native_call& call)
        {
            const auto connection = static_cast<uint32_t>(call.arg(0));
            const auto x = static_cast<double>(call.arg_float(0));
            const auto y = static_cast<double>(call.arg_float(1));
            const auto region = call.arg(2);
            const auto out = call.arg(7);

            if (out == 0)
            {
                fail(call, MACOS_CG_ERROR_ILLEGAL_ARGUMENT);
                return;
            }

            // The shape argument is the window's *opaque* shape, not its frame. AppKit passes
            // CGRegionCreateEmptyRegion()'s result here (measured, 25G76), so it is empty at creation
            // and the frame arrives later through SLSTransactionSetWindowShape; a window whose shape is
            // empty starts at 1x1 rather than at nothing, because a zero-sized window has no backing
            // store to allocate.
            resolve_region_rect(call, region, [connection, x, y, out](macos_emulator& emu, const std::optional<macos_cg_rect>& rect) {
                auto& state = emu.ui;
                const auto width = rect ? std::max(clamp_cg_dimension(rect->width), 1) : 1;
                const auto height = rect ? std::max(clamp_cg_dimension(rect->height), 1) : 1;

                auto* window = state.server.create_window(connection, clamp_cg_dimension(x), clamp_cg_dimension(y), width, height);
                if (window == nullptr)
                {
                    set_result(emu, MACOS_CG_ERROR_INVALID_CONNECTION);
                    return;
                }

                state.sync_window(emu, *window);

                const uint32_t id = window->id;
                if (!emu.memory.try_write_memory(out, &id, sizeof(id)))
                {
                    set_result(emu, MACOS_CG_ERROR_ILLEGAL_ARGUMENT);
                    return;
                }

                set_result(emu, MACOS_CG_ERROR_SUCCESS);
            });
        }

        void sls_set_window_layer_context(const macos_native_call& call)
        {
            // (cid, wid, CAContext*) -- the host asks the context object for its contextId and sends MIG
            // 30264. The association is recorded as the object pointer; resolving it into a composited
            // layer is the compositor task.
            auto* window = window_for(call, call.arg(0), call.arg(1));
            if (window == nullptr || call.arg(2) == 0)
            {
                fail(call, MACOS_CG_ERROR_ILLEGAL_ARGUMENT);
                return;
            }

            window->layer_context = call.arg(2);
            succeed(call);
        }

        // The two titles an app sets are CFConstantStrings (measured layout: +0x08 info word, +0x10 UTF-8
        // data pointer, +0x18 byte length; info == 0x7c8 marks that form). Any other CFString
        // representation is reported by name once per info value and leaves the title alone rather than
        // reading the wrong offsets.
        void sls_set_window_title(const macos_native_call& call)
        {
            auto& ui = ui_of(call);
            auto* window = window_for(call, call.arg(0), call.arg(1));
            if (window == nullptr)
            {
                fail(call, MACOS_CG_ERROR_ILLEGAL_ARGUMENT);
                return;
            }

            const auto string = call.arg(2);
            if (string == 0)
            {
                window->title.clear();
                ui.sync_window(call.emu_ref, *window);
                succeed(call);
                return;
            }

            std::array<uint64_t, 4> fields{};
            if (!call.emu_ref.memory.try_read_memory(string, fields.data(), sizeof(fields)))
            {
                fail(call, MACOS_CG_ERROR_ILLEGAL_ARGUMENT);
                return;
            }

            constexpr uint64_t constant_string_utf8_info = 0x7c8;
            if (fields[1] != constant_string_utf8_info)
            {
                static std::set<uint64_t> reported{};
                if (reported.insert(fields[1]).second)
                {
                    call.emu_ref.log.warn("SLSSetWindowTitle: unhandled CFString form (info=0x%" PRIx64 "); title not updated\n",
                                          fields[1]);
                }

                succeed(call);
                return;
            }

            const auto data = fields[2];
            const auto length = std::min<uint64_t>(fields[3], 4096);
            if (data == 0 && length != 0)
            {
                fail(call, MACOS_CG_ERROR_ILLEGAL_ARGUMENT);
                return;
            }

            std::string utf8(length, '\0');
            if (length != 0 && !call.emu_ref.memory.try_read_memory(data, utf8.data(), utf8.size()))
            {
                fail(call, MACOS_CG_ERROR_ILLEGAL_ARGUMENT);
                return;
            }

            window->title = u8_to_u16(utf8);
            ui.sync_window(call.emu_ref, *window);
            succeed(call);
        }

        void sls_set_window_event_mask(const macos_native_call& call)
        {
            // SLSSetEventMask(cid, mask, wid) -- measured argument order.
            auto* window = window_for(call, call.arg(0), call.arg(2));
            if (window == nullptr)
            {
                fail(call, MACOS_CG_ERROR_ILLEGAL_ARGUMENT);
                return;
            }

            window->event_mask = call.arg(1);
            succeed(call);
        }

        void sls_set_window_client_perceived_type(const macos_native_call& call)
        {
            auto* window = window_for(call, call.arg(0), call.arg(1));
            if (window == nullptr)
            {
                fail(call, MACOS_CG_ERROR_ILLEGAL_ARGUMENT);
                return;
            }

            window->perceived_type = static_cast<uint32_t>(call.arg(2));
            succeed(call);
        }

        void sls_window_is_ordered_in(const macos_native_call& call)
        {
            const auto* window = window_for(call, call.arg(0), call.arg(1));
            const auto out = call.arg(2);
            if (window == nullptr || out == 0)
            {
                fail(call, MACOS_CG_ERROR_ILLEGAL_ARGUMENT);
                return;
            }

            const uint8_t ordered = window->ordered_in ? 1 : 0;
            if (!call.emu_ref.memory.try_write_memory(out, &ordered, sizeof(ordered)))
            {
                fail(call, MACOS_CG_ERROR_ILLEGAL_ARGUMENT);
                return;
            }

            succeed(call);
        }

        void slps_register_with_server(const macos_native_call& call)
        {
            // The 30283 CreateApplication handshake. The process is now known to the "server".
            ui_of(call).server.process_registered = true;
            succeed(call);
        }

        void slps_set_main_application_connection(const macos_native_call& call)
        {
            ui_of(call).server.main_application_connection = static_cast<uint32_t>(call.arg(0));
            succeed(call);
        }

        void sls_set_front_process_with_info(const macos_native_call& call)
        {
            ui_of(call).server.front_process_set = true;
            succeed(call);
        }

        // sogen's desktop has no dock: an empty rect and zeroed outs are the honest answer. The host's
        // measured values (dock hidden: rect {5,0,0,0}, orientation 3) describe a dock that exists,
        // which this emulator does not have.
        void sls_get_dock_rect_with_orientation(const macos_native_call& call)
        {
            const std::array<uint64_t, 6> zeroed{};
            const std::array<uint64_t, 1> zero{};
            const auto rect = call.arg(2);
            const auto orientation = call.arg(3);

            bool wrote = true;
            if (call.arg(1) != 0)
            {
                wrote = call.emu_ref.memory.try_write_memory(call.arg(1), zero.data(), sizeof(zero));
            }
            if (wrote && rect != 0)
            {
                wrote = call.emu_ref.memory.try_write_memory(rect, zeroed.data(), 32);
            }
            if (wrote && orientation != 0)
            {
                wrote = call.emu_ref.memory.try_write_memory(orientation, zero.data(), sizeof(zero));
            }

            if (!wrote)
            {
                fail(call, MACOS_CG_ERROR_ILLEGAL_ARGUMENT);
                return;
            }

            succeed(call);
        }

        void ca_render_server_get_max_renderable_io_surface_size(const macos_native_call& call)
        {
            // Measured outs on 25G76: {0x4000, 1}, {0x4000, 0}, {0x8000}; the routine returns true.
            const std::array<uint32_t, 2> first{0x4000, 1};
            const std::array<uint32_t, 2> second{0x4000, 0};
            const uint32_t third = 0x8000;

            bool wrote = true;
            if (call.arg(1) != 0)
            {
                wrote = call.emu_ref.memory.try_write_memory(call.arg(1), first.data(), sizeof(first));
            }
            if (wrote && call.arg(2) != 0)
            {
                wrote = call.emu_ref.memory.try_write_memory(call.arg(2), second.data(), sizeof(second));
            }
            if (wrote && call.arg(3) != 0)
            {
                wrote = call.emu_ref.memory.try_write_memory(call.arg(3), &third, sizeof(third));
            }

            call.ret(wrote ? 1 : 0);
        }

        void return_zero(const macos_native_call& call)
        {
            // SLSGetAppearanceThemeLegacy (0 = light theme, measured) and SLSCopyDisplayColorSpace (NULL
            // colorspace, tolerated by the caller, measured).
            call.ret(0);
        }

        void sls_get_last_used_keyboard_id(const macos_native_call& call)
        {
            call.ret(0x5c); // measured on the 25G76 host
        }

        void ca_render_server_get_needed_alignment(const macos_native_call& call)
        {
            call.ret(0x10); // measured on the 25G76 host
        }
    }

    std::optional<macos_cg_rect> read_cg_rect(macos_emulator& emu, const uint64_t address)
    {
        if (address == 0)
        {
            return std::nullopt;
        }

        std::array<double, 4> values{};
        if (!emu.memory.try_read_memory(address, values.data(), sizeof(values)))
        {
            return std::nullopt;
        }

        return macos_cg_rect{.x = values[0], .y = values[1], .width = values[2], .height = values[3]};
    }

    int32_t clamp_cg_dimension(const double value)
    {
        // CGFloat is a double and a guest may pass anything representable in one, NaN and infinity
        // included, so the conversion has to be defined rather than left to the target. arm64 happens to
        // saturate -- (int32_t)NaN is 0 and (int32_t)inf is INT32_MAX, both measured -- but wasm's
        // i32.trunc_f64_s *traps* on NaN, and sogen builds for wasm. That is what this guard is for, and
        // it is why no test on this host can distinguish it from the bare cast.
        if (std::isnan(value))
        {
            return 0;
        }

        if (value <= -static_cast<double>(MACOS_GUI_MAX_WINDOW_DIMENSION))
        {
            return -MACOS_GUI_MAX_WINDOW_DIMENSION;
        }

        if (value >= static_cast<double>(MACOS_GUI_MAX_WINDOW_DIMENSION))
        {
            return MACOS_GUI_MAX_WINDOW_DIMENSION;
        }

        return static_cast<int32_t>(value);
    }

    void register_skylight_first_pixel_routines(macos_native_dispatch& dispatch)
    {
        const std::string skylight{MACOS_SKYLIGHT_IMAGE_PATH};
        const std::string core_graphics{MACOS_CORE_GRAPHICS_IMAGE_PATH};

        dispatch.register_routine(skylight, "_SLSNewWindow", sls_new_window);
        dispatch.register_routine(skylight, "_SLSReleaseWindow", sls_release_window);
        dispatch.register_routine(skylight, "_SLSSetWindowLevel", sls_set_window_level);
        dispatch.register_routine(skylight, "_SLSGetWindowLevel", sls_get_window_level);
        dispatch.register_routine(skylight, "_SLSOrderWindow", sls_order_window);
        dispatch.register_routine(skylight, "_SLSMoveWindow", sls_move_window);
        dispatch.register_routine(skylight, "_SLSMainDisplayID", sls_main_display_id);
        dispatch.register_routine(skylight, "_SLSGetDisplayList", sls_get_display_list);
        dispatch.register_routine(skylight, "_SLSGetOnlineDisplayList", sls_get_display_list);
        dispatch.register_routine(skylight, "_SLSGetActiveDisplayList", sls_get_display_list);
        dispatch.register_routine(skylight, "_SLSGetDisplayBounds", sls_get_display_bounds);
        dispatch.register_routine(skylight, "_SLSGetWindowBounds", sls_get_window_bounds);
        dispatch.register_routine(skylight, "_SLSFlushWindowContentRegion", sls_flush_window_content_region);

        dispatch.register_routine(skylight, "_SLSSetWindowOpacity", sls_set_window_opacity);
        dispatch.register_routine(skylight, "_SLSSetWindowAlpha", accept_silently);
        dispatch.register_routine(skylight, "_SLSSetWindowShape", accept_silently);
        dispatch.register_routine(skylight, "_SLSSetWindowTags", accept_silently);
        dispatch.register_routine(skylight, "_SLSSetWindowResolution", accept_silently);
        dispatch.register_routine(skylight, "_SLSSetWindowTransform", accept_silently);
        dispatch.register_routine(skylight, "_SLSWindowSetShadowProperties", accept_silently);

        dispatch.register_routine(skylight, "_SLSDisableUpdate", accept_without_window);
        dispatch.register_routine(skylight, "_SLSReenableUpdate", accept_without_window);

        dispatch.register_routine(skylight, "_SLWindowContextCreate", sl_window_context_create);
        dispatch.register_routine(core_graphics, "_CGSNewRegionWithRect", cgs_new_region_with_rect);

        // The transaction-based path a 25G76 AppKit/SwiftUI app actually walks to first frame. Arg
        // shapes and return conventions are measured on the host; see
        dispatch.register_routine(skylight, "_SLSServerPort", sls_server_port);
        dispatch.register_routine(skylight, "_SLSTransactionCreate", sls_transaction_create);
        dispatch.register_routine(skylight, "_SLSTransactionSetWindowShape", sls_transaction_set_window_shape);
        dispatch.register_routine(skylight, "_SLSTransactionOrderWindowGroup", sls_transaction_order_window_group);
        dispatch.register_routine(skylight, "_SLSTransactionOrderWindowGroupFrontConditionally",
                                  sls_transaction_order_window_group_front_conditionally);
        dispatch.register_routine(skylight, "_SLSTransactionCommit", commit_transaction_call);
        dispatch.register_routine(skylight, "_SLSTransactionCommitUsingMethod", sls_transaction_commit_using_method);
        dispatch.register_routine(skylight, "_SLSNewWindowWithOpaqueShape", sls_new_window_with_opaque_shape);
        dispatch.register_routine(skylight, "_SLSNewWindowWithOpaqueShapeAndContext", sls_new_window_with_opaque_shape);
        dispatch.register_routine(skylight, "_SLSSetWindowLayerContext", sls_set_window_layer_context);
        dispatch.register_routine(skylight, "_SLSSetWindowTitle", sls_set_window_title);
        dispatch.register_routine(skylight, "_SLSSetEventMask", sls_set_window_event_mask);
        dispatch.register_routine(skylight, "_SLSSetWindowClientPerceivedType", sls_set_window_client_perceived_type);
        dispatch.register_routine(skylight, "_SLSWindowIsOrderedIn", sls_window_is_ordered_in);
        dispatch.register_routine(skylight, "_SLPSRegisterWithServer", slps_register_with_server);
        dispatch.register_routine(skylight, "_SLPSSetMainApplicationConnection", slps_set_main_application_connection);
        dispatch.register_routine(skylight, "_SLSSetFrontProcessWithInfo", sls_set_front_process_with_info);
        dispatch.register_routine(skylight, "_SLSGetDockRectWithOrientation", sls_get_dock_rect_with_orientation);
        dispatch.register_routine(skylight, "_SLSGetLastUsedKeyboardID", sls_get_last_used_keyboard_id);
        dispatch.register_routine(skylight, "_SLSGetAppearanceThemeLegacy", return_zero);
        dispatch.register_routine(skylight, "_SLSCopyDisplayColorSpace", return_zero);

        dispatch.register_routine(skylight, "_SLSSetGestureEventSubmask", accept_without_window);
        dispatch.register_routine(skylight, "_SLSCoalesceEventsInMask", accept_without_window);
        dispatch.register_routine(skylight, "_SLPSModifyConnectionNotifications", accept_without_window);
        dispatch.register_routine(skylight, "_SLPSSetNotifications", accept_without_window);
        dispatch.register_routine(skylight, "_SLSPackagesEnableConnectionWindowModificationNotifications", accept_without_window);
        dispatch.register_routine(skylight, "_SLSPackagesEnableConnectionOcclusionNotifications", accept_without_window);

        const std::string quartz_core{MACOS_QUARTZ_CORE_IMAGE_PATH};
        dispatch.register_routine(quartz_core, "_CARenderServerGetServerPort", ca_render_server_get_server_port);
        dispatch.register_routine(quartz_core, "_CARenderServerGetNeededAlignment", ca_render_server_get_needed_alignment);
        register_cf_container_routines(dispatch);

        dispatch.register_routine(quartz_core, "_CARenderServerGetMaxRenderableIOSurfaceSize",
                                  ca_render_server_get_max_renderable_io_surface_size);
    }
}
