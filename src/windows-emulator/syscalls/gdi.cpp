#include "../std_include.hpp"
#include "../debug_font.hpp"
#include "../emulator_utils.hpp"
#include "../syscall_utils.hpp"

#include <array>
#include <bit>
#include <limits>

// #define ENABLE_DXGK_LOGGING

namespace sogen
{

    namespace syscalls
    {
        namespace
        {
            constexpr uint8_t k_gdi_dc_type = 0x01;
            constexpr uint8_t k_gdi_region_type = 0x04;
            constexpr uint8_t k_gdi_bitmap_type = 0x05;
            constexpr uint8_t k_gdi_font_type = 0x0A;
            constexpr uint8_t k_gdi_brush_type = 0x10;
            constexpr uint8_t k_gdi_pen_type = 0x30;

            constexpr uint32_t k_gdi_dc_attr_size = 0x130;
            constexpr uint32_t k_gdi_brush_attr_size = 0x20;
            constexpr uint32_t k_gdi_pen_attr_size = 0x20;
            constexpr uint32_t k_gdi_bitmap_attr_size = 0x20;
            constexpr uint32_t k_gdi_font_attr_size = 0x40;
            constexpr uint32_t k_gdi_region_attr_size = 0x20;

            constexpr uint32_t k_gdi_dc_attr_hbrush_offset = 0x10;
            constexpr uint32_t k_gdi_dc_attr_hpen_offset = 0x18;
            constexpr uint32_t k_gdi_dc_attr_background_color_offset = 0x20; // DC_ATTR.crBackgroundClr (SetBkColor)
            constexpr uint32_t k_gdi_dc_attr_text_color_offset = 0x28;       // DC_ATTR.crForegroundClr (SetTextColor)
            constexpr uint32_t k_gdi_dc_attr_pt_current_offset = 0xD8;
            constexpr uint32_t k_gdi_dc_attr_font_offset = 0x128;

            constexpr uint32_t k_stock_white_brush_index = 0x00;
            constexpr uint32_t k_stock_black_pen_index = 0x06;
            constexpr uint32_t k_stock_system_font_index = 0x0D;
            constexpr uint32_t k_stock_default_gui_font_index = 0x11;
            constexpr uint32_t k_stock_dc_brush_index = 0x12;
            constexpr uint32_t k_stock_dc_pen_index = 0x13;
            constexpr uint32_t k_default_gui_font_data_index = 0x07;
            constexpr uint32_t k_user_client_drawing_brush_index = USER_NUM_SYSCOLORS;
            static_assert(k_user_client_drawing_brush_index < USER_SERVERINFO_BRUSH_SLOT_COUNT,
                          "client drawing brush index must be inside server info brush table");

            constexpr uint32_t k_logfontw_size = 0x5C;
            constexpr uint32_t k_logbrush_size = 0x0C;
            constexpr uint32_t k_logpen_size = 0x10;
            constexpr uint32_t k_bitmap_size = 0x18;

            constexpr uint32_t k_textmetric_size = 0x3C;
            constexpr uint32_t k_default_font_height = 16;
            constexpr uint32_t k_default_font_ascent = 12;
            constexpr uint32_t k_default_font_descent = 4;
            constexpr uint32_t k_default_font_width = 8;
            constexpr uint32_t k_default_font_weight = 400;
            constexpr int k_text_cell_width = static_cast<int>(k_default_font_width);
            constexpr int k_text_cell_height = static_cast<int>(k_default_font_height);
            constexpr int k_text_glyph_y_offset = (k_text_cell_height - debug_font::glyph_height) / 2;

            constexpr uint32_t k_default_dpi = 96;
            constexpr uint32_t k_default_width = 1920;
            constexpr uint32_t k_default_height = 1080;
            constexpr uint32_t k_default_bitmap_fill = 0xFFF5F5F5u;

            constexpr uint32_t k_gdi_first_dynamic_handle = 0x2000;
            constexpr uint32_t k_gdi_default_cookie = STATUS_WAIT_1;

            constexpr int16_t k_gdi_batch_cmd_text_out = 2;
            constexpr int16_t k_gdi_batch_cmd_poly_pat_blt = 1;
            constexpr uint32_t k_gdibs_no_rect = 0x80000000u;
            constexpr size_t k_gdi_poly_pat_blt_rect_offset = 0x30;
            constexpr size_t k_gdi_pat_rect_size = 0x18;

            constexpr std::array<uint32_t, USER_NUM_SYSCOLORS> k_default_system_colors = {
                0x00C8D0D4u, // COLOR_SCROLLBAR
                0x00000000u, // COLOR_BACKGROUND
                0x00D1B499u, // COLOR_ACTIVECAPTION
                0x00DBCDBFu, // COLOR_INACTIVECAPTION
                0x00F0F0F0u, // COLOR_MENU
                0x00FFFFFFu, // COLOR_WINDOW
                0x00000000u, // COLOR_WINDOWFRAME
                0x00000000u, // COLOR_MENUTEXT
                0x00000000u, // COLOR_WINDOWTEXT
                0x00FFFFFFu, // COLOR_CAPTIONTEXT
                0x00B9D1EAu, // COLOR_ACTIVEBORDER
                0x00F2E4D7u, // COLOR_INACTIVEBORDER
                0x00F0F0F0u, // COLOR_APPWORKSPACE
                0x00D77800u, // COLOR_HIGHLIGHT
                0x00FFFFFFu, // COLOR_HIGHLIGHTTEXT
                0x00F0F0F0u, // COLOR_BTNFACE
                0x00A0A0A0u, // COLOR_BTNSHADOW
                0x006D6D6Du, // COLOR_GRAYTEXT
                0x00000000u, // COLOR_BTNTEXT
                0x00000000u, // COLOR_INACTIVECAPTIONTEXT
                0x00FFFFFFu, // COLOR_BTNHIGHLIGHT
                0x00696969u, // COLOR_3DDKSHADOW
                0x00E3E3E3u, // COLOR_3DLIGHT
                0x00000000u, // COLOR_INFOTEXT
                0x00E1FFFFu, // COLOR_INFOBK
                0x00000000u, // COLOR_HOTLIGHT
                0x00CC6600u, // COLOR_GRADIENTACTIVECAPTION
                0x00F2E4D7u, // COLOR_GRADIENTINACTIVECAPTION
                0x00F0F0F0u, // COLOR_MENUHILIGHT
                0x00F0F0F0u, // COLOR_MENUBAR
                0x00FFFFFFu, // COLOR_DESKTOP
            };

            struct gdi_batch_header
            {
                int16_t size{};
                int16_t cmd{};
            };

            struct gdi_batch_text_out
            {
                gdi_batch_header header{};
                COLORREF foreground{};
                COLORREF background{};
                LONG bk_mode{};
                ULONG foreground_ul{};
                ULONG background_ul{};
                int32_t x{};
                int32_t y{};
                UINT options{};
                RECT rect{};
                DWORD code_page{};
                UINT count{};
                UINT dx_size{};
                uint64_t font{};
                FLONG text_align{};
                POINTL viewport_org{};
                union
                {
                    std::array<WCHAR, 2> string;
                    std::array<ULONG, 1> buffer;
                };
            };

            struct gdi_batch_pat_rect
            {
                LONG x{};
                LONG y{};
                LONG width{};
                LONG height{};
                uint64_t brush{};
            };

            struct gdi_batch_poly_pat_blt
            {
                gdi_batch_header header{};
                DWORD rop{};
                DWORD mode{};
                DWORD count{};
                COLORREF foreground{};
                COLORREF background{};
                COLORREF brush_color{};
                ULONG foreground_ul{};
                ULONG background_ul{};
                ULONG brush_ul{};
                POINTL viewport_org{};
                std::array<gdi_batch_pat_rect, 1> rects{};
            };

            constexpr uint32_t k_dxgk_adapter_count = 1;
            constexpr uint32_t k_dxgk_adapter_handle = 0x4000;
            constexpr uint32_t k_dxgk_device_handle = 0x5000;
            constexpr uint32_t k_dxgk_context_handle = 0x6000;
            constexpr uint32_t k_dxgk_shared_primary_handle = 0x7000;
            constexpr LUID k_dxgk_adapter_luid = {0x1000, 0};
            constexpr uint32_t k_dxgk_adapter_source_count = 1;
            constexpr size_t k_dxgk_command_buffer_size = 0x1000;
            constexpr uint64_t k_dxgk_dedicated_video_memory_size = 4ull * 1024 * 1024 * 1024;
            constexpr uint64_t k_dxgk_shared_system_memory_size = 8ull * 1024 * 1024 * 1024;
            constexpr uint32_t k_dxgk_fake_vendor_id = 0x10DE;
            constexpr uint32_t k_dxgk_fake_device_id = 0x1C03;
            constexpr uint32_t k_dxgk_fake_revision_id = 0xA1;
            constexpr uint32_t k_dxgk_open_resource_resource_private_size = 0x18;
            constexpr uint32_t k_dxgk_open_resource_allocation_private_size = 0x18;
            constexpr uint32_t k_dxgk_open_resource_descriptor_size = 0x80;
            constexpr uint32_t k_dxgk_open_resource_total_private_size =
                k_dxgk_open_resource_allocation_private_size + k_dxgk_open_resource_descriptor_size;
            constexpr GUID k_dxgk_adapter_guid = {0x5b45201d, 0xf2f2, 0x4f3b, {0x85, 0xbb, 0x30, 0xff, 0x1f, 0x95, 0x35, 0x99}};

            uint64_t ensure_gdi_shared_table(const syscall_context& c)
            {
                uint64_t table = 0;
                c.proc.peb64.access([&](const PEB64& peb) { table = peb.GdiSharedHandleTable; });

                if (table != 0)
                {
                    return table;
                }

                const auto shared = c.proc.base_allocator.reserve<GDI_SHARED_MEMORY64>();
                shared.access([](GDI_SHARED_MEMORY64& mem) { mem = {}; });
                table = shared.value();

                c.proc.peb64.access([&](PEB64& peb) { peb.GdiSharedHandleTable = table; });

                if (c.proc.peb32 && table <= std::numeric_limits<uint32_t>::max())
                {
                    c.proc.peb32->access([&](PEB32& peb32) { peb32.GdiSharedHandleTable = static_cast<uint32_t>(table); });
                }

                return table;
            }

            uint64_t ensure_gdi_cookie(const syscall_context& c)
            {
                uint64_t cookie = 0;
                c.proc.peb64.access([&](PEB64& peb) {
                    if (peb.GdiDCAttributeList == 0)
                    {
                        peb.GdiDCAttributeList = k_gdi_default_cookie;
                    }

                    cookie = peb.GdiDCAttributeList;
                });

                if (c.proc.peb32)
                {
                    c.proc.peb32->access([&](PEB32& peb32) {
                        if (peb32.GdiDCAttributeList == 0)
                        {
                            peb32.GdiDCAttributeList = static_cast<uint32_t>(cookie);
                        }
                    });
                }

                return cookie;
            }

            uint64_t encode_gdi_user_pointer(const syscall_context& c, const uint64_t pointer, const uint64_t cookie)
            {
                if (pointer == 0)
                {
                    return 0;
                }

                if (c.proc.is_wow64_process)
                {
                    if (pointer > std::numeric_limits<uint32_t>::max())
                    {
                        return 0;
                    }

                    // wow64 gdi32full decodes low 32 bits: ror.d(encoded, 32 - (cookie & 0x1f)) ^ cookie
                    const auto pointer32 = static_cast<uint32_t>(pointer);
                    const auto cookie32 = static_cast<uint32_t>(cookie);
                    const uint32_t decoded = pointer32 ^ cookie32;
                    const auto rotate = static_cast<int>(32 - (cookie32 & 0x1F));
                    return std::rotl(decoded, rotate);
                }

                // x64 gdi32full decodes UserPointer: ror.q(encoded, 64 - (cookie & 0x3f)) ^ cookie
                const uint64_t decoded = pointer ^ cookie;
                const auto rotate = static_cast<int>(64 - (cookie & 0x3F));
                return std::rotl(decoded, rotate);
            }

            uint64_t read_gdi_shared_value(const syscall_context& c, const uint64_t offset)
            {
                const auto table = ensure_gdi_shared_table(c);
                if (table == 0)
                {
                    return 0;
                }

                uint64_t value = 0;
                c.emu.read_memory(table + offset, &value, sizeof(value));
                return value;
            }

            uint64_t read_gdi_object_slot(const syscall_context& c, const uint32_t index)
            {
                if (index >= 0x20)
                {
                    return 0;
                }

                return read_gdi_shared_value(c, offsetof(GDI_SHARED_MEMORY64, Objects) + (sizeof(uint64_t) * index));
            }

            bool write_gdi_object_slot(const syscall_context& c, const uint32_t index, const uint64_t value)
            {
                if (index >= 0x20)
                {
                    return false;
                }

                const auto table = ensure_gdi_shared_table(c);
                if (table == 0)
                {
                    return false;
                }

                const auto slot = table + offsetof(GDI_SHARED_MEMORY64, Objects) + (sizeof(uint64_t) * index);
                c.emu.write_memory(slot, &value, sizeof(value));
                return true;
            }

            uint64_t read_gdi_data_slot(const syscall_context& c, const uint32_t index)
            {
                if (index >= 0x200)
                {
                    return 0;
                }

                return read_gdi_shared_value(c, offsetof(GDI_SHARED_MEMORY64, Data) + (sizeof(uint64_t) * index));
            }

            bool write_gdi_data_slot(const syscall_context& c, const uint32_t index, const uint64_t value)
            {
                if (index >= 0x200)
                {
                    return false;
                }

                const auto table = ensure_gdi_shared_table(c);
                if (table == 0)
                {
                    return false;
                }

                const auto slot = table + offsetof(GDI_SHARED_MEMORY64, Data) + (sizeof(uint64_t) * index);
                c.emu.write_memory(slot, &value, sizeof(value));
                return true;
            }

            uint64_t allocate_gdi_user_block(const syscall_context& c, const uint32_t size)
            {
                const auto aligned_size = static_cast<size_t>(page_align_up(size));
                const uint64_t base =
                    c.win_emu.memory.allocate_memory(aligned_size, memory_permission::read_write, false, DEFAULT_ALLOCATION_ADDRESS_32BIT);
                if (base == 0)
                {
                    return 0;
                }

                if (base > std::numeric_limits<uint32_t>::max())
                {
                    c.win_emu.memory.release_memory(base, 0);
                    return 0;
                }

                std::vector<uint8_t> zeroed(size, 0);
                c.emu.write_memory(base, zeroed.data(), zeroed.size());
                return base;
            }

            uint32_t allocate_gdi_handle(const syscall_context& c, const uint8_t type, const uint64_t user_ptr, const uint64_t object_ptr)
            {
                const auto table = ensure_gdi_shared_table(c);
                if (table == 0)
                {
                    return 0;
                }

                const uint64_t cookie = ensure_gdi_cookie(c);
                const uint64_t encoded_user_ptr = encode_gdi_user_pointer(c, user_ptr, cookie);

                for (uint32_t index = k_gdi_first_dynamic_handle; index < GDI_MAX_HANDLE_COUNT; ++index)
                {
                    const uint64_t entry_addr = table + (static_cast<uint64_t>(index) * sizeof(GDI_HANDLE_ENTRY64));
                    const emulator_object<GDI_HANDLE_ENTRY64> entry_obj{c.emu, entry_addr};
                    const auto current = entry_obj.read();

                    if (current.Type != 0)
                    {
                        continue;
                    }

                    auto generation = static_cast<uint16_t>((current.Unique + 0x100u) & 0xFF00u);
                    if (generation == 0)
                    {
                        generation = 0x100;
                    }

                    const auto unique = static_cast<uint16_t>(generation | (type & 0x7Fu));
                    const uint32_t handle_value = (static_cast<uint32_t>(unique) << 16) | index;

                    entry_obj.access([&](GDI_HANDLE_ENTRY64& writable) {
                        writable = {};
                        writable.Object = object_ptr;
                        writable.Owner.Value = 0;
                        writable.Unique = unique;
                        writable.Type = type;
                        writable.Flags = 0;
                        writable.UserPointer = encoded_user_ptr;
                    });

                    return handle_value;
                }

                return 0;
            }

