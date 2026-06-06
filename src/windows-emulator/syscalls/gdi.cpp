#include "../std_include.hpp"
#include "../debug_font.hpp"
#include "../emulator_utils.hpp"
#include "../syscall_utils.hpp"

#include <array>
#include <bit>
#include <limits>

namespace sogen
{

    namespace syscalls
    {
        namespace
        {
            struct GDI_HANDLE_ENTRY32
            {
                uint32_t Object;
                uint32_t OwnerValue;
                USHORT Unique;
                UCHAR Type;
                UCHAR Flags;
                uint32_t UserPointer;
            };
            static_assert(sizeof(GDI_HANDLE_ENTRY32) == 0x10);

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
                    surface.pixels.assign(static_cast<size_t>(width) * height, 0xFFFFFFFFu);
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
                case 0x28: // PLANES
                case 0x2A: // NUMBRUSHES
                case 0x2C: // NUMPENS
                    return 1;
                case 0x58: // LOGPIXELSX
                case 0x5A: // LOGPIXELSY
                    return k_default_dpi;
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
        }

        // Returns the surface a paint DC should be presented to, and (via present_handle) the host window handle it
        // belongs to (the top-level window for child controls). Used by NtUserEndPaint to flush guest paint output.
        gdi_bitmap_surface* get_dc_present_surface(const syscall_context& c, const hdc dc, uint32_t& present_handle)
        {
            int32_t origin_x = 0;
            int32_t origin_y = 0;
            return resolve_dc_surface(c, dc, origin_x, origin_y, present_handle);
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
                    fill_rect(*surface, clip_rect.left, clip_rect.top, clip_rect.right, clip_rect.bottom, get_dc_brush_color(c, dc));
                }
            }

            std::vector<uint32_t> advances{};
            if (dx != 0)
            {
                advances.resize(count);
                c.emu.read_memory(dx, advances.data(), advances.size() * sizeof(uint32_t));
            }

            const auto color = get_dc_pen_color(c, dc);
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
    }

} // namespace sogen