            bool read_gdi_entry_for_handle(const syscall_context& c, uint32_t handle_value, GDI_HANDLE_ENTRY64& entry,
                                           uint64_t& entry_addr);

            uint32_t allocate_gdi_object(const syscall_context& c, const uint8_t type, const uint32_t attr_size)
            {
                const uint64_t attr = allocate_gdi_user_block(c, attr_size);
                if (attr == 0)
                {
                    return 0;
                }

                const uint64_t user_ptr = (type == k_gdi_font_type) ? 0 : attr;
                return allocate_gdi_handle(c, type, user_ptr, attr);
            }

            bool get_gdi_object_address(const syscall_context& c, const uint32_t handle_value, const uint8_t expected_type, uint64_t& addr)
            {
                GDI_HANDLE_ENTRY64 entry{};
                uint64_t entry_addr = 0;
                if (!read_gdi_entry_for_handle(c, handle_value, entry, entry_addr) || entry.Type != expected_type)
                {
                    return false;
                }

                addr = entry.Object;
                return addr != 0;
            }

            uint32_t window_surface_fill_color(const syscall_context& c, const window& win);

            // Resolves the bitmap surface a DC draws into, plus the offset from DC-local coordinates to surface
            // coordinates, and the host window handle the surface should be presented to. For a window DC this is
            // the top-level host window's persistent surface; child controls draw into it at their client offset.
            gdi_bitmap_surface* resolve_dc_surface(const syscall_context& c, const hdc dc, int32_t& origin_x, int32_t& origin_y,
                                                   uint32_t& present_handle)
            {
                origin_x = 0;
                origin_y = 0;
                present_handle = 0;

                const auto dc_it = c.proc.gdi_dc_states.find(static_cast<uint32_t>(dc));
                if (dc_it == c.proc.gdi_dc_states.end())
                {
                    return nullptr;
                }

                const auto& dc_state = dc_it->second;
                if (dc_state.selected_bitmap != 0)
                {
                    const auto bmp_it = c.proc.gdi_bitmap_surfaces.find(dc_state.selected_bitmap);
                    if (bmp_it == c.proc.gdi_bitmap_surfaces.end())
                    {
                        return nullptr;
                    }

                    present_handle = static_cast<uint32_t>(dc_state.target_window);
                    return &bmp_it->second;
                }

                if (dc_state.target_window == 0)
                {
                    return nullptr;
                }

                const auto* win = c.proc.windows.get(dc_state.target_window);
                if (!win)
                {
                    return nullptr;
                }

                // Walk up to the owning top-level window, accumulating each child's client-area offset.
                int32_t off_x = 0;
                int32_t off_y = 0;
                while (win && (win->style & WS_CHILD) != 0)
                {
                    off_x += win->x;
                    off_y += win->y;
                    win = win->parent_handle != 0 ? c.proc.windows.get(win->parent_handle) : nullptr;
                }

                if (!win || !win->host_surface_window || win->width <= 0 || win->height <= 0)
                {
                    return nullptr;
                }

                const auto top_handle = static_cast<uint32_t>(win->handle);
                const auto width = static_cast<uint32_t>(win->width);
                const auto height = static_cast<uint32_t>(win->height);

                auto& surface = c.proc.gdi_window_surfaces[top_handle];
                if (surface.width != width || surface.height != height || surface.pixels.size() != static_cast<size_t>(width) * height)
                {
                    surface.width = width;
                    surface.height = height;
                    surface.pixels.assign(static_cast<size_t>(width) * height, window_surface_fill_color(c, *win));
                }

                origin_x = off_x;
                origin_y = off_y;
                present_handle = top_handle;
                return &surface;
            }

            bool get_dc_state_and_surface(const syscall_context& c, const hdc dc, gdi_dc_state*& dc_state, gdi_bitmap_surface*& surface,
                                          int32_t& origin_x, int32_t& origin_y)
            {
                dc_state = nullptr;
                surface = nullptr;
                origin_x = 0;
                origin_y = 0;

                const auto dc_it = c.proc.gdi_dc_states.find(static_cast<uint32_t>(dc));
                if (dc_it == c.proc.gdi_dc_states.end())
                {
                    return false;
                }

                dc_state = &dc_it->second;
                uint32_t present_handle = 0;
                surface = resolve_dc_surface(c, dc, origin_x, origin_y, present_handle);
                return surface != nullptr;
            }

            void set_surface_pixel(gdi_bitmap_surface& surface, const int x, const int y, const uint32_t color)
            {
                if (x < 0 || y < 0 || x >= static_cast<int>(surface.width) || y >= static_cast<int>(surface.height))
                {
                    return;
                }

                surface.pixels[static_cast<size_t>(y) * surface.width + static_cast<size_t>(x)] = color;
            }

            uint32_t colorref_to_bgra(const uint32_t colorref)
            {
                return 0xFF000000u | ((colorref & 0x000000FFu) << 16) | (colorref & 0x0000FF00u) | ((colorref & 0x00FF0000u) >> 16);
            }

            bool set_gdi_object_color(const syscall_context& c, const uint32_t handle_value, const uint8_t expected_type,
                                      const uint32_t colorref)
            {
                uint64_t attr = 0;
                if (!get_gdi_object_address(c, handle_value, expected_type, attr))
                {
                    return false;
                }

                c.emu.write_memory(attr + sizeof(uint32_t), &colorref, sizeof(colorref));
                return true;
            }

            uint32_t get_brush_color(const syscall_context& c, const uint32_t brush_handle)
            {
                uint64_t brush_attr = 0;
                if (!get_gdi_object_address(c, brush_handle, k_gdi_brush_type, brush_attr))
                {
                    return 0xFFFFFFFFu;
                }

                uint32_t colorref = 0;
                c.win_emu.memory.try_read_memory(brush_attr + sizeof(uint32_t), &colorref, sizeof(colorref));
                return colorref_to_bgra(colorref);
            }

            uint32_t get_dc_pen_color(const syscall_context& c, const hdc dc)
            {
                uint64_t dc_attr = 0;
                if (!get_gdi_object_address(c, static_cast<uint32_t>(dc), k_gdi_dc_type, dc_attr))
                {
                    return 0xFF000000u;
                }

                uint32_t pen_handle = 0;
                c.win_emu.memory.try_read_memory(dc_attr + k_gdi_dc_attr_hpen_offset, &pen_handle, sizeof(pen_handle));
                if (pen_handle == 0)
                {
                    return 0xFF000000u;
                }

                uint64_t pen_attr = 0;
                if (!get_gdi_object_address(c, pen_handle, k_gdi_pen_type, pen_attr))
                {
                    return 0xFF000000u;
                }

                uint32_t colorref = 0;
                c.win_emu.memory.try_read_memory(pen_attr + sizeof(uint32_t), &colorref, sizeof(colorref));
                return colorref_to_bgra(colorref);
            }

            void set_dc_current_point(const syscall_context& c, const hdc dc, const int32_t x, const int32_t y)
            {
                if (auto it = c.proc.gdi_dc_states.find(static_cast<uint32_t>(dc)); it != c.proc.gdi_dc_states.end())
                {
                    it->second.current_x = x;
                    it->second.current_y = y;
                }

                uint64_t dc_attr = 0;
                if (get_gdi_object_address(c, static_cast<uint32_t>(dc), k_gdi_dc_type, dc_attr))
                {
                    c.emu.write_memory(dc_attr + k_gdi_dc_attr_pt_current_offset, &x, sizeof(x));
                    c.emu.write_memory(dc_attr + k_gdi_dc_attr_pt_current_offset + sizeof(int32_t), &y, sizeof(y));
                }
            }

            uint32_t get_dc_brush_color(const syscall_context& c, const hdc dc)
            {
                uint64_t dc_attr = 0;
                if (!get_gdi_object_address(c, static_cast<uint32_t>(dc), k_gdi_dc_type, dc_attr))
                {
                    return 0xFFFFFFFFu;
                }

                uint32_t brush_handle = 0;
                c.win_emu.memory.try_read_memory(dc_attr + k_gdi_dc_attr_hbrush_offset, &brush_handle, sizeof(brush_handle));
                if (brush_handle == 0)
                {
                    return 0xFFFFFFFFu;
                }

                return get_brush_color(c, brush_handle);
            }

            // Reads a COLORREF directly out of the DC_ATTR (the user-mode block gdi32 updates on
            // SetTextColor / SetBkColor without a syscall), returning `fallback` if the DC is unknown.
            uint32_t get_dc_attr_color(const syscall_context& c, const hdc dc, const uint32_t offset, const uint32_t fallback)
            {
                uint64_t dc_attr = 0;
                if (!get_gdi_object_address(c, static_cast<uint32_t>(dc), k_gdi_dc_type, dc_attr))
                {
                    return fallback;
                }

                uint32_t colorref = 0;
                c.win_emu.memory.try_read_memory(dc_attr + offset, &colorref, sizeof(colorref));
                return colorref_to_bgra(colorref);
            }

            uint32_t get_dc_text_color(const syscall_context& c, const hdc dc)
            {
                return get_dc_attr_color(c, dc, k_gdi_dc_attr_text_color_offset, 0xFF000000u);
            }

            uint32_t get_dc_background_color(const syscall_context& c, const hdc dc)
            {
                return get_dc_attr_color(c, dc, k_gdi_dc_attr_background_color_offset, 0xFFFFFFFFu);
            }

            // The base fill for a freshly (re)created top-level window surface, matching what WM_ERASEBKGND
            // would paint: the window class's background brush. hbrBackground is either a (COLOR_* + 1)
            // system-color encoding (small integer) or a real GDI brush handle. A class brush of 0 means
            // "no background", which we keep white, except for dialogs (#32770) whose face is the gray 3D
            // face (COLOR_BTNFACE) painted by DefDlgProc.
            uint32_t window_surface_fill_color(const syscall_context& c, const window& win)
            {
                constexpr uint32_t white_fill = 0xFFFFFFFFu;
                constexpr uint32_t color_btnface_index = 15; // COLOR_BTNFACE in k_default_system_colors

                uint64_t background = 0;
                if (const auto it = c.proc.classes.find(win.class_name); it != c.proc.classes.end())
                {
                    background = it->second.wnd_class.hbrBackground;
                }

                if (background == 0)
                {
                    if (win.is_dialog())
                    {
                        return colorref_to_bgra(k_default_system_colors[color_btnface_index]);
                    }

                    return white_fill;
                }

                if (background <= USER_NUM_SYSCOLORS)
                {
                    return colorref_to_bgra(k_default_system_colors[static_cast<size_t>(background - 1)]);
                }

                return get_brush_color(c, static_cast<uint32_t>(background));
            }

            void draw_line(gdi_bitmap_surface& surface, int x0, int y0, const int x1, const int y1, const uint32_t color)
            {
                int dx = std::abs(x1 - x0);
                const int sx = x0 < x1 ? 1 : -1;
                int dy = -std::abs(y1 - y0);
                const int sy = y0 < y1 ? 1 : -1;
                int err = dx + dy;

                for (;;)
                {
                    set_surface_pixel(surface, x0, y0, color);
                    if (x0 == x1 && y0 == y1)
                    {
                        break;
                    }

                    const int e2 = err * 2;
                    if (e2 >= dy)
                    {
                        err += dy;
                        x0 += sx;
                    }
                    if (e2 <= dx)
                    {
                        err += dx;
                        y0 += sy;
                    }
                }
            }

            bool point_in_rect(const int x, const int y, const RECT* clip)
            {
                return clip == nullptr || (x >= clip->left && x < clip->right && y >= clip->top && y < clip->bottom);
            }

            void set_text_pixel(gdi_bitmap_surface& surface, const int x, const int y, const uint32_t color, const RECT* clip)
            {
                if (point_in_rect(x, y, clip))
                {
                    set_surface_pixel(surface, x, y, color);
                }
            }

            void draw_text_glyph(gdi_bitmap_surface& surface, const int x, const int y, char32_t codepoint, const uint32_t color,
                                 const RECT* clip)
            {
                if (codepoint < debug_font::first_codepoint || codepoint > debug_font::last_codepoint)
                {
                    codepoint = U'?';
                }

                const auto& glyph = debug_font::glyphs[static_cast<size_t>(codepoint - debug_font::first_codepoint)];
                for (int row = 0; row < debug_font::glyph_height; ++row)
                {
                    const uint8_t bits = glyph[static_cast<size_t>(row)];
                    for (int col = 0; col < debug_font::glyph_width; ++col)
                    {
                        if ((bits & (0x01u << col)) != 0)
                        {
                            set_text_pixel(surface, x + col, y + k_text_glyph_y_offset + row, color, clip);
                        }
                    }
                }
            }

            void draw_text(gdi_bitmap_surface& surface, const int x, const int y, const std::u16string_view text, const uint32_t color,
                           const RECT* clip = nullptr, const uint32_t* dx = nullptr)
            {
                int pen_x = x;
                for (size_t i = 0; i < text.size(); ++i)
                {
                    draw_text_glyph(surface, pen_x, y, static_cast<char32_t>(text[i]), color, clip);
                    pen_x += dx != nullptr ? static_cast<int32_t>(dx[i]) : k_text_cell_width;
                }
            }

            void fill_rect(gdi_bitmap_surface& surface, const int left, const int top, const int right, const int bottom,
                           const uint32_t color)
            {
                for (int y = top; y < bottom; ++y)
                {
                    for (int x = left; x < right; ++x)
                    {
                        set_surface_pixel(surface, x, y, color);
                    }
                }
            }

            template <typename Batch>
            bool flush_gdi_text_batch(const syscall_context& c, const Batch& batch)
            {
                const auto batch_offset = batch.Offset & ~k_gdibs_no_rect;
                if (batch_offset == 0 || batch.HDC == 0 || batch_offset > sizeof(batch.Buffer))
                {
                    return true;
                }

                const auto dc = static_cast<hdc>(batch.HDC);
                gdi_dc_state* dc_state = nullptr;
                gdi_bitmap_surface* surface = nullptr;
                int32_t origin_x = 0;
                int32_t origin_y = 0;
                if (!get_dc_state_and_surface(c, dc, dc_state, surface, origin_x, origin_y) || !surface)
                {
                    return true;
                }

                const auto* bytes = reinterpret_cast<const uint8_t*>(batch.Buffer);
                size_t offset = 0;
                while (offset + sizeof(gdi_batch_header) <= batch_offset)
                {
                    const auto* header = reinterpret_cast<const gdi_batch_header*>(bytes + offset);
                    if (header->size <= 0 || offset + static_cast<size_t>(header->size) > batch_offset)
                    {
                        break;
                    }

                    if (header->cmd == k_gdi_batch_cmd_text_out &&
                        static_cast<size_t>(header->size) >= offsetof(gdi_batch_text_out, string))
                    {
                        const auto* text_out = reinterpret_cast<const gdi_batch_text_out*>(header);
                        RECT clip_rect{.left = text_out->rect.left + origin_x,
                                       .top = text_out->rect.top + origin_y,
                                       .right = text_out->rect.right + origin_x,
                                       .bottom = text_out->rect.bottom + origin_y};
                        const auto has_rect = (text_out->options & k_gdibs_no_rect) == 0;
                        if ((text_out->options & ETO_OPAQUE) != 0 && (text_out->options & k_gdibs_no_rect) == 0)
                        {
                            fill_rect(*surface, clip_rect.left, clip_rect.top, clip_rect.right, clip_rect.bottom,
                                      colorref_to_bgra(text_out->background));
                        }

                        // The guest controls the batch contents: the variable payload after
                        // `string` holds the dx array (dx_size bytes) followed by the text
                        // (count * sizeof(char16_t) bytes). Validate both fit inside this
                        // command before dereferencing, otherwise a malformed count/dx_size
                        // would read past header->size and beyond batch.Buffer.
                        const auto payload_capacity = static_cast<size_t>(header->size) - offsetof(gdi_batch_text_out, string);
                        const auto dx_bytes = static_cast<size_t>(text_out->dx_size);
                        const auto text_bytes = static_cast<size_t>(text_out->count) * sizeof(char16_t);
                        if (dx_bytes <= payload_capacity && text_bytes <= payload_capacity - dx_bytes)
                        {
                            const auto* dx = reinterpret_cast<const uint32_t*>(text_out->string.data());
                            const auto* text =
                                reinterpret_cast<const char16_t*>(text_out->string.data() + text_out->dx_size / sizeof(char16_t));
                            draw_text(*surface, text_out->x + text_out->viewport_org.x + origin_x,
                                      text_out->y + text_out->viewport_org.y + origin_y, std::u16string_view(text, text_out->count),
                                      colorref_to_bgra(text_out->foreground),
                                      has_rect && (text_out->options & ETO_CLIPPED) != 0 ? &clip_rect : nullptr,
                                      text_out->dx_size >= text_out->count * sizeof(uint32_t) ? dx : nullptr);
                        }
                    }
                    else if (header->cmd == k_gdi_batch_cmd_poly_pat_blt &&
                             static_cast<size_t>(header->size) >= k_gdi_poly_pat_blt_rect_offset)
                    {
                        uint32_t declared_count = 0;
                        std::memcpy(&declared_count, bytes + offset + 0x0C, sizeof(declared_count));

                        const auto available_rects =
                            (static_cast<size_t>(header->size) - k_gdi_poly_pat_blt_rect_offset) / k_gdi_pat_rect_size;
                        const auto rect_count = declared_count == 0 ? available_rects : std::min<size_t>(declared_count, available_rects);
                        for (size_t i = 0; i < rect_count; ++i)
                        {
                            const auto rect_offset = offset + k_gdi_poly_pat_blt_rect_offset + (i * k_gdi_pat_rect_size);
                            gdi_batch_pat_rect rect{};
                            uint64_t brush = 0;
                            std::memcpy(&rect, bytes + rect_offset, sizeof(rect));
                            brush = rect.brush;

                            const auto color = brush != 0 ? get_brush_color(c, static_cast<uint32_t>(brush)) : get_dc_brush_color(c, dc);
                            fill_rect(*surface, rect.x + origin_x, rect.y + origin_y, rect.x + rect.width + origin_x,
                                      rect.y + rect.height + origin_y, color);
                        }
                    }

                    offset += static_cast<size_t>(header->size);
                }

                return true;
            }

            void seed_user_system_color_brushes(const syscall_context& c)
            {
                constexpr size_t k_brush_seed_count =
                    USER_NUM_SYSCOLORS < USER_SERVERINFO_BRUSH_SLOT_COUNT ? USER_NUM_SYSCOLORS : USER_SERVERINFO_BRUSH_SLOT_COUNT;

                std::array<uint64_t, k_brush_seed_count> system_brushes{};
                uint64_t client_drawing_brush = 0;
                bool needs_seed = false;

                c.proc.user_handles.get_server_info().access([&](const USER_SERVERINFO& server_info) {
                    for (size_t i = 0; i < system_brushes.size(); ++i)
                    {
                        system_brushes[i] = server_info.ahbrSystem[i];
                        if (system_brushes[i] == 0)
                        {
                            needs_seed = true;
                        }
                    }

                    client_drawing_brush = server_info.ahbrSystem[k_user_client_drawing_brush_index];
                    if (client_drawing_brush == 0)
                    {
                        needs_seed = true;
                    }
                });

                if (!needs_seed)
                {
                    return;
                }

                for (auto& brush : system_brushes)
                {
                    if (brush != 0)
                    {
                        continue;
                    }

                    const uint32_t handle = allocate_gdi_object(c, k_gdi_brush_type, k_gdi_brush_attr_size);
                    if (handle != 0)
                    {
                        brush = handle;
                    }
                }

                const auto system_brush_count = std::min(system_brushes.size(), k_default_system_colors.size());
                for (size_t i = 0; i < system_brush_count; ++i)
                {
                    if (system_brushes[i] != 0)
                    {
                        set_gdi_object_color(c, static_cast<uint32_t>(system_brushes[i]), k_gdi_brush_type, k_default_system_colors[i]);
                    }
                }

                if (client_drawing_brush == 0)
                {
                    const uint32_t handle = allocate_gdi_object(c, k_gdi_brush_type, k_gdi_brush_attr_size);
                    if (handle != 0)
                    {
                        client_drawing_brush = handle;
                    }
                }
                if (client_drawing_brush != 0)
                {
                    set_gdi_object_color(c, static_cast<uint32_t>(client_drawing_brush), k_gdi_brush_type, k_default_system_colors[15]);
                }

                c.proc.user_handles.get_server_info().access([&](USER_SERVERINFO& server_info) {
                    for (size_t i = 0; i < system_brushes.size(); ++i)
                    {
                        if (server_info.ahbrSystem[i] == 0)
                        {
                            server_info.ahbrSystem[i] = system_brushes[i];
                        }
                    }

                    if (server_info.ahbrSystem[k_user_client_drawing_brush_index] == 0)
                    {
                        server_info.ahbrSystem[k_user_client_drawing_brush_index] = client_drawing_brush;
                    }
                });
            }

            void seed_gdi_stock_objects(const syscall_context& c)
            {
                struct stock_seed
                {
                    uint32_t index;
                    uint8_t type;
                    uint32_t attr_size;
                    bool mirror_to_default_gui_slot;
                };

                constexpr std::array<stock_seed, 6> seeds = {
                    stock_seed{
                        .index = k_stock_white_brush_index,
                        .type = k_gdi_brush_type,
                        .attr_size = k_gdi_brush_attr_size,
                        .mirror_to_default_gui_slot = false,
                    },
                    stock_seed{
                        .index = k_stock_black_pen_index,
                        .type = k_gdi_pen_type,
                        .attr_size = k_gdi_pen_attr_size,
                        .mirror_to_default_gui_slot = false,
                    },
                    stock_seed{
                        .index = k_stock_system_font_index,
                        .type = k_gdi_font_type,
                        .attr_size = k_gdi_font_attr_size,
                        .mirror_to_default_gui_slot = false,
                    },
                    stock_seed{
                        .index = k_stock_default_gui_font_index,
                        .type = k_gdi_font_type,
                        .attr_size = k_gdi_font_attr_size,
                        .mirror_to_default_gui_slot = true,
                    },
                    stock_seed{
                        .index = k_stock_dc_brush_index,
                        .type = k_gdi_brush_type,
                        .attr_size = k_gdi_brush_attr_size,
                        .mirror_to_default_gui_slot = false,
                    },
                    stock_seed{
                        .index = k_stock_dc_pen_index,
                        .type = k_gdi_pen_type,
                        .attr_size = k_gdi_pen_attr_size,
                        .mirror_to_default_gui_slot = false,
                    },
                };

                for (const auto& seed : seeds)
                {
                    if (read_gdi_object_slot(c, seed.index) != 0)
                    {
                        continue;
                    }

                    const uint32_t handle = allocate_gdi_object(c, seed.type, seed.attr_size);
                    if (handle == 0)
                    {
                        continue;
                    }

                    const uint64_t handle64 = handle;
                    write_gdi_object_slot(c, seed.index, handle64);
                    if (seed.type == k_gdi_brush_type)
                    {
                        const uint32_t color = seed.index == k_stock_white_brush_index ? 0x00FFFFFFu : k_default_system_colors[15];
                        set_gdi_object_color(c, handle, k_gdi_brush_type, color);
                    }
                    else if (seed.type == k_gdi_pen_type)
                    {
                        set_gdi_object_color(c, handle, k_gdi_pen_type, 0x00000000u);
                    }

                    if (seed.mirror_to_default_gui_slot)
                    {
                        write_gdi_data_slot(c, k_default_gui_font_data_index, handle64);
                    }
                }

                if (read_gdi_data_slot(c, k_default_gui_font_data_index) == 0)
                {
                    const auto default_gui_font = read_gdi_object_slot(c, k_stock_default_gui_font_index);
                    if (default_gui_font != 0)
                    {
                        write_gdi_data_slot(c, k_default_gui_font_data_index, default_gui_font);
                    }
                }

                seed_user_system_color_brushes(c);
            }

            void initialize_dc_attr(const syscall_context& c, const uint64_t dc_attr)
            {
                std::array<uint8_t, k_gdi_dc_attr_size> zeroed{};
                c.emu.write_memory(dc_attr, zeroed.data(), zeroed.size());

                const uint64_t system_font = read_gdi_object_slot(c, k_stock_system_font_index);
                if (system_font != 0)
                {
                    const auto handle_value = static_cast<uint32_t>(system_font);
                    c.emu.write_memory(dc_attr + k_gdi_dc_attr_font_offset, &handle_value, sizeof(handle_value));
                }
            }

            uint32_t allocate_gdi_dc(const syscall_context& c, uint64_t& dc_attr)
            {
                seed_gdi_stock_objects(c);

                dc_attr = allocate_gdi_user_block(c, k_gdi_dc_attr_size);
                if (dc_attr == 0)
                {
                    return 0;
                }

                initialize_dc_attr(c, dc_attr);
                const auto handle_value = allocate_gdi_handle(c, k_gdi_dc_type, dc_attr, dc_attr);
                if (handle_value != 0)
                {
                    c.proc.gdi_dc_states[handle_value] = {};
                }
                return handle_value;
            }

            hdc ensure_default_hdc(const syscall_context& c)
            {
                if (c.proc.gdi_default_dc_handle != 0)
                {
                    return c.proc.gdi_default_dc_handle;
                }

                uint64_t dc_attr = 0;
                const uint32_t handle_value = allocate_gdi_dc(c, dc_attr);
                if (handle_value == 0)
                {
                    return 0;
                }

                c.proc.gdi_default_dc_handle = handle_value;
                return handle_value;
            }

            uint32_t get_device_caps_value(const uint32_t index)
            {
                switch (index)
                {
                case 4: // HORZSIZE
                    return (k_default_width * 254) / (k_default_dpi * 10);
                case 6: // VERTSIZE
                    return (k_default_height * 254) / (k_default_dpi * 10);
                case 8:    // HORZRES
                case 0x76: // DESKTOPHORZRES
                    return k_default_width;
                case 0xA:  // VERTRES
                case 0x75: // DESKTOPVERTRES
                    return k_default_height;
                case 14:
                case 0x28: // PLANES
                case 0x2A: // NUMBRUSHES
                case 0x2C: // NUMPENS
                    return 1;
                case 0x58: // LOGPIXELSX
                case 0x5A: // LOGPIXELSY
                    return k_default_dpi;
                case 12:
                    return 32;
                default:
                    return 0;
                }
            }

            void write_device_caps(const syscall_context& c, const emulator_pointer caps_ptr, const size_t count)
            {
                std::vector<uint32_t> caps(count, 0);
                const auto set_cap = [&](const size_t idx, const uint32_t value) {
                    if (idx < caps.size())
                    {
                        caps[idx] = value;
                    }
                };

                set_cap(4, get_device_caps_value(4));
                set_cap(6, get_device_caps_value(6));
                set_cap(8, get_device_caps_value(8));
                set_cap(0xA, get_device_caps_value(0xA));
                set_cap(0x28, get_device_caps_value(0x28));
                set_cap(0x2A, get_device_caps_value(0x2A));
                set_cap(0x2C, get_device_caps_value(0x2C));
                set_cap(0x58, get_device_caps_value(0x58));
                set_cap(0x5A, get_device_caps_value(0x5A));

                if (caps_ptr != 0)
                {
                    c.emu.write_memory(caps_ptr, caps.data(), caps.size() * sizeof(uint32_t));
                }
            }

            uint32_t get_gdi_object_size(const uint8_t type)
            {
                switch (type)
                {
                case k_gdi_brush_type:
                    return k_logbrush_size;
                case k_gdi_pen_type:
                    return k_logpen_size;
                case k_gdi_font_type:
                    return k_logfontw_size;
                case k_gdi_bitmap_type:
                    return k_bitmap_size;
                case k_gdi_region_type:
                    return k_gdi_region_attr_size;
                default:
                    return 0;
                }
            }

            bool read_gdi_entry_for_handle(const syscall_context& c, const uint32_t handle_value, GDI_HANDLE_ENTRY64& entry,
                                           uint64_t& entry_addr)
            {
                const auto table = ensure_gdi_shared_table(c);
                if (table == 0)
                {
                    return false;
                }

                const uint32_t index = handle_value & 0xFFFF;
                if (index >= GDI_MAX_HANDLE_COUNT)
                {
                    return false;
                }

                entry_addr = table + (static_cast<uint64_t>(index) * sizeof(GDI_HANDLE_ENTRY64));
                const emulator_object<GDI_HANDLE_ENTRY64> entry_obj{c.emu, entry_addr};
                entry = entry_obj.read();

                const auto unique = static_cast<uint16_t>(handle_value >> 16);
                return entry.Type != 0 && entry.Unique == unique;
            }

            template <typename... Args>
            void dxgk_info(const syscall_context& c, const char* fmt, Args&&... args)
            {
#ifdef ENABLE_DXGK_LOGGING
                c.win_emu.log.info(fmt, std::forward<Args>(args)...);
#else
                (void)c;
                (void)fmt;
                ((void)args, ...);
#endif
            }

            template <typename... Args>
            void dxgk_warn(const syscall_context& c, const char* fmt, Args&&... args)
            {
#ifdef ENABLE_DXGK_LOGGING
                c.win_emu.log.warn(fmt, std::forward<Args>(args)...);
#else
                (void)c;
                (void)fmt;
                ((void)args, ...);
#endif
            }

            template <typename... Args>
            void dxgk_error(const syscall_context& c, const char* fmt, Args&&... args)
            {
#ifdef ENABLE_DXGK_LOGGING
                c.win_emu.log.error(fmt, std::forward<Args>(args)...);
#else
                (void)c;
                (void)fmt;
                ((void)args, ...);
#endif
            }

            EMU_D3DKMT_ADAPTERINFO make_default_dxgk_adapter_info()
            {
                EMU_D3DKMT_ADAPTERINFO adapter_info{};
                adapter_info.hAdapter = k_dxgk_adapter_handle;
                adapter_info.AdapterLuid = k_dxgk_adapter_luid;
                adapter_info.NumOfSources = k_dxgk_adapter_source_count;
                adapter_info.bPrecisePresentRegionsPreferred = FALSE;
                return adapter_info;
            }

            uint64_t infer_warp_allocation_size_from_private_data(const syscall_context& c, const uint64_t private_data,
                                                                  const uint32_t private_data_size)
            {
                if (private_data == 0 || private_data_size < 0x1C)
                {
                    return 0;
                }

                const auto kind = c.emu.read_memory<uint32_t>(private_data + 0x00);
                const auto width_or_size = c.emu.read_memory<uint32_t>(private_data + 0x04);
                const auto height = c.emu.read_memory<uint32_t>(private_data + 0x08);
                const auto pitch = c.emu.read_memory<uint32_t>(private_data + 0x14);
                const auto byte_size = c.emu.read_memory<uint32_t>(private_data + 0x18);

                if (byte_size == 0)
                {
                    return 0;
                }

                if (kind == 1)
                {
                    return width_or_size != 0 && pitch == byte_size ? byte_size : 0;
                }

                if (kind == 3)
                {
                    if (width_or_size == 0 || height == 0 || pitch == 0)
                    {
                        return 0;
                    }

                    const uint64_t minimum_size = static_cast<uint64_t>(pitch) * height;
                    return byte_size >= minimum_size ? byte_size : 0;
                }

                return 0;
            }

            template <typename T>
            NTSTATUS write_query_adapter_info(const syscall_context& c, const EMU_D3DKMT_QUERYADAPTERINFO& query, const T& value)
            {
                if (query.PrivateDriverDataSize < sizeof(T))
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                c.emu.write_memory(query.pPrivateDriverData, &value, sizeof(T));
                return STATUS_SUCCESS;
            }

            template <typename TDestroyAllocation>
            NTSTATUS destroy_dxgk_allocations(const syscall_context& c, const emulator_object<TDestroyAllocation> destroy_allocation)
            {
                if (!destroy_allocation)
                {
                    return STATUS_INVALID_PARAMETER;
                }

                NTSTATUS status = STATUS_SUCCESS;

                destroy_allocation.access([&](const TDestroyAllocation& params) {
                    if (params.hDevice != 0 && params.hDevice != k_dxgk_device_handle)
                    {
                        dxgk_warn(c, "DestroyAllocation: Unexpected device 0x%X", params.hDevice);
                    }

                    if (params.AllocationCount == 0)
                    {
                        if (params.hResource == 0)
                        {
                            return;
                        }

                        const auto destroy_count = c.proc.dxgk.destroy_resource_allocations(c.win_emu.memory, params.hResource);
                        if (destroy_count == 0)
                        {
                            dxgk_warn(c, "DestroyAllocation: Unknown resource handle 0x%X", params.hResource);
                            return;
                        }

                        dxgk_info(c, "DestroyAllocation: destroyed %zu allocation(s) for resource 0x%X", destroy_count, params.hResource);
                        return;
                    }

                    if (params.phAllocationList == 0)
                    {
                        dxgk_warn(c, "DestroyAllocation: AllocationCount=%u but phAllocationList is null", params.AllocationCount);
                        status = STATUS_INVALID_PARAMETER;
                        return;
                    }

                    const emulator_object<UINT32> allocation_list{c.emu, params.phAllocationList};
                    for (UINT32 i = 0; i < params.AllocationCount; ++i)
                    {
                        const auto allocation_handle = allocation_list.read(i);
                        if (!c.proc.dxgk.destroy_allocation(c.win_emu.memory, allocation_handle))
                        {
                            dxgk_warn(c, "DestroyAllocation: Unknown allocation handle 0x%X", allocation_handle);
                            continue;
                        }

                        dxgk_info(c, "DestroyAllocation: destroyed allocation 0x%X", allocation_handle);
                    }
                });

                return status;
            }
        }

        // Returns the surface a paint DC should be presented to, and (via present_handle) the host window handle it
        // belongs to (the top-level window for child controls). Used by NtUserEndPaint to flush guest paint output.
        gdi_bitmap_surface* get_dc_present_surface(const syscall_context& c, const hdc dc, uint32_t& present_handle)
        {
            int32_t origin_x = 0;
            int32_t origin_y = 0;
            return resolve_dc_surface(c, dc, origin_x, origin_y, present_handle);
        }

        NTSTATUS handle_NtDxgkIsFeatureEnabled()
        {
            // puts("NtDxgkIsFeatureEnabled not supported");
            return STATUS_NOT_SUPPORTED;
        }

        NTSTATUS handle_NtGdiInit(const syscall_context& c)
        {
            if (ensure_gdi_shared_table(c) == 0)
            {
                return STATUS_UNSUCCESSFUL;
            }

            const auto cookie = ensure_gdi_cookie(c);
            seed_gdi_stock_objects(c);

            return static_cast<NTSTATUS>(cookie);
        }

        NTSTATUS handle_NtGdiInit2(const syscall_context& c)
        {
            return handle_NtGdiInit(c);
        }

        uint32_t handle_NtGdiGetDeviceCaps(const syscall_context&, const hdc /*dc*/, const uint32_t index)
        {
            return get_device_caps_value(index);
        }

        uint32_t handle_NtGdiGetDeviceCapsAll(const syscall_context& c, const hdc /*dc*/, const emulator_pointer caps)
        {
            write_device_caps(c, caps, 0x24);
            return 1;
        }

        uint32_t handle_NtGdiComputeXformCoefficients(const syscall_context&, const hdc dc)
        {
            return dc ? 1 : 0;
        }

        void draw_system_button_glyph(const syscall_context& c, const hdc dc, const int x, const int y, const uint32_t index)
        {
            constexpr uint32_t k_obi_radio_mask = 0x47;
            constexpr uint32_t k_obi_check_base = 0x48;
            constexpr uint32_t k_obi_radio_base = 0x4D;
            constexpr uint32_t k_obi_3state_base = 0x52;
            constexpr uint32_t k_state_count = 5;

            if (index == k_obi_radio_mask)
            {
                return;
            }

            bool radio = false;
            bool indeterminate = false;
            uint32_t offset = 0;
            if (index >= k_obi_radio_base && index < k_obi_radio_base + k_state_count)
            {
                radio = true;
                offset = index - k_obi_radio_base;
            }
            else if (index >= k_obi_3state_base && index < k_obi_3state_base + k_state_count)
            {
                indeterminate = true;
                offset = index - k_obi_3state_base;
            }
            else if (index >= k_obi_check_base && index < k_obi_check_base + k_state_count)
            {
                offset = index - k_obi_check_base;
            }
            else
            {
                return; // not a checkbox/radio glyph we model
            }

            const bool checked = indeterminate || offset == 1 || offset == 3 || offset == 4;
            const bool pressed = offset == 2 || offset == 3;
            const bool disabled_mark = indeterminate || offset == 4;

            gdi_dc_state* dc_state = nullptr;
            gdi_bitmap_surface* surface = nullptr;
            int32_t origin_x = 0;
            int32_t origin_y = 0;
            if (!get_dc_state_and_surface(c, dc, dc_state, surface, origin_x, origin_y) || !surface)
            {
                return;
            }

            const int ox = x + origin_x;
            const int oy = y + origin_y;

            constexpr int k_glyph = 13;
            constexpr uint32_t k_white = 0xFFFFFFFFu;
            constexpr uint32_t k_face = 0xFFF0F0F0u;
            constexpr uint32_t k_shadow = 0xFF808080u;
            constexpr uint32_t k_frame = 0xFF404040u;
            constexpr uint32_t k_ink = 0xFF000000u;
            constexpr uint32_t k_ink_disabled = 0xFF808080u;

            const uint32_t interior = pressed ? k_face : k_white;
            const uint32_t mark = disabled_mark ? k_ink_disabled : k_ink;

            if (radio)
            {
                // 13x13 disc: thin ring around the edge, filled interior, centre dot when checked.
                constexpr int center = 6;
                for (int dy = -center; dy <= center; ++dy)
                {
                    for (int dx = -center; dx <= center; ++dx)
                    {
                        const int d2 = dx * dx + dy * dy;
                        if (d2 > 37)
                        {
                            continue;
                        }
                        uint32_t color = (d2 >= 25) ? k_frame : interior;
                        if (checked && d2 <= 5)
                        {
                            color = mark;
                        }
                        set_surface_pixel(*surface, ox + center + dx, oy + center + dy, color);
                    }
                }
                return;
            }

            fill_rect(*surface, ox, oy, ox + k_glyph, oy + k_glyph, interior);
            for (int i = 0; i < k_glyph; ++i)
            {
                set_surface_pixel(*surface, ox + i, oy, k_shadow);              // top edge
                set_surface_pixel(*surface, ox, oy + i, k_shadow);              // left edge
                set_surface_pixel(*surface, ox + i, oy + k_glyph - 1, k_white); // bottom edge
                set_surface_pixel(*surface, ox + k_glyph - 1, oy + i, k_white); // right edge
            }
            for (int i = 1; i < k_glyph - 1; ++i)
            {
                set_surface_pixel(*surface, ox + i, oy + 1, k_frame); // inner top
                set_surface_pixel(*surface, ox + 1, oy + i, k_frame); // inner left
            }

            if (checked)
            {
                static constexpr std::array<const char*, k_glyph> k_check = {
                    "             ", //
                    "             ", //
                    "             ", //
                    "          XX ", //
                    "         XX  ", //
                    "        XX   ", //
                    "  X    XX    ", //
                    "  XX  XX     ", //
                    "   XXXX      ", //
                    "    XX       ", //
                    "             ", //
                    "             ", //
                    "             ", //
                };
                for (int row = 0; row < k_glyph; ++row)
                {
                    for (int col = 0; col < k_glyph; ++col)
                    {
                        if (k_check[row][col] != ' ')
                        {
                            set_surface_pixel(*surface, ox + col, oy + row, mark);
                        }
                    }
                }
            }
        }

        BOOL handle_NtGdiFlush(const syscall_context& c)
        {
            auto& thread = c.win_emu.current_thread();
            if (thread.teb64)
            {
                GDI_TEB_BATCH64 batch{};
                thread.teb64->access([&](TEB64& teb) {
                    batch = teb.GdiTebBatch;
                    teb.GdiTebBatch = {};
                    teb.GdiBatchCount = 0;
                });
                return flush_gdi_text_batch(c, batch) ? TRUE : FALSE;
            }

            if (thread.teb32)
            {
                GDI_TEB_BATCH32 batch{};
                thread.teb32->access([&](TEB32& teb) {
                    batch = teb.GdiTebBatch;
                    teb.GdiTebBatch = {};
                    teb.GdiBatchCount = 0;
                });
                return flush_gdi_text_batch(c, batch) ? TRUE : FALSE;
            }

            return TRUE;
        }

        uint64_t handle_NtGdiCreateSolidBrush(const syscall_context& c, const uint32_t color, const uint64_t /*unused*/)
        {
            const auto handle = allocate_gdi_object(c, k_gdi_brush_type, k_gdi_brush_attr_size);
            uint64_t brush_attr = 0;
            if (handle != 0 && get_gdi_object_address(c, static_cast<uint32_t>(handle), k_gdi_brush_type, brush_attr))
            {
                c.emu.write_memory(brush_attr + sizeof(uint32_t), &color, sizeof(color));
            }
            return handle;
        }

        uint64_t handle_NtGdiCreatePatternBrushInternal(const syscall_context& c, const handle /*bitmap*/, const uint32_t /*unused*/)
        {
            return allocate_gdi_object(c, k_gdi_brush_type, k_gdi_brush_attr_size);
        }

        uint64_t handle_NtGdiCreatePen(const syscall_context& c, const uint32_t /*style*/, const uint32_t /*width*/, const uint32_t color)
        {
            const auto handle = allocate_gdi_object(c, k_gdi_pen_type, k_gdi_pen_attr_size);
            uint64_t pen_attr = 0;
            if (handle != 0 && get_gdi_object_address(c, static_cast<uint32_t>(handle), k_gdi_pen_type, pen_attr))
            {
                c.emu.write_memory(pen_attr + sizeof(uint32_t), &color, sizeof(color));
            }
            return handle;
        }

        uint64_t handle_NtGdiCreateCompatibleDC(const syscall_context& c, const hdc /*dc*/)
        {
            uint64_t dc_attr = 0;
            return allocate_gdi_dc(c, dc_attr);
        }

        hdc handle_NtGdiOpenDCW(const syscall_context& c)
        {
            return ensure_default_hdc(c);
        }

        int32_t handle_NtGdiSaveDC(const syscall_context& c, const hdc dc)
        {
            const auto dc_value = static_cast<uint32_t>(dc);
            auto& stack = c.proc.gdi_dc_save_states[dc_value];

            gdi_dc_state snapshot{};
            if (const auto it = c.proc.gdi_dc_states.find(dc_value); it != c.proc.gdi_dc_states.end())
            {
                snapshot = it->second;
            }

            stack.push_back(snapshot);
            return static_cast<int32_t>(stack.size());
        }

        BOOL handle_NtGdiRestoreDC(const syscall_context& c, const hdc dc, const int32_t saved_dc)
        {
            const auto dc_value = static_cast<uint32_t>(dc);
            const auto it = c.proc.gdi_dc_save_states.find(dc_value);
            if (it == c.proc.gdi_dc_save_states.end() || it->second.empty())
            {
                return FALSE;
            }

            auto& stack = it->second;

            // A positive level identifies an absolute save instance (1-based); a negative level is
            // relative to the top of the stack (-1 being the most recently saved state).
            size_t target_index = 0;
            if (saved_dc < 0)
            {
                const auto relative = static_cast<size_t>(-static_cast<int64_t>(saved_dc));
                if (relative == 0 || relative > stack.size())
                {
                    return FALSE;
                }
                target_index = stack.size() - relative;
            }
            else
            {
                if (saved_dc < 1 || static_cast<size_t>(saved_dc) > stack.size())
                {
                    return FALSE;
                }
                target_index = static_cast<size_t>(saved_dc) - 1;
            }

            c.proc.gdi_dc_states[dc_value] = stack[target_index];
            stack.resize(target_index);
            return TRUE;
        }

        uint64_t handle_NtGdiCreateCompatibleBitmap(const syscall_context& c, const hdc /*dc*/, const uint32_t width, const uint32_t height)
        {
            const auto handle_value = allocate_gdi_object(c, k_gdi_bitmap_type, k_gdi_bitmap_attr_size);
            if (handle_value != 0)
            {
                auto& surface = c.proc.gdi_bitmap_surfaces[handle_value];
                surface.width = width;
                surface.height = height;
                surface.pixels.assign(static_cast<size_t>(width) * static_cast<size_t>(height), k_default_bitmap_fill);
            }
            return handle_value;
        }

        uint64_t handle_NtGdiCreateBitmap(const syscall_context& c, const uint32_t width, const uint32_t height, const uint32_t planes,
                                          const uint32_t bits_pixel, const emulator_pointer bits)
        {
            const auto handle_value = allocate_gdi_object(c, k_gdi_bitmap_type, k_gdi_bitmap_attr_size);
            if (handle_value != 0)
            {
                auto& surface = c.proc.gdi_bitmap_surfaces[handle_value];
                surface.width = width;
                surface.height = height;
                surface.pixels.assign(static_cast<size_t>(width) * static_cast<size_t>(height), k_default_bitmap_fill);

                if (bits != 0 && planes == 1 && bits_pixel == 32)
                {
                    const auto byte_count = static_cast<size_t>(width) * static_cast<size_t>(height) * sizeof(uint32_t);
                    c.emu.read_memory(bits, surface.pixels.data(), byte_count);
                }
            }
            return handle_value;
        }

        uint64_t handle_NtGdiCreateDIBitmapInternal(const syscall_context& c, const hdc /*dc*/, const uint32_t width, const uint32_t height,
                                                    const uint32_t /*usage*/, const emulator_pointer bits, const emulator_pointer /*info*/,
                                                    const uint32_t /*info_header_size*/, const uint32_t /*init*/, const uint32_t /*offset*/,
                                                    const uint32_t /*cj*/, const uint32_t /*i_usage*/)
        {
            const auto handle_value = allocate_gdi_object(c, k_gdi_bitmap_type, k_gdi_bitmap_attr_size);
            if (handle_value != 0)
            {
                auto& surface = c.proc.gdi_bitmap_surfaces[handle_value];
                surface.width = width;
                surface.height = height;
                surface.pixels.assign(static_cast<size_t>(width) * static_cast<size_t>(height), k_default_bitmap_fill);
                if (bits != 0)
                {
                    const auto byte_count = static_cast<size_t>(width) * static_cast<size_t>(height) * sizeof(uint32_t);
                    c.emu.read_memory(bits, surface.pixels.data(), byte_count);
                }
            }
            return handle_value;
        }

        // GetDIBits: report a bitmap's geometry into the caller's BITMAPINFOHEADER and, when a pixel buffer is
        // supplied, copy its contents out as a bottom-up 32bpp BI_RGB DIB. The emulator's GDI bitmaps are all
        // stored as 32bpp BGRA surfaces, so that is the only format reported. D3D9/DX11 init queries this to
        // probe a memory bitmap before deciding its presentation path.
        int handle_NtGdiGetDIBitsInternal(const syscall_context& c, const hdc /*dc*/, const handle bitmap, const uint32_t start_scan,
                                          const uint32_t scan_lines, const emulator_pointer bits, const emulator_pointer info,
                                          const uint32_t /*usage*/, const uint32_t max_bits, const uint32_t /*max_info*/)
        {
            if (info == 0)
            {
                return 0;
            }

            const auto it = c.proc.gdi_bitmap_surfaces.find(static_cast<uint32_t>(bitmap.bits));
            if (it == c.proc.gdi_bitmap_surfaces.end())
            {
                return 0;
            }
            const auto& surface = it->second;

            const int32_t bi_width = static_cast<int32_t>(surface.width);
            const int32_t bi_height = static_cast<int32_t>(surface.height);
            const uint16_t planes = 1;
            const uint16_t bit_count = 32;
            const uint32_t compression = 0; // BI_RGB
            const uint32_t size_image = surface.width * surface.height * static_cast<uint32_t>(sizeof(uint32_t));
            c.emu.write_memory(info + 4, &bi_width, sizeof(bi_width));
            c.emu.write_memory(info + 8, &bi_height, sizeof(bi_height));
            c.emu.write_memory(info + 12, &planes, sizeof(planes));
            c.emu.write_memory(info + 14, &bit_count, sizeof(bit_count));
            c.emu.write_memory(info + 16, &compression, sizeof(compression));
            c.emu.write_memory(info + 20, &size_image, sizeof(size_image));

            // Query mode: report geometry only.
            if (bits == 0 || surface.width == 0 || surface.height == 0)
            {
                return bi_height;
            }

            // Copy the requested scanlines as a bottom-up DIB (row 0 is the bottom image row).
            const size_t stride = static_cast<size_t>(surface.width) * sizeof(uint32_t);
            uint32_t copied = 0;
            for (uint32_t row = 0; row < scan_lines; ++row)
            {
                const uint32_t dib_row = start_scan + row;
                if (dib_row >= surface.height)
                {
                    break;
                }
                const size_t dst_offset = static_cast<size_t>(row) * stride;
                if (max_bits != 0 && dst_offset + stride > max_bits)
                {
                    break;
                }
                const uint32_t src_y = surface.height - 1 - dib_row;
                c.emu.write_memory(bits + dst_offset, surface.pixels.data() + static_cast<size_t>(src_y) * surface.width, stride);
                ++copied;
            }

            return static_cast<int>(copied);
        }

        int handle_NtGdiSetDIBitsToDeviceInternal(const syscall_context& c, const hdc dc, const int x_dest, const int y_dest,
                                                  const uint32_t width, const uint32_t height, const int x_src, const int y_src,
                                                  const uint32_t /*start_scan*/, const uint32_t scan_lines, const emulator_pointer bits,
                                                  const emulator_pointer info, const uint32_t /*color_use*/, const uint32_t max_bits,
                                                  const uint32_t /*max_info*/, const uint32_t /*transform_coordinates*/,
                                                  const uint64_t /*color_transform*/)
        {
            if (bits == 0 || info == 0)
            {
                return 0;
            }

            gdi_dc_state* dc_state = nullptr;
            gdi_bitmap_surface* surface = nullptr;
            int32_t origin_x = 0;
            int32_t origin_y = 0;
            if (!get_dc_state_and_surface(c, dc, dc_state, surface, origin_x, origin_y) || !surface)
            {
                return 0;
            }

            int32_t bi_width = 0;
            int32_t bi_height = 0;
            uint16_t bit_count = 0;
            uint32_t compression = 0;
            c.emu.read_memory(info + 4, &bi_width, sizeof(bi_width));
            c.emu.read_memory(info + 8, &bi_height, sizeof(bi_height));
            c.emu.read_memory(info + 14, &bit_count, sizeof(bit_count));
            c.emu.read_memory(info + 16, &compression, sizeof(compression));

            constexpr uint32_t bi_rgb = 0;
            if (bit_count != 32 || compression != bi_rgb || bi_width <= 0)
            {
                c.win_emu.log.warn("NtGdiSetDIBitsToDeviceInternal: unsupported DIB (bpp=%u compression=%u width=%d)\n", bit_count,
                                   compression, bi_width);
                return 0;
            }

            const bool top_down = bi_height < 0;
            const auto src_width = static_cast<uint32_t>(bi_width);
            const auto src_height = static_cast<uint32_t>(top_down ? -bi_height : bi_height);
            const auto stored_rows = std::min(scan_lines, src_height);
            const size_t stride = static_cast<size_t>(src_width) * sizeof(uint32_t);

            std::vector<uint8_t> data(stride * stored_rows);
            if (data.empty())
            {
                return 0;
            }
            if (max_bits != 0 && data.size() > max_bits)
            {
                data.resize(max_bits);
            }
            c.emu.read_memory(bits, data.data(), data.size());
            const auto available_rows = static_cast<uint32_t>(data.size() / stride);

            uint32_t copied = 0;
            for (uint32_t j = 0; j < height; ++j)
            {
                const auto src_img_y = static_cast<uint32_t>(y_src) + j;
                if (src_img_y >= src_height)
                {
                    break;
                }
                const uint32_t bits_row = top_down ? src_img_y : (src_height - 1 - src_img_y);
                if (bits_row >= available_rows)
                {
                    continue;
                }

                const uint8_t* row = data.data() + static_cast<size_t>(bits_row) * stride;
                for (uint32_t i = 0; i < width; ++i)
                {
                    const auto src_x = static_cast<uint32_t>(x_src) + i;
                    if (src_x >= src_width)
                    {
                        break;
                    }
                    uint32_t pixel = 0;
                    std::memcpy(&pixel, row + static_cast<size_t>(src_x) * sizeof(uint32_t), sizeof(pixel));
                    set_surface_pixel(*surface, x_dest + origin_x + static_cast<int>(i), y_dest + origin_y + static_cast<int>(j),
                                      pixel | 0xFF000000u);
                }
                ++copied;
            }

            return static_cast<int>(copied);
        }

        // Software Vulkan/GL drivers (e.g. SwiftShader) present a rendered frame by grabbing the window DC
        // with GetDC and StretchDIBits-ing the framebuffer into it -- there is no BeginPaint/EndPaint on
        // this path. We blit the source DIB into the DC's backing surface (nearest-neighbour scaled from
        // the source rectangle onto the destination rectangle) and, for a window-backed DC, present the
        // surface to the UI immediately, mirroring what NtUserEndPaint does for ordinary GDI painting.
        int handle_NtGdiStretchDIBitsInternal(const syscall_context& c, const hdc dc, const int x_dst, const int y_dst, const int dst_width,
                                              const int dst_height, const int x_src, const int y_src, const int src_width,
                                              const int src_height, const emulator_pointer bits, const emulator_pointer info,
                                              const uint32_t /*usage*/, const uint32_t /*rop*/, const uint32_t /*max_info*/,
                                              const uint32_t max_bits, const uint64_t /*color_transform*/)
        {
            if (bits == 0 || info == 0 || dst_width == 0 || dst_height == 0 || src_width <= 0 || src_height <= 0)
            {
                return 0;
            }

            int32_t origin_x = 0;
            int32_t origin_y = 0;
            uint32_t present_handle = 0;
            gdi_bitmap_surface* surface = resolve_dc_surface(c, dc, origin_x, origin_y, present_handle);
            if (!surface)
            {
                return 0;
            }

            int32_t bi_width = 0;
            int32_t bi_height = 0;
            uint16_t bit_count = 0;
            uint32_t compression = 0;
            c.emu.read_memory(info + 4, &bi_width, sizeof(bi_width));
            c.emu.read_memory(info + 8, &bi_height, sizeof(bi_height));
            c.emu.read_memory(info + 14, &bit_count, sizeof(bit_count));
            c.emu.read_memory(info + 16, &compression, sizeof(compression));

            constexpr uint32_t bi_rgb = 0;
            if (bit_count != 32 || compression != bi_rgb || bi_width <= 0)
            {
                c.win_emu.log.warn("NtGdiStretchDIBitsInternal: unsupported DIB (bpp=%u compression=%u width=%d)\n", bit_count, compression,
                                   bi_width);
                return 0;
            }

            const bool top_down = bi_height < 0;
            const auto img_width = static_cast<uint32_t>(bi_width);
            const auto img_height = static_cast<uint32_t>(top_down ? -bi_height : bi_height);
            const size_t stride = static_cast<size_t>(img_width) * sizeof(uint32_t);

            std::vector<uint8_t> data(stride * img_height);
            if (data.empty())
            {
                return 0;
            }
            if (max_bits != 0 && data.size() > max_bits)
            {
                data.resize(max_bits);
            }
            c.emu.read_memory(bits, data.data(), data.size());
            const auto available_rows = static_cast<uint32_t>(data.size() / stride);

            // Nearest-neighbour scale the source rectangle onto the destination rectangle. Negative dest
            // extents mirror the image (GDI semantics); the source rectangle is taken as positive.
            const int dst_w = std::abs(dst_width);
            const int dst_h = std::abs(dst_height);
            const bool flip_x = dst_width < 0;
            const bool flip_y = dst_height < 0;

            for (int dy = 0; dy < dst_h; ++dy)
            {
                const auto src_off_y = static_cast<uint32_t>(static_cast<int64_t>(dy) * src_height / dst_h);
                const auto img_y = static_cast<uint32_t>(y_src) + src_off_y;
                if (img_y >= img_height)
                {
                    continue;
                }
                const uint32_t bits_row = top_down ? img_y : (img_height - 1 - img_y);
                if (bits_row >= available_rows)
                {
                    continue;
                }
                const uint8_t* row = data.data() + static_cast<size_t>(bits_row) * stride;
                const int out_y = y_dst + origin_y + (flip_y ? (dst_h - 1 - dy) : dy);

                for (int dx = 0; dx < dst_w; ++dx)
                {
                    const auto src_off_x = static_cast<uint32_t>(static_cast<int64_t>(dx) * src_width / dst_w);
                    const auto img_x = static_cast<uint32_t>(x_src) + src_off_x;
                    if (img_x >= img_width)
                    {
                        continue;
                    }
                    uint32_t pixel = 0;
                    std::memcpy(&pixel, row + static_cast<size_t>(img_x) * sizeof(uint32_t), sizeof(pixel));
                    const int out_x = x_dst + origin_x + (flip_x ? (dst_w - 1 - dx) : dx);
                    set_surface_pixel(*surface, out_x, out_y, pixel | 0xFF000000u);
                }
            }

            if (present_handle != 0 && surface->width > 0 && surface->height > 0 && !surface->pixels.empty())
            {
                c.win_emu.ui().present_surface(present_handle,
                                               ui_surface_desc{.width = static_cast<int>(surface->width),
                                                               .height = static_cast<int>(surface->height),
                                                               .stride = static_cast<int>(surface->width * sizeof(uint32_t)),
                                                               .format = ui_surface_format::bgra8,
                                                               .pixels = surface->pixels.data()});
            }

            return static_cast<int>(img_height);
        }

        uint32_t handle_NtGdiDeleteObjectApp(const syscall_context& c, const uint32_t handle_value)
        {
            GDI_HANDLE_ENTRY64 entry{};
            uint64_t entry_addr = 0;
            if (!read_gdi_entry_for_handle(c, handle_value, entry, entry_addr))
            {
                return 0;
            }

            const emulator_object<GDI_HANDLE_ENTRY64> entry_obj{c.emu, entry_addr};
            entry_obj.access([&](GDI_HANDLE_ENTRY64& writable) {
                const auto unique = writable.Unique;
                writable = {};
                writable.Unique = unique;
            });

            if (handle_value == c.proc.gdi_default_dc_handle)
            {
                c.proc.gdi_default_dc_handle = 0;
            }

            c.proc.gdi_dc_states.erase(handle_value);
            c.proc.gdi_bitmap_surfaces.erase(handle_value);
            return 1;
        }

        uint64_t handle_NtGdiSelectBitmap(const syscall_context& c, const hdc dc, const handle bitmap)
        {
            const auto it = c.proc.gdi_dc_states.find(static_cast<uint32_t>(dc));
            if (it == c.proc.gdi_dc_states.end())
            {
                return bitmap.bits;
            }

            const auto old = it->second.selected_bitmap;
            it->second.selected_bitmap = static_cast<uint32_t>(bitmap.bits);
            return old;
        }

        uint64_t handle_NtGdiSelectFont(const syscall_context&, const hdc dc, const uint64_t font)
        {
            return dc != 0 ? font : 0;
        }

        hdc handle_NtGdiGetDCforBitmap(const syscall_context& c, const handle /*bitmap*/)
        {
            return ensure_default_hdc(c);
        }

        BOOL handle_NtGdiGetDCDword(const syscall_context& c, const hdc dc, const uint32_t /*index*/, const emulator_pointer result)
        {
            if (dc == 0 || result == 0)
            {
                return FALSE;
            }

            // gdi32 only falls back to this syscall for DC dwords it does not cache client-side
            // (e.g. GdiGetIsMemDc). 0 is the correct default for the control paint path: the paint
            // DC is a real (non-memory) DC, so "is memory DC" is false.
            uint32_t value = 0;
            c.emu.write_memory(result, &value, sizeof(value));
            return TRUE;
        }

        BOOL handle_NtGdiSetBrushOrg(const syscall_context& c, const hdc dc, const int /*x*/, const int /*y*/, const emulator_pointer prev)
        {
            if (dc == 0)
            {
                return FALSE;
            }

            if (prev != 0)
            {
                constexpr POINT previous{};
                c.emu.write_memory(prev, &previous, sizeof(previous));
            }

            return TRUE;
        }

        uint64_t handle_NtGdiHfontCreate(const syscall_context& c, const emulator_pointer /*logfont*/, const uint32_t /*angle*/)
        {
            return allocate_gdi_object(c, k_gdi_font_type, k_gdi_font_attr_size);
        }

        uint32_t handle_NtGdiExtGetObjectW(const syscall_context& c, const uint32_t handle_value, const uint32_t size,
                                           const emulator_pointer buffer)
        {
            GDI_HANDLE_ENTRY64 entry{};
            uint64_t entry_addr = 0;
            if (!read_gdi_entry_for_handle(c, handle_value, entry, entry_addr))
            {
                return 0;
            }

            const uint32_t object_size = get_gdi_object_size(entry.Type);
            if (object_size == 0)
            {
                return 0;
            }

            if (buffer == 0)
            {
                return object_size;
            }

            if (size < object_size)
            {
                return 0;
            }

            std::vector<uint8_t> zeroed(object_size, 0);
            c.emu.write_memory(buffer, zeroed.data(), zeroed.size());
            return object_size;
        }

        uint32_t handle_NtGdiEnumFonts()
        {
            return 0;
        }

        uint32_t handle_NtGdiGetTextCharsetInfo(const syscall_context& c, const hdc /*dc*/, const emulator_pointer sig,
                                                const uint32_t /*flags*/)
        {
            if (sig != 0)
            {
                std::array<uint8_t, 0x18> zeroed{};
                c.emu.write_memory(sig, zeroed.data(), zeroed.size());
            }

            return 1;
        }

        uint32_t handle_NtGdiQueryFontAssocInfo(const syscall_context&, const hdc /*dc*/)
        {
            return 0;
        }

        uint32_t handle_NtGdiGetTextMetricsW(const syscall_context& c, const hdc dc, const emulator_pointer ptm, const uint32_t cj)
        {
            if (dc == 0 || ptm == 0 || cj < k_textmetric_size)
            {
                return 0;
            }

            std::array<uint8_t, k_textmetric_size> zeroed{};
            c.emu.write_memory(ptm, zeroed.data(), zeroed.size());

            const auto write_u32 = [&](const uint32_t offset, const uint32_t value) {
                if (offset + sizeof(uint32_t) <= cj)
                {
                    c.emu.write_memory(ptm + offset, &value, sizeof(value));
                }
            };

            const auto write_u16 = [&](const uint32_t offset, const uint16_t value) {
                if (offset + sizeof(uint16_t) <= cj)
                {
                    c.emu.write_memory(ptm + offset, &value, sizeof(value));
                }
            };

            const auto write_u8 = [&](const uint32_t offset, const uint8_t value) {
                if (offset + sizeof(uint8_t) <= cj)
                {
                    c.emu.write_memory(ptm + offset, &value, sizeof(value));
                }
            };

            write_u32(0x00, k_default_font_height);
            write_u32(0x04, k_default_font_ascent);
            write_u32(0x08, k_default_font_descent);
            write_u32(0x14, k_default_font_width);
            write_u32(0x18, k_default_font_width);
            write_u32(0x1C, k_default_font_weight);
            write_u16(0x2C, 0x20);
            write_u16(0x2E, 0x7E);
            write_u16(0x30, 0x3F);
            write_u16(0x32, 0x20);
            write_u8(0x38, 0x01);

            return 1;
        }

        int32_t handle_NtGdiGetTextFaceW(const syscall_context& c, const hdc dc, const int32_t count, const emulator_pointer face_name,
                                         const BOOL /*alias_name*/)
        {
            if (dc == 0)
            {
                return 0;
            }

            static constexpr std::wstring_view k_default_font_name = L"Segoe UI";
            const auto required = static_cast<int32_t>(k_default_font_name.size() + 1);

            if (face_name == 0)
            {
                return required;
            }

            if (count <= 0)
            {
                return 0;
            }

            const auto writable_chars = std::min<int32_t>(count - 1, static_cast<int32_t>(k_default_font_name.size()));
            if (writable_chars > 0)
            {
                c.emu.write_memory(face_name, k_default_font_name.data(), static_cast<size_t>(writable_chars) * sizeof(wchar_t));
            }

            const wchar_t terminator = L'\0';
            c.emu.write_memory(face_name + static_cast<uint64_t>(writable_chars) * sizeof(wchar_t), &terminator, sizeof(terminator));
            return required;
        }

        BOOL handle_NtGdiGetTextExtent(const syscall_context& c, const hdc dc, const emulator_pointer /*text*/, const int32_t char_count,
                                       const emulator_pointer size, const ULONG /*flags*/)
        {
            if (dc == 0 || size == 0 || char_count < 0)
            {
                return FALSE;
            }

            const SIZE text_size{
                .cx = static_cast<LONG>(k_default_font_width * char_count),
                .cy = static_cast<LONG>(k_default_font_height),
            };
            c.emu.write_memory(size, &text_size, sizeof(text_size));
            return TRUE;
        }

        uint64_t handle_NtGdiCreateRectRgn(const syscall_context& c, const LONG /*x_left*/, const LONG /*y_top*/, const LONG /*x_right*/,
                                           const LONG /*y_bottom*/)
        {
            return allocate_gdi_object(c, k_gdi_region_type, k_gdi_region_attr_size);
        }

        int32_t handle_NtGdiGetRandomRgn(const syscall_context&, const hdc dc, const uint64_t region, const LONG /*index*/)
        {
            return (dc != 0 && region != 0) ? 1 : 0;
        }

        int32_t handle_NtGdiIntersectClipRect(const syscall_context&, const hdc dc, const LONG /*x_left*/, const LONG /*y_top*/,
                                              const LONG /*x_right*/, const LONG /*y_bottom*/)
        {
            return dc != 0 ? 2 : 0;
        }

        uint32_t handle_NtGdiGetCharSet(const syscall_context&, const hdc dc)
        {
            return dc != 0 ? 1 : 0;
        }

        int32_t handle_NtGdiExtSelectClipRgn(const syscall_context&, const hdc dc, const uint64_t /*region*/, const LONG /*mode*/)
        {
            return dc != 0 ? 1 : -1;
        }

        BOOL handle_NtGdiLineTo(const syscall_context& c, const hdc dc, const LONG x_end, const LONG y_end)
        {
            (void)handle_NtGdiFlush(c);

            gdi_dc_state* dc_state = nullptr;
            gdi_bitmap_surface* surface = nullptr;
            int32_t origin_x = 0;
            int32_t origin_y = 0;
            if (!get_dc_state_and_surface(c, dc, dc_state, surface, origin_x, origin_y) || !dc_state || !surface)
            {
                return FALSE;
            }

            uint64_t dc_attr = 0;
            if (get_gdi_object_address(c, static_cast<uint32_t>(dc), k_gdi_dc_type, dc_attr))
            {
                struct
                {
                    int32_t x{};
                    int32_t y{};
                } current_point;
                if (c.win_emu.memory.try_read_memory(dc_attr + k_gdi_dc_attr_pt_current_offset, &current_point, sizeof(current_point)))
                {
                    dc_state->current_x = current_point.x;
                    dc_state->current_y = current_point.y;
                }
            }

            const auto color = get_dc_pen_color(c, dc);
            draw_line(*surface, dc_state->current_x + origin_x, dc_state->current_y + origin_y, x_end + origin_x, y_end + origin_y, color);

            dc_state->current_x = x_end;
            dc_state->current_y = y_end;
            if (dc_attr != 0)
            {
                c.emu.write_memory(dc_attr + k_gdi_dc_attr_pt_current_offset, &dc_state->current_x, sizeof(dc_state->current_x));
                c.emu.write_memory(dc_attr + k_gdi_dc_attr_pt_current_offset + sizeof(int32_t), &dc_state->current_y,
                                   sizeof(dc_state->current_y));
            }
            return TRUE;
        }

        BOOL handle_NtGdiRectangle(const syscall_context& c, const hdc dc, const LONG left, const LONG top, const LONG right,
                                   const LONG bottom)
        {
            set_dc_current_point(c, dc, left, top);

            if (!handle_NtGdiLineTo(c, dc, right - 1, top))
            {
                return FALSE;
            }
            if (!handle_NtGdiLineTo(c, dc, right - 1, bottom - 1))
            {
                return FALSE;
            }
            if (!handle_NtGdiLineTo(c, dc, left, bottom - 1))
            {
                return FALSE;
            }
            return handle_NtGdiLineTo(c, dc, left, top);
        }

        BOOL handle_NtGdiPatBlt(const syscall_context& c, const hdc dc, const LONG x, const LONG y, const LONG width, const LONG height,
                                const DWORD /*rop*/)
        {
            gdi_dc_state* dc_state = nullptr;
            gdi_bitmap_surface* surface = nullptr;
            int32_t origin_x = 0;
            int32_t origin_y = 0;
            if (!get_dc_state_and_surface(c, dc, dc_state, surface, origin_x, origin_y) || !dc_state || !surface)
            {
                return FALSE;
            }

            const auto color = get_dc_brush_color(c, dc);
            for (int yy = 0; yy < height; ++yy)
            {
                for (int xx = 0; xx < width; ++xx)
                {
                    set_surface_pixel(*surface, x + xx + origin_x, y + yy + origin_y, color);
                }
            }
            return TRUE;
        }

        BOOL handle_NtGdiPolyPatBlt(const syscall_context& c, const hdc dc, const DWORD /*rop*/, const emulator_pointer poly,
                                    const DWORD count, const DWORD /*mode*/)
        {
            gdi_dc_state* dc_state = nullptr;
            gdi_bitmap_surface* surface = nullptr;
            int32_t origin_x = 0;
            int32_t origin_y = 0;
            if (!get_dc_state_and_surface(c, dc, dc_state, surface, origin_x, origin_y) || !surface)
            {
                return FALSE;
            }

            if (poly == 0 || count == 0 || count > 0x1000)
            {
                return FALSE;
            }

            // POLYPATBLT entry (verified at runtime against this build's gdi32): { int x; int y; int cx; int cy; HBRUSH hbr; }
            // i.e. position + size, NOT a RECT with right/bottom. Button frame draws pass thin edges such as
            // {x=96, y=4, cx=1, cy=20}, which only make sense as width/height. HBRUSH is at +0x10 on x64.
            constexpr uint64_t k_entry_size = 0x18;
            constexpr uint64_t k_brush_offset = 0x10;
            for (DWORD i = 0; i < count; ++i)
            {
                const auto entry = poly + static_cast<uint64_t>(i) * k_entry_size;
                std::array<int32_t, 4> rect{}; // x, y, cx, cy
                uint64_t brush = 0;
                if (!c.win_emu.memory.try_read_memory(entry, rect.data(), rect.size() * sizeof(int32_t)) ||
                    !c.win_emu.memory.try_read_memory(entry + k_brush_offset, &brush, sizeof(brush)))
                {
                    return FALSE;
                }

                const auto color = brush != 0 ? get_brush_color(c, static_cast<uint32_t>(brush)) : get_dc_brush_color(c, dc);
                fill_rect(*surface, rect[0] + origin_x, rect[1] + origin_y, rect[0] + rect[2] + origin_x, rect[1] + rect[3] + origin_y,
                          color);
            }
            return TRUE;
        }

        BOOL handle_NtGdiExtTextOutW(const syscall_context& c, const hdc dc, const LONG x, const LONG y, const UINT options,
                                     const emulator_pointer rect, const emulator_pointer text, const UINT count, const emulator_pointer dx,
                                     const DWORD /*code_page*/)
        {
            (void)handle_NtGdiFlush(c);

            gdi_dc_state* dc_state = nullptr;
            gdi_bitmap_surface* surface = nullptr;
            int32_t origin_x = 0;
            int32_t origin_y = 0;
            if (!get_dc_state_and_surface(c, dc, dc_state, surface, origin_x, origin_y) || !dc_state || !surface)
            {
                return FALSE;
            }

            if (count == 0 || text == 0)
            {
                dc_state->current_x = x;
                dc_state->current_y = y;
                return TRUE;
            }

            std::u16string glyphs;
            glyphs.resize(count);
            c.emu.read_memory(text, glyphs.data(), count * sizeof(char16_t));

            RECT clip_rect{};
            const auto has_rect = rect != 0 && (options & k_gdibs_no_rect) == 0;
            if (has_rect)
            {
                c.emu.read_memory(rect, &clip_rect, sizeof(clip_rect));
                clip_rect.left += origin_x;
                clip_rect.top += origin_y;
                clip_rect.right += origin_x;
                clip_rect.bottom += origin_y;
                if ((options & ETO_OPAQUE) != 0)
                {
                    fill_rect(*surface, clip_rect.left, clip_rect.top, clip_rect.right, clip_rect.bottom, get_dc_background_color(c, dc));
                }
            }

            std::vector<uint32_t> advances{};
            if (dx != 0)
            {
                advances.resize(count);
                c.emu.read_memory(dx, advances.data(), advances.size() * sizeof(uint32_t));
            }

            const auto color = get_dc_text_color(c, dc);
            draw_text(*surface, x + origin_x, y + origin_y, glyphs, color, has_rect && (options & ETO_CLIPPED) != 0 ? &clip_rect : nullptr,
                      advances.empty() ? nullptr : advances.data());

            dc_state->current_x = x + static_cast<int32_t>(count * k_text_cell_width);
            dc_state->current_y = y;
            return TRUE;
        }

        BOOL handle_NtGdiGetRealizationInfo(const syscall_context& c, const hdc dc, const emulator_pointer realization_info,
                                            const uint64_t /*font*/)
        {
            if (dc == 0)
            {
                return FALSE;
            }

            if (realization_info != 0)
            {
                std::array<uint8_t, 0x20> zeroed{};
                c.emu.write_memory(realization_info, zeroed.data(), zeroed.size());
            }

            return TRUE;
        }

        BOOL handle_NtGdiMoveToEx(const syscall_context& c, const hdc dc, const LONG x, const LONG y, const emulator_pointer old_point_ptr)
        {
            if (old_point_ptr != 0)
            {
                uint64_t dc_attr = 0;
                if (get_gdi_object_address(c, static_cast<uint32_t>(dc), k_gdi_dc_type, dc_attr))
                {
                    int32_t old_x = 0;
                    int32_t old_y = 0;
                    c.win_emu.memory.try_read_memory(dc_attr + k_gdi_dc_attr_pt_current_offset, &old_x, sizeof(old_x));
                    c.win_emu.memory.try_read_memory(dc_attr + k_gdi_dc_attr_pt_current_offset + sizeof(int32_t), &old_y, sizeof(old_y));
                    c.emu.write_memory(old_point_ptr, &old_x, sizeof(old_x));
                    c.emu.write_memory(old_point_ptr + sizeof(int32_t), &old_y, sizeof(old_y));
                }
            }
            set_dc_current_point(c, dc, x, y);
            return dc != 0 ? TRUE : FALSE;
        }

        uint64_t handle_NtGdiSelectBrushLocal(const syscall_context& c, const hdc dc, const uint32_t brush,
                                              const emulator_pointer /*old_brush_ptr*/)
        {
            uint32_t old_brush = 0;
            uint64_t dc_attr = 0;
            if (get_gdi_object_address(c, static_cast<uint32_t>(dc), k_gdi_dc_type, dc_attr))
            {
                c.win_emu.memory.try_read_memory(dc_attr + k_gdi_dc_attr_hbrush_offset, &old_brush, sizeof(old_brush));
                c.emu.write_memory(dc_attr + k_gdi_dc_attr_hbrush_offset, &brush, sizeof(brush));
            }
            return old_brush;
        }

        uint64_t handle_NtGdiSelectPenLocal(const syscall_context& c, const hdc dc, const uint32_t pen,
                                            const emulator_pointer /*old_pen_ptr*/)
        {
            uint32_t old_pen = 0;
            uint64_t dc_attr = 0;
            if (get_gdi_object_address(c, static_cast<uint32_t>(dc), k_gdi_dc_type, dc_attr))
            {
                c.win_emu.memory.try_read_memory(dc_attr + k_gdi_dc_attr_hpen_offset, &old_pen, sizeof(old_pen));
                c.emu.write_memory(dc_attr + k_gdi_dc_attr_hpen_offset, &pen, sizeof(pen));
            }
            return old_pen;
        }

        NTSTATUS handle_NtGdiGetEntry(const syscall_context& c, const uint32_t handle_value, const emulator_pointer entry_ptr)
        {
            if (entry_ptr == 0)
            {
                return STATUS_INVALID_PARAMETER;
            }

            GDI_HANDLE_ENTRY64 entry{};
            uint64_t entry_addr = 0;
            if (!read_gdi_entry_for_handle(c, handle_value, entry, entry_addr))
            {
                return STATUS_INVALID_HANDLE;
            }

            if (c.proc.is_wow64_process)
            {
                GDI_HANDLE_ENTRY32 entry32{};
                entry32.Object = static_cast<uint32_t>(entry.Object & std::numeric_limits<uint32_t>::max());
                entry32.OwnerValue = entry.Owner.Value;
                entry32.Unique = entry.Unique;
                entry32.Type = entry.Type;
                entry32.Flags = entry.Flags;
                entry32.UserPointer = static_cast<uint32_t>(entry.UserPointer & std::numeric_limits<uint32_t>::max());
                c.emu.write_memory(entry_ptr, &entry32, sizeof(entry32));
                return STATUS_SUCCESS;
            }

            c.emu.write_memory(entry_ptr, &entry, sizeof(entry));
            return STATUS_SUCCESS;
        }

        NTSTATUS write_default_dxgk_adapter_array(const syscall_context& c, const UINT32 num_adapters, const UINT64 adapters_ptr)
        {
            if (adapters_ptr == 0)
            {
                return STATUS_INVALID_PARAMETER;
            }

            if (num_adapters < k_dxgk_adapter_count)
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            const auto adapter_info = make_default_dxgk_adapter_info();
            c.emu.write_memory(adapters_ptr, &adapter_info, sizeof(adapter_info));

            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtGdiDdDDIEnumAdapters2(const syscall_context& c, const emulator_object<EMU_D3DKMT_ENUMADAPTERS2> enum_adapters)
        {
            if (!enum_adapters)
            {
                return STATUS_INVALID_PARAMETER;
            }

            NTSTATUS status = STATUS_SUCCESS;
            enum_adapters.access([&](EMU_D3DKMT_ENUMADAPTERS2& desc) {
                if (desc.pAdapters == 0)
                {
                    desc.NumAdapters = k_dxgk_adapter_count;
                    return;
                }

                status = write_default_dxgk_adapter_array(c, desc.NumAdapters, desc.pAdapters);
                if (status != STATUS_SUCCESS)
                {
                    return;
                }

                desc.NumAdapters = k_dxgk_adapter_count;
            });

            return status;
        }

        NTSTATUS handle_NtDxgkEnumAdapters3(const syscall_context& c, const emulator_object<EMU_D3DKMT_ENUMADAPTERS3> enum_adapters)
        {
            if (!enum_adapters)
            {
                return STATUS_INVALID_PARAMETER;
            }

            NTSTATUS status = STATUS_SUCCESS;
            enum_adapters.access([&](EMU_D3DKMT_ENUMADAPTERS3& desc) {
                if (desc.pAdapters == 0)
                {
                    desc.NumAdapters = k_dxgk_adapter_count;
                    return;
                }

                status = write_default_dxgk_adapter_array(c, desc.NumAdapters, desc.pAdapters);
                if (status != STATUS_SUCCESS)
                {
                    return;
                }

                desc.NumAdapters = k_dxgk_adapter_count;
            });

            return status;
        }

        NTSTATUS handle_NtDxgkGetProperties(const syscall_context& c, const emulator_object<EMU_D3DKMT_GET_PROPERTIES> get_properties)
        {
            if (!get_properties)
            {
                return STATUS_INVALID_PARAMETER;
            }

            auto props = get_properties.read();

            if (props.PropertyId == 1 || props.PropertyId == 2)
            {
                struct EMU_PREFERRED_ADAPTER_INFO
                {
                    UINT32 LuidLow;
                    UINT32 LuidHigh;
                    UINT32 SourceId;
                    UINT32 Flags;
                } info{};

                if (props.Size < sizeof(info) || !props.pBuffer)
                {
                    return STATUS_INVALID_PARAMETER;
                }

                info.LuidLow = k_dxgk_adapter_luid.LowPart;
                info.LuidHigh = k_dxgk_adapter_luid.HighPart;
                info.SourceId = 0;
                info.Flags = 1;

                c.emu.write_memory(props.pBuffer, &info, sizeof(info));
                return STATUS_SUCCESS;
            }

            dxgk_error(c, "NtDxgkGetProperties: Unknown PropertyId %d", props.PropertyId);
            return STATUS_NOT_SUPPORTED;
        }

        NTSTATUS handle_NtGdiDdDDICloseAdapter()
        {
            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtGdiDdDDIQueryAdapterInfo(const syscall_context& c,
                                                   const emulator_object<EMU_D3DKMT_QUERYADAPTERINFO> query_adapter)
        {
            if (!query_adapter)
            {
                return STATUS_INVALID_PARAMETER;
            }

            auto query = query_adapter.read();

            if (!query.pPrivateDriverData)
            {
                return STATUS_INVALID_PARAMETER;
            }

            if (query.hAdapter != k_dxgk_adapter_handle)
            {
                dxgk_warn(c, "NtGdiDdDDIQueryAdapterInfo: Querying unknown adapter handle 0x%X", query.hAdapter);
            }

            switch (query.Type)
            {
            case KMTQAITYPE::KMTQAITYPE_UMDRIVERNAME: {
                if (query.PrivateDriverDataSize < 520) // MAX_PATH * 2
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                struct EMU_D3DKMT_UMDRIVERNAME
                {
                    uint32_t Version;
                    char16_t UhDriverName[260]; // NOLINT
                } driver_name{};

                utils::string::copy(driver_name.UhDriverName, u"d3d10warp.dll");

                c.emu.write_memory(query.pPrivateDriverData, &driver_name, sizeof(driver_name));
                return STATUS_SUCCESS;
            }

            case KMTQAITYPE::KMTQAITYPE_GETSEGMENTSIZE: {
                if (query.PrivateDriverDataSize == 24)
                {
                    struct EMU_D3DKMT_SEGMENTSIZEINFO_ONLY
                    {
                        UINT64 DedicatedVideoMemorySize;
                        UINT64 DedicatedSystemMemorySize;
                        UINT64 SharedSystemMemorySize;
                    } segment_info{};

                    segment_info.DedicatedVideoMemorySize = k_dxgk_dedicated_video_memory_size;
                    segment_info.DedicatedSystemMemorySize = 0;
                    segment_info.SharedSystemMemorySize = k_dxgk_shared_system_memory_size;

                    c.emu.write_memory(query.pPrivateDriverData, &segment_info, sizeof(segment_info));
                    return STATUS_SUCCESS;
                }

                if (query.PrivateDriverDataSize >= 32)
                {
                    struct EMU_D3DKMT_QUERYSEGMENT
                    {
                        UINT32 PhysicalAdapterIndex;
                        UINT32 Padding;
                        UINT64 DedicatedVideoMemorySize;
                        UINT64 DedicatedSystemMemorySize;
                        UINT64 SharedSystemMemorySize;
                    } segment_data{};

                    segment_data.DedicatedVideoMemorySize = k_dxgk_dedicated_video_memory_size;
                    segment_data.DedicatedSystemMemorySize = 0;
                    segment_data.SharedSystemMemorySize = k_dxgk_shared_system_memory_size;

                    c.emu.write_memory(query.pPrivateDriverData, &segment_data, sizeof(segment_data));
                    return STATUS_SUCCESS;
                }

                return STATUS_BUFFER_TOO_SMALL;
            }

            case KMTQAITYPE::KMTQAITYPE_ADAPTERGUID: {
                GUID adapter_guid = k_dxgk_adapter_guid;
                return write_query_adapter_info(c, query, adapter_guid);
            }

            case KMTQAITYPE::KMTQAITYPE_FLIPQUEUEINFO: {
                struct EMU_D3DKMT_FLIPQUEUEINFO
                {
                    UINT32 MaxHardwareFlipQueueLength;
                    UINT32 MaxSoftwareFlipQueueLength;
                    UINT32 FlipFlags;
                } flip_queue_info{};

                if (query.PrivateDriverDataSize < sizeof(flip_queue_info))
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                flip_queue_info.MaxHardwareFlipQueueLength = 0;
                flip_queue_info.MaxSoftwareFlipQueueLength = 0x1F;
                flip_queue_info.FlipFlags = 0;

                return write_query_adapter_info(c, query, flip_queue_info);
            }

            case KMTQAITYPE::KMTQAITYPE_DRIVERVERSION: {
                return write_query_adapter_info(c, query, 3200);
            }

            case KMTQAITYPE::KMTQAITYPE_WDDM_1_2_CAPS: {
                struct EMU_D3DKMT_WDDM_1_2_CAPS
                {
                    UINT32 Value0;
                    UINT32 Value1;
                    UINT32 Value2;
                } caps{};

                if (query.PrivateDriverDataSize < sizeof(caps))
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                caps.Value0 = 0x190;
                caps.Value1 = 0x0C8;
                caps.Value2 = 0x1DF;

                return write_query_adapter_info(c, query, caps);
            }

            case KMTQAITYPE::KMTQAITYPE_WDDM_1_3_CAPS: {
                return write_query_adapter_info(c, query, 0x34u);
            }

            case KMTQAITYPE::KMTQAITYPE_ADAPTERTYPE: {
                return write_query_adapter_info(c, query, 3);
            }

            case KMTQAITYPE::KMTQAITYPE_PHYSICALADAPTERCOUNT: {
                return write_query_adapter_info(c, query, 1);
            }

            case KMTQAITYPE::KMTQAITYPE_PHYSICALADAPTERDEVICEIDS: {
                struct EMU_D3DKMT_PHYSICAL_ADAPTER_DEVICE_IDS
                {
                    UINT32 VendorID;
                    UINT32 DeviceID;
                    UINT32 SubSystemID;
                    UINT32 RevisionID;
                    UINT32 BusNumber;
                    UINT32 DeviceNumber;
                    UINT32 FunctionNumber;
                } ids{};

                ids.VendorID = k_dxgk_fake_vendor_id;
                ids.DeviceID = k_dxgk_fake_device_id;
                ids.SubSystemID = 0;
                ids.RevisionID = k_dxgk_fake_revision_id;
                ids.BusNumber = 0;
                ids.DeviceNumber = 0;
                ids.FunctionNumber = 0;

                return write_query_adapter_info(c, query, ids);
            }

            case KMTQAITYPE::KMTQAITYPE_ADAPTERTYPE_RENDER: {
                return write_query_adapter_info(c, query, 1);
            }

            case KMTQAITYPE::KMTQAITYPE_QUERY_ADAPTER_UNIQUE_GUID: {
                GUID unique_guid = k_dxgk_adapter_guid;
                return write_query_adapter_info(c, query, unique_guid);
            }

            default: {
                dxgk_warn(c, "NtGdiDdDDIQueryAdapterInfo: Unhandled query Type %d", static_cast<UINT32>(query.Type));

                if (query.PrivateDriverDataSize != 0)
                {
                    std::vector<uint8_t> zeros(query.PrivateDriverDataSize, 0);
                    c.emu.write_memory(query.pPrivateDriverData, zeros.data(), zeros.size());
                }

                return STATUS_SUCCESS;
            }
            }
        }

        NTSTATUS handle_NtGdiDdDDICreateDevice(const syscall_context& c, const emulator_object<EMU_D3DKMT_CREATEDEVICE> device_desc)
        {
            if (!device_desc)
            {
                return STATUS_INVALID_PARAMETER;
            }

            device_desc.access([&](EMU_D3DKMT_CREATEDEVICE& create_device) {
                if (create_device.hAdapter != k_dxgk_adapter_handle)
                {
                    dxgk_warn(c, "NtGdiDdDDICreateDevice: Invalid Adapter Handle 0x%X", create_device.hAdapter);
                }

                create_device.hDevice = k_dxgk_device_handle;
                create_device.pCommandBuffer = 0;
                create_device.CommandBufferSize = 0;

                dxgk_info(c, "NtGdiDdDDICreateDevice: Created Device Handle 0x%X on Adapter 0x%X", create_device.hDevice,
                          create_device.hAdapter);
            });

            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtGdiDdDDIEscape(const syscall_context& c, const emulator_object<EMU_D3DKMT_ESCAPE> escape_desc)
        {
            if (!escape_desc)
            {
                return STATUS_INVALID_PARAMETER;
            }

            auto escape = escape_desc.read();

            if (escape.hAdapter != k_dxgk_adapter_handle)
            {
                dxgk_warn(c, "NtGdiDdDDIEscape: Unknown Adapter 0x%X", escape.hAdapter);
            }

            if (escape.Type == 0 && escape.pPrivateDriverData != 0 && escape.PrivateDriverDataSize >= 4)
            {
                const auto command = c.emu.read_memory<uint32_t>(escape.pPrivateDriverData);

                dxgk_info(c, "NtGdiDdDDIEscape: Cmd 0x%X, Size %d", command, escape.PrivateDriverDataSize);

                switch (command)
                {
                case 1: {
                    if (escape.PrivateDriverDataSize >= 16)
                    {
                        struct WARP_CMD_GET_INFO
                        {
                            UINT32 Command;
                            UINT32 Status;
                            UINT32 LuidLow;
                            UINT32 LuidHigh;
                        } info{};

                        info.Command = command;
                        info.Status = 0;
                        info.LuidLow = 5;
                        info.LuidHigh = 1;

                        c.emu.write_memory(escape.pPrivateDriverData, &info, sizeof(info));
                        return STATUS_SUCCESS;
                    }
                    break;
                }

                case 3:
                    return STATUS_SUCCESS;

                case 4: {
                    if (escape.PrivateDriverDataSize >= 8)
                    {
                        auto buffer = c.emu.read_memory(escape.pPrivateDriverData, escape.PrivateDriverDataSize);

                        uint32_t status = 0;
                        if (buffer.size() >= 8)
                        {
                            std::memcpy(&buffer[4], &status, sizeof(status));
                            c.emu.write_memory(escape.pPrivateDriverData, buffer.data(), buffer.size());
                        }
                    }
                    return STATUS_SUCCESS;
                }

                case 0:
                    if (escape.PrivateDriverDataSize >= 56)
                    {
                        int one = 1;
                        c.emu.write_memory(escape.pPrivateDriverData + 28, &one, sizeof(one));
                        return STATUS_SUCCESS;
                    }
                    break;

                default: {
                    if (escape.PrivateDriverDataSize >= 8)
                    {
                        const auto current_val = c.emu.read_memory<uint32_t>(escape.pPrivateDriverData + 4);
                        if (current_val != 0)
                        {
                            uint32_t success = 0;
                            c.emu.write_memory(escape.pPrivateDriverData + 4, &success, sizeof(success));
                        }
                    }
                    break;
                }
                }
            }

            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtGdiDdDDICreateContext(const syscall_context& c, const emulator_object<EMU_D3DKMT_CREATECONTEXT> context_desc)
        {
            if (!context_desc)
            {
                return STATUS_INVALID_PARAMETER;
            }

            context_desc.access([&](EMU_D3DKMT_CREATECONTEXT& create_context) {
                if (create_context.hDevice != k_dxgk_device_handle)
                {
                    dxgk_warn(c, "NtGdiDdDDICreateContext: Invalid Device Handle 0x%X", create_context.hDevice);
                }

                create_context.hContext = k_dxgk_context_handle;

                const auto cmd_buffer_size = k_dxgk_command_buffer_size;
                const auto cmd_buffer_ptr = c.win_emu.memory.allocate_memory(cmd_buffer_size, memory_permission::read_write);

                std::vector<uint8_t> zeros(cmd_buffer_size, 0);
                c.win_emu.memory.write_memory(cmd_buffer_ptr, zeros.data(), cmd_buffer_size);

                create_context.pCommandBuffer = cmd_buffer_ptr;
                create_context.CommandBufferSize = cmd_buffer_size;
                create_context.pAllocationList = 0;
                create_context.AllocationListSize = 0;
                create_context.pPatchLocationList = 0;
                create_context.PatchLocationListSize = 0;

                dxgk_info(c, "NtGdiDdDDICreateContext: Created Context 0x%X on Device 0x%X (CmdBuf: 0x%llX)", create_context.hContext,
                          create_context.hDevice, cmd_buffer_ptr);
            });

            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtGdiDdDDICreateAllocation(const syscall_context& c,
                                                   const emulator_object<EMU_D3DKMT_CREATEALLOCATION> allocation_desc)
        {
            if (!allocation_desc)
            {
                return STATUS_INVALID_PARAMETER;
            }

            allocation_desc.access([&](EMU_D3DKMT_CREATEALLOCATION& create_alloc) {
                if (create_alloc.hDevice != k_dxgk_device_handle)
                {
                    dxgk_warn(c, "NtGdiDdDDICreateAllocation: Unexpected Device Handle 0x%X", create_alloc.hDevice);
                }

                if (create_alloc.hResource == 0)
                {
                    create_alloc.hResource = c.proc.dxgk.create_resource();
                }

                if (create_alloc.NumAllocations > 0 && create_alloc.pAllocationInfo != 0)
                {
                    for (UINT32 allocation_index = 0; allocation_index < create_alloc.NumAllocations; ++allocation_index)
                    {
                        constexpr size_t info_size = sizeof(EMU_D3DDDI_ALLOCATIONINFO);
                        const emulator_pointer current_info_ptr = create_alloc.pAllocationInfo + (allocation_index * info_size);
                        const emulator_object<EMU_D3DDDI_ALLOCATIONINFO> allocation_info{c.emu, current_info_ptr};

                        allocation_info.access([&](EMU_D3DDDI_ALLOCATIONINFO& alloc_info) {
                            const uint64_t backing_size = infer_warp_allocation_size_from_private_data(c, alloc_info.pPrivateDriverData,
                                                                                                       alloc_info.PrivateDriverDataSize);

                            alloc_info.hAllocation = c.proc.dxgk.create_allocation(c.win_emu.memory, create_alloc.hResource, backing_size);

                            const auto* allocation = c.proc.dxgk.get_allocation(alloc_info.hAllocation);
                            const auto actual_size = allocation ? allocation->backing_size : 0ull;
                            const auto backing_memory = allocation ? allocation->backing_memory : 0ull;

                            dxgk_info(c, "NtGdiDdDDICreateAllocation: Alloc %u/%u -> Handle 0x%X Size=0x%llX Address=0x%llX",
                                      allocation_index + 1, create_alloc.NumAllocations, alloc_info.hAllocation, actual_size,
                                      backing_memory);
                        });
                    }
                }
            });

            return STATUS_SUCCESS;
        }

        bool write_warp_open_resource_resource_private_data(const syscall_context& c, const uint64_t buffer, const uint32_t buffer_size,
                                                            const uint64_t allocation_private)
        {
            if (buffer == 0 || buffer_size < k_dxgk_open_resource_resource_private_size)
            {
                return false;
            }

            std::vector<uint8_t> resource_private_data(k_dxgk_open_resource_resource_private_size, 0);
            std::memcpy(resource_private_data.data() + 0x08, &allocation_private, sizeof(allocation_private));

            c.emu.write_memory(buffer, resource_private_data.data(), resource_private_data.size());

            dxgk_info(c, "NtGdiDdDDIOpenResource: resource private data=0x%llX alloc_private=0x%llX size=0x%X", buffer, allocation_private,
                      buffer_size);
            return true;
        }

        bool write_warp_open_resource_allocation_private_data(const syscall_context& c, const uint64_t buffer, const uint32_t buffer_size)
        {
            if (buffer == 0 || buffer_size < k_dxgk_open_resource_total_private_size)
            {
                return false;
            }

            std::vector<uint8_t> private_data(k_dxgk_open_resource_total_private_size, 0);

            const uint64_t allocation_private = buffer;
            const uint64_t descriptor = buffer + k_dxgk_open_resource_allocation_private_size;

            const auto write_private = [&](const size_t offset, const auto value) {
                std::memcpy(private_data.data() + offset, &value, sizeof(value));
            };

            write_private(0x08, descriptor);

            const size_t descriptor_offset = k_dxgk_open_resource_allocation_private_size;
            write_private(descriptor_offset + 0x00, 2u);
            write_private(descriptor_offset + 0x04, k_default_width);
            write_private(descriptor_offset + 0x08, k_default_height);
            write_private(descriptor_offset + 0x0C, 1u);
            write_private(descriptor_offset + 0x10, 0x57u);
            write_private(descriptor_offset + 0x1C, 1u);
            write_private(descriptor_offset + 0x20, 1u);

            c.emu.write_memory(buffer, private_data.data(), private_data.size());

            dxgk_info(c, "NtGdiDdDDIOpenResource: allocation private data=0x%llX descriptor=0x%llX size=0x%X", allocation_private,
                      descriptor, buffer_size);
            return true;
        }

        NTSTATUS handle_NtGdiDdDDIQueryResourceInfo(const syscall_context& c,
                                                    const emulator_object<EMU_D3DKMT_QUERYRESOURCEINFO> resource_info)
        {
            if (!resource_info)
            {
                return STATUS_INVALID_PARAMETER;
            }

            resource_info.access([&](EMU_D3DKMT_QUERYRESOURCEINFO& params) {
                if (params.hDevice != 0 && params.hDevice != k_dxgk_device_handle)
                {
                    dxgk_warn(c, "NtGdiDdDDIQueryResourceInfo: Unexpected device 0x%X", params.hDevice);
                }

                if (params.hGlobalShare != k_dxgk_shared_primary_handle && params.hGlobalShare <= 0x8000)
                {
                    dxgk_warn(c, "NtGdiDdDDIQueryResourceInfo: Unexpected global share 0x%X", params.hGlobalShare);
                }

                if (params.pPrivateRuntimeData != 0 && params.PrivateRuntimeDataSize != 0)
                {
                    std::vector<uint8_t> zeros(params.PrivateRuntimeDataSize, 0);
                    c.emu.write_memory(params.pPrivateRuntimeData, zeros.data(), zeros.size());
                }

                params.PrivateRuntimeDataSize = 0;
                params.TotalPrivateDriverDataSize = k_dxgk_open_resource_total_private_size;
                params.ResourcePrivateDriverDataSize = k_dxgk_open_resource_resource_private_size;
                params.NumAllocations = 1;
            });

            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtGdiDdDDIOpenResource(const syscall_context& c, const emulator_object<EMU_D3DKMT_OPENRESOURCE> open_resource)
        {
            if (!open_resource)
            {
                return STATUS_INVALID_PARAMETER;
            }

            NTSTATUS status = STATUS_SUCCESS;

            open_resource.access([&](EMU_D3DKMT_OPENRESOURCE& params) {
                if (params.hDevice != 0 && params.hDevice != k_dxgk_device_handle)
                {
                    dxgk_warn(c, "NtGdiDdDDIOpenResource: Unexpected device 0x%X", params.hDevice);
                }

                if (params.hGlobalShare != k_dxgk_shared_primary_handle && params.hGlobalShare <= 0x8000)
                {
                    dxgk_warn(c, "NtGdiDdDDIOpenResource: Unexpected global share 0x%X", params.hGlobalShare);
                }

                if (params.pTotalPrivateDriverDataBuffer == 0 ||
                    params.TotalPrivateDriverDataBufferSize < k_dxgk_open_resource_total_private_size)
                {
                    status = STATUS_BUFFER_TOO_SMALL;
                    return;
                }

                if (params.pResourcePrivateDriverData == 0 ||
                    params.ResourcePrivateDriverDataSize < k_dxgk_open_resource_resource_private_size)
                {
                    status = STATUS_BUFFER_TOO_SMALL;
                    return;
                }

                params.hResource = c.proc.dxgk.create_resource();

                const auto backing_size = static_cast<uint64_t>(k_default_width) * static_cast<uint64_t>(k_default_height) * 4ull;
                const auto allocation_private_data = params.pTotalPrivateDriverDataBuffer;

                if (allocation_private_data != 0 &&
                    write_warp_open_resource_allocation_private_data(c, allocation_private_data, params.TotalPrivateDriverDataBufferSize))
                {
                    params.TotalPrivateDriverDataBufferSize = k_dxgk_open_resource_total_private_size;
                }

                if (params.pResourcePrivateDriverData != 0)
                {
                    write_warp_open_resource_resource_private_data(c, params.pResourcePrivateDriverData,
                                                                   params.ResourcePrivateDriverDataSize, allocation_private_data);
                }

                if (params.NumAllocations > 0 && params.pOpenAllocationInfo != 0)
                {
                    constexpr size_t info_size = sizeof(EMU_D3DDDI_OPENALLOCATIONINFO2);

                    for (UINT32 allocation_index = 0; allocation_index < params.NumAllocations; ++allocation_index)
                    {
                        const emulator_pointer info_ptr = params.pOpenAllocationInfo + (allocation_index * info_size);
                        const emulator_object<EMU_D3DDDI_OPENALLOCATIONINFO2> allocation_info{c.emu, info_ptr};

                        allocation_info.access([&](EMU_D3DDDI_OPENALLOCATIONINFO2& alloc_info) {
                            alloc_info.hAllocation = c.proc.dxgk.create_allocation(c.win_emu.memory, params.hResource, backing_size);
                            alloc_info.pPrivateDriverData = allocation_private_data;
                            alloc_info.PrivateDriverDataSize =
                                allocation_private_data != 0 ? k_dxgk_open_resource_allocation_private_size : 0;

                            const auto* allocation = c.proc.dxgk.get_allocation(alloc_info.hAllocation);
                            const auto actual_size = allocation ? allocation->backing_size : 0ull;
                            const auto backing_memory = allocation ? allocation->backing_memory : 0ull;

                            alloc_info.GpuVirtualAddress = backing_memory;
                            std::ranges::fill(alloc_info.Reserved, 0ull);

                            dxgk_info(c, "NtGdiDdDDIOpenResource: Alloc %u/%u -> Handle 0x%X Size=0x%llX Address=0x%llX",
                                      allocation_index + 1, params.NumAllocations, alloc_info.hAllocation, actual_size, backing_memory);
                        });
                    }
                }

                dxgk_info(c, "NtGdiDdDDIOpenResource: share=0x%X -> resource=0x%X allocs=%u", params.hGlobalShare, params.hResource,
                          params.NumAllocations);
            });

            return status;
        }

        NTSTATUS handle_NtGdiDdDDILock(const syscall_context& c, const emulator_object<EMU_D3DKMT_LOCK> lock_desc)
        {
            if (!lock_desc)
            {
                return STATUS_INVALID_PARAMETER;
            }

            NTSTATUS status = STATUS_SUCCESS;

            lock_desc.access([&](EMU_D3DKMT_LOCK& lock_params) {
                if (lock_params.hDevice != k_dxgk_device_handle && lock_params.hDevice != 0)
                {
                    dxgk_warn(c, "NtGdiDdDDILock: Locking on unknown device 0x%X", lock_params.hDevice);
                }

                const auto* allocation = c.proc.dxgk.get_allocation(lock_params.hAllocation);
                if (allocation == nullptr)
                {
                    dxgk_warn(c, "NtGdiDdDDILock: Unknown allocation handle 0x%X", lock_params.hAllocation);
                    status = STATUS_INVALID_HANDLE;
                    return;
                }

                lock_params.pData = allocation->backing_memory;
                lock_params.GpuVirtualAddress = allocation->backing_memory;

                dxgk_info(c, "NtGdiDdDDILock: Handle 0x%X -> Address=0x%llX Size=0x%llX", lock_params.hAllocation,
                          allocation->backing_memory, allocation->backing_size);
            });

            return status;
        }

        NTSTATUS handle_NtGdiDdDDIGetDisplayModeList(const syscall_context& c,
                                                     const emulator_object<EMU_D3DKMT_GETDISPLAYMODELIST> display_mode_list)
        {
            if (!display_mode_list)
            {
                return STATUS_INVALID_PARAMETER;
            }

            NTSTATUS status = STATUS_SUCCESS;

            display_mode_list.access([&](EMU_D3DKMT_GETDISPLAYMODELIST& params) {
                if (params.hAdapter != k_dxgk_adapter_handle)
                {
                    dxgk_warn(c, "NtGdiDdDDIGetDisplayModeList: Unknown adapter 0x%X", params.hAdapter);
                }

                const auto capacity = params.ModeCount;
                params.ModeCount = 1;

                if (params.pModeList == 0)
                {
                    return;
                }

                if (capacity < 1)
                {
                    status = STATUS_BUFFER_TOO_SMALL;
                    return;
                }

                const emulator_object<EMU_D3DKMT_DISPLAYMODE> mode{c.emu, params.pModeList};
                mode.access([](EMU_D3DKMT_DISPLAYMODE& current_mode) {
                    current_mode.Width = k_default_width;
                    current_mode.Height = k_default_height;
                    current_mode.Format = 22;
                    current_mode.IntegerRefreshRate = 60;
                    current_mode.RefreshRate = {.Numerator = 60, .Denominator = 1};
                    current_mode.ScanLineOrdering = 1;
                    current_mode.DisplayOrientation = 1;
                    current_mode.DisplayFixedOutput = 0;
                    current_mode.Flags = 0;
                });
            });

            return status;
        }

        NTSTATUS handle_NtGdiDdDDIGetSharedPrimaryHandle(const syscall_context& c,
                                                         const emulator_object<EMU_D3DKMT_GETSHAREDPRIMARYHANDLE> shared_primary)
        {
            if (!shared_primary)
            {
                return STATUS_INVALID_PARAMETER;
            }

            shared_primary.access([&](EMU_D3DKMT_GETSHAREDPRIMARYHANDLE& params) {
                if (params.hAdapter != k_dxgk_adapter_handle)
                {
                    dxgk_warn(c, "NtGdiDdDDIGetSharedPrimaryHandle: Unknown adapter 0x%X", params.hAdapter);
                }

                params.hSharedPrimary = k_dxgk_shared_primary_handle;
            });

            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtGdiDdDDIGetDeviceState(const syscall_context& c, const emulator_object<EMU_D3DKMT_GETDEVICESTATE> device_state)
        {
            if (!device_state)
            {
                return STATUS_INVALID_PARAMETER;
            }

            device_state.access([&](EMU_D3DKMT_GETDEVICESTATE& state) {
                if (state.hDevice != k_dxgk_device_handle)
                {
                    dxgk_warn(c, "NtGdiDdDDIGetDeviceState: Unknown device 0x%X", state.hDevice);
                }

                state.State = 0;
            });

            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtGdiDdDDIMarkDeviceAsError(const syscall_context& c,
                                                    const emulator_object<EMU_D3DKMT_MARKDEVICEASERROR> mark_error)
        {
            if (!mark_error)
            {
                return STATUS_INVALID_PARAMETER;
            }

            mark_error.access([&](const EMU_D3DKMT_MARKDEVICEASERROR& params) {
                if (params.hDevice != 0 && params.hDevice != k_dxgk_device_handle)
                {
                    dxgk_warn(c, "NtGdiDdDDIMarkDeviceAsError: Unexpected device 0x%X", params.hDevice);
                }

                dxgk_info(c, "NtGdiDdDDIMarkDeviceAsError: device=0x%X reason=0x%X", params.hDevice, params.Reason);
            });

            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtGdiDdDDIGetCachedHybridQueryValue(const syscall_context&, const emulator_object<uint32_t> value)
        {
            if (value)
            {
                value.write(3);
            }
            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtGdiDdDDICacheHybridQueryValue()
        {
            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtGdiDdDDIUnlock()
        {
            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtGdiDdDDIDestroyAllocation2(const syscall_context& c,
                                                     const emulator_object<EMU_D3DKMT_DESTROYALLOCATION2> destroy_allocation)
        {
            return destroy_dxgk_allocations(c, destroy_allocation);
        }

        NTSTATUS handle_NtGdiDdDDIDestroyAllocation(const syscall_context& c,
                                                    const emulator_object<EMU_D3DKMT_DESTROYALLOCATION> destroy_allocation)
        {
            return destroy_dxgk_allocations(c, destroy_allocation);
        }

        NTSTATUS handle_NtGdiDdDDIDestroyContext()
        {
            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtGdiDdDDIDestroyDevice()
        {
            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtGdiDdDDIOpenAdapterFromHdc(const syscall_context&,
                                                     const emulator_object<EMU_D3DKMT_OPENADAPTERFROMHDC> open_adapter)
        {
            if (!open_adapter)
            {
                return STATUS_INVALID_PARAMETER;
            }

            open_adapter.access([](EMU_D3DKMT_OPENADAPTERFROMHDC& open_params) {
                open_params.hAdapter = k_dxgk_adapter_handle;
                open_params.AdapterLuid = k_dxgk_adapter_luid;
                open_params.VidPnSourceId = 0;
            });

            return STATUS_SUCCESS;
        }
    }

} // namespace sogen
