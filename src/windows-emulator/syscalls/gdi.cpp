#include "../std_include.hpp"
#include "../debug_font.hpp"
#include "../emulated_display_adapter.hpp"
#include "../emulator_utils.hpp"
#include "../gdi_clip.hpp"
#include "../gdi_font_signature.hpp"
#include "../syscall_utils.hpp"

#include <array>
#include <bit>
#include <limits>
#include <utils/buffer_accessor.hpp>

// #define ENABLE_DXGK_LOGGING

namespace sogen
{

    namespace syscalls
    {
        uint32_t handle_NtGdiDeleteObjectApp(const syscall_context& c, uint32_t handle_value);

        namespace
        {
            constexpr uint8_t k_gdi_dc_type = 0x01;
            constexpr uint8_t k_gdi_region_type = 0x04;
            constexpr uint8_t k_gdi_bitmap_type = 0x05;
            constexpr uint8_t k_gdi_palette_type = 0x08;
            constexpr uint8_t k_gdi_font_type = 0x0A;
            constexpr uint8_t k_gdi_brush_type = 0x10;
            constexpr uint8_t k_gdi_pen_type = 0x30;
            constexpr int32_t k_gdi_dc_object_bitmap = 0x50000;
            constexpr int32_t k_gdi_dc_object_font = 0xA0000;
            constexpr int32_t k_gdi_dc_object_brush = 0x100000;
            constexpr int32_t k_gdi_dc_object_pen = 0x300000;

            constexpr uint32_t k_gdi_dc_attr_size = 0x130;
            constexpr uint32_t k_gdi_brush_attr_size = 0x20;
            constexpr uint32_t k_gdi_pen_attr_size = 0x20;
            constexpr uint32_t k_gdi_bitmap_attr_size = 0x20;
            constexpr uint32_t k_gdi_palette_attr_size = 0x20;
            constexpr uint32_t k_gdi_font_attr_size = 0x40;
            constexpr uint32_t k_gdi_region_attr_size = 0x20;

            constexpr uint32_t k_max_palette_entries = 0xFFFF;
            constexpr uint64_t k_log_palette_entries_offset = 0x04;
            constexpr uint32_t k_palette_object_size = sizeof(uint16_t);
            constexpr uint8_t k_palette_entry_reserved = 0x01;
            constexpr uint32_t k_clr_invalid = 0xFFFFFFFF;

            constexpr uint32_t k_gdi_pal_animate = 0;
            constexpr uint32_t k_gdi_pal_set_entries = 1;
            constexpr uint32_t k_gdi_pal_get_entries = 2;

            constexpr uint32_t k_gdi_dc_attr_hbrush_offset = 0xA0;
            constexpr uint32_t k_gdi_dc_attr_hpen_offset = 0xA8;
            constexpr uint32_t k_gdi_dc_attr_dc_brush_color_offset = 0xC4; // SetDCBrushColor
            constexpr uint32_t k_gdi_dc_attr_dc_pen_color_offset = 0xCC;   // SetDCPenColor
            constexpr uint32_t k_gdi_dc_attr_realized_background_color_offset = 0xB0;
            constexpr uint32_t k_gdi_dc_attr_background_color_offset = 0xB4; // SetBkColor/GetBkColor
            constexpr uint32_t k_gdi_dc_attr_text_color_offset = 0xBC;       // SetTextColor/GetTextColor
            constexpr uint32_t k_gdi_dc_attr_rop2_offset = 0xD4;             // DC_ATTR.jROP2 (SetROP2)
            constexpr uint32_t k_gdi_dc_attr_pt_current_offset = 0xD8;
            constexpr uint32_t k_gdi_dc_attr_text_align_offset = 0x110; // DC_ATTR.lTextAlign (SetTextAlign)
            constexpr uint32_t k_gdi_dc_attr_font_offset = 0x128;

            constexpr uint32_t k_error_invalid_handle = 6;
            constexpr uint8_t k_r2_copypen = 13;

            constexpr uint32_t k_stock_white_brush_index = 0x00;
            constexpr uint32_t k_stock_ltgray_brush_index = 0x01;
            constexpr uint32_t k_stock_gray_brush_index = 0x02;
            constexpr uint32_t k_stock_dkgray_brush_index = 0x03;
            constexpr uint32_t k_stock_black_brush_index = 0x04;
            constexpr uint32_t k_stock_null_brush_index = 0x05;
            constexpr uint32_t k_stock_white_pen_index = 0x06;
            constexpr uint32_t k_stock_black_pen_index = 0x07;
            constexpr uint32_t k_stock_null_pen_index = 0x08;
            constexpr uint32_t k_stock_system_font_index = 0x0D;
            constexpr uint32_t k_stock_default_palette = 0x0F;
            constexpr uint32_t k_stock_default_gui_font_index = 0x11;
            constexpr uint32_t k_stock_dc_brush_index = 0x12;
            constexpr uint32_t k_stock_dc_pen_index = 0x13;
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

            struct font_enum_record
            {
                uint32_t size;
                uint32_t text_metric_offset;
                uint32_t type;
                EMU_ENUMLOGFONTEXW log_font;
                std::array<uint32_t, 2> log_font_design_vector{0x08007664, 0};
                std::array<uint32_t, 2> text_metric_header{};
                EMU_NEWTEXTMETRICEXW text_metric;
                std::array<uint32_t, 2> text_metric_design_vector{0x08006C61, 0};
            };

            static_assert(offsetof(font_enum_record, log_font) == 0x0C);
            static_assert(offsetof(font_enum_record, log_font_design_vector) == 0x168);
            static_assert(offsetof(font_enum_record, text_metric_header) == 0x170);
            static_assert(offsetof(font_enum_record, text_metric) == 0x178);
            static_assert(offsetof(font_enum_record, text_metric_design_vector) == 0x1DC);
            static_assert(sizeof(font_enum_record) == 0x1E4);

            constexpr uint32_t k_default_dpi = 96;
            constexpr uint32_t k_default_bitmap_fill = 0xFFF5F5F5u;

            constexpr uint32_t k_gdi_first_dynamic_handle = 0x2000;
            constexpr uint32_t k_gdi_default_cookie = STATUS_WAIT_1;

            constexpr int16_t k_gdi_batch_cmd_text_out = 2;
            constexpr int16_t k_gdi_batch_cmd_text_out_rect = 3;
            constexpr int16_t k_gdi_batch_cmd_poly_pat_blt = 1;
            constexpr int16_t k_gdi_batch_cmd_delete_object = 7;
            constexpr int16_t k_gdi_batch_cmd_delete_region = 8;
            constexpr uint32_t k_gdibs_no_rect = 0x80000000u;
            constexpr uint32_t k_ps_solid = 0;
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
                0x00000000u, // reserved
                0x00000000u, // COLOR_HOTLIGHT
                0x00CC6600u, // COLOR_GRADIENTACTIVECAPTION
                0x00F2E4D7u, // COLOR_GRADIENTINACTIVECAPTION
                0x00F0F0F0u, // COLOR_MENUHILIGHT
                0x00F0F0F0u, // COLOR_MENUBAR
            };

            // The logfonts GetObject reports for the stock fonts on Windows.
            EMU_LOGFONTW stock_font_logfont(const uint32_t index)
            {
                EMU_LOGFONTW font{};
                font.lfCharSet = DEFAULT_CHARSET;
                if (index == k_stock_system_font_index)
                {
                    font.lfHeight = 16;
                    font.lfWidth = 7;
                    font.lfWeight = FW_BOLD;
                    font.lfPitchAndFamily = VARIABLE_PITCH | FF_SWISS;
                    std::ranges::copy(u"System", font.lfFaceName);
                }
                else
                {
                    font.lfHeight = -11;
                    font.lfWeight = FW_NORMAL;
                    std::ranges::copy(u"MS Shell Dlg", font.lfFaceName);
                }
                return font;
            }

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
                UINT dx_offset{};
                uint64_t font{};
                FLONG text_align{};
                POINTL viewport_org{};

                union
                {
                    std::array<WCHAR, 2> string;
                    std::array<ULONG, 1> buffer;
                };
            };

            struct gdi_batch_text_out_rect
            {
                gdi_batch_header header{};
                COLORREF background{};
                UINT options{};
                RECT rect{};
                POINTL viewport_org{};
                ULONG background_ul{};
            };

            static_assert(offsetof(gdi_batch_text_out_rect, rect) == 0x0C);
            static_assert(sizeof(gdi_batch_text_out_rect) == 0x28);

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

            // gdi32's DeleteObject queues brushes and pens that have a user attribute as command 7 and regions as command 8.
            struct gdi_batch_delete_object
            {
                gdi_batch_header header{};
                uint32_t reserved{};
                uint64_t handle{};
            };

            static_assert(sizeof(gdi_batch_delete_object) == 0x10);

            constexpr uint32_t k_dxgk_adapter_count = 1;
            constexpr uint32_t k_dxgk_adapter_handle = 0x4000;
            constexpr uint32_t k_dxgk_device_handle = 0x5000;
            constexpr uint32_t k_dxgk_context_handle = 0x6000;
            constexpr uint32_t k_dxgk_shared_primary_handle = 0x7000;
            constexpr LUID k_dxgk_adapter_luid = {.LowPart = 0x1000, .HighPart = 0};
            // Stable emulator-only unique adapter id. Sequential, not a host device.
            constexpr GUID k_dxgk_adapter_unique_id = {
                .Data1 = 0x00000001, .Data2 = 0x0002, .Data3 = 0x0003, .Data4 = {0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01}};
            constexpr uint32_t k_dxgk_adapter_source_count = 1;
            constexpr uint32_t k_dxgk_command_buffer_size = 0x1000;
            constexpr uint32_t k_dxgk_allocation_list_entry_size = 8;
            constexpr uint32_t k_dxgk_patch_location_list_entry_size = 24;
            constexpr uint32_t k_dxgk_allocation_list_count = 512;
            constexpr uint32_t k_dxgk_patch_location_list_count = 512;
            // Upper bounds on the guest-requested submission-buffer sizes. The guest is untrusted, so cap the
            // NewCommandBufferSize / New*ListSize fields before they size a host allocation -- otherwise a
            // crafted D3DKMT_RENDER can force a multi-gigabyte host allocation, and count * entry_size can
            // overflow uint32. With these caps the byte sizes stay well under 4 GiB.
            constexpr uint32_t k_dxgk_max_command_buffer_size = 0x4000000; // 64 MiB
            constexpr uint32_t k_dxgk_max_list_count = 0x10000;            // 64k entries (<= 1.5 MiB of list bytes)
            constexpr uint64_t k_dxgk_dedicated_video_memory_size = 4ull * 1024 * 1024 * 1024;
            constexpr uint64_t k_dxgk_shared_system_memory_size = 8ull * 1024 * 1024 * 1024;
            constexpr uint32_t k_dxgk_open_resource_resource_private_size = 0x18;
            constexpr uint32_t k_dxgk_open_resource_allocation_private_size = 0x18;
            constexpr uint32_t k_dxgk_open_resource_descriptor_size = 0x80;
            constexpr uint32_t k_dxgk_open_resource_total_private_size =
                k_dxgk_open_resource_allocation_private_size + k_dxgk_open_resource_descriptor_size;

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

                if (c.proc.is_wow64_process && pointer > std::numeric_limits<uint32_t>::max())
                {
                    return 0;
                }

                // Windows 10 2004-22H2 stores UserPointer as it is: gdi32's GdiGetEntry copies the entry (x64 and WoW64)
                // and gdi32full uses the pointer directly (SelectObjectImpl reads the DC_ATTR through it).
                if (c.win_emu.version.is_build_within(19041, 19046))
                {
                    return pointer;
                }

                if (c.proc.is_wow64_process)
                {
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
                if (index >= std::extent_v<decltype(GDI_SHARED_MEMORY64::StockObjects)>)
                {
                    return 0;
                }

                return read_gdi_shared_value(c, offsetof(GDI_SHARED_MEMORY64, StockObjects) + (sizeof(uint64_t) * index));
            }

            uint64_t read_gdi_object96_slot(const syscall_context& c, const uint32_t index)
            {
                if (index >= std::extent_v<decltype(GDI_SHARED_MEMORY64::StockObjects96)>)
                {
                    return 0;
                }

                return read_gdi_shared_value(c, offsetof(GDI_SHARED_MEMORY64, StockObjects96) + (sizeof(uint64_t) * index));
            }

            uint64_t read_gdi_dpi_fonts_slot(const syscall_context& c, const uint32_t index)
            {
                if (index >= std::extent_v<decltype(GDI_SHARED_MEMORY64::DpiDepDefaultGuiFonts)>)
                {
                    return 0;
                }

                return read_gdi_shared_value(c, offsetof(GDI_SHARED_MEMORY64, DpiDepDefaultGuiFonts) + (sizeof(uint64_t) * index));
            }

            // The stock objects, the memory DC default bitmap among them, and the system color brushes belong to the
            // system on Windows: they are neither charged to the process nor deleted by DeleteObject.
            template <typename Predicate>
            bool any_system_gdi_object(const syscall_context& c, const Predicate& predicate)
            {
                if (predicate(c.proc.gdi_memory_dc_default_bitmap_handle))
                {
                    return true;
                }

                for (uint32_t index = 0; index < std::extent_v<decltype(GDI_SHARED_MEMORY64::StockObjects)>; ++index)
                {
                    if (predicate(static_cast<uint32_t>(read_gdi_object_slot(c, index))))
                    {
                        return true;
                    }
                }

                for (uint32_t index = 0; index < std::extent_v<decltype(GDI_SHARED_MEMORY64::StockObjects96)>; ++index)
                {
                    if (predicate(static_cast<uint32_t>(read_gdi_object96_slot(c, index))))
                    {
                        return true;
                    }
                }

                for (uint32_t index = 0; index < std::extent_v<decltype(GDI_SHARED_MEMORY64::DpiDepDefaultGuiFonts)>; ++index)
                {
                    if (predicate(static_cast<uint32_t>(read_gdi_dpi_fonts_slot(c, index))))
                    {
                        return true;
                    }
                }

                const auto server_info = c.proc.user_handles.get_server_info().read();
                return std::ranges::any_of(server_info.ahbrSystem,
                                           [&](const auto brush) { return predicate(static_cast<uint32_t>(brush)); });
            }

            bool is_system_gdi_object(const syscall_context& c, const uint32_t handle_value)
            {
                return any_system_gdi_object(c, [&](const uint32_t object) { return object == handle_value; });
            }

            bool write_gdi_object_slot(const syscall_context& c, const uint32_t index, const uint64_t value)
            {
                if (index >= std::extent_v<decltype(GDI_SHARED_MEMORY64::StockObjects)>)
                {
                    return false;
                }

                const auto table = ensure_gdi_shared_table(c);
                if (table == 0)
                {
                    return false;
                }

                const auto slot = table + offsetof(GDI_SHARED_MEMORY64, StockObjects) + (sizeof(uint64_t) * index);
                c.emu.write_memory(slot, &value, sizeof(value));
                return true;
            }

            bool write_gdi_object96_slot(const syscall_context& c, const uint32_t index, const uint64_t value)
            {
                if (index >= std::extent_v<decltype(GDI_SHARED_MEMORY64::StockObjects96)>)
                {
                    return false;
                }

                const auto table = ensure_gdi_shared_table(c);
                if (table == 0)
                {
                    return false;
                }

                const auto slot = table + offsetof(GDI_SHARED_MEMORY64, StockObjects96) + (sizeof(uint64_t) * index);
                c.emu.write_memory(slot, &value, sizeof(value));
                return true;
            }

            // Native processes get these above 4 GiB even where an older executable moves its own default allocation address
            // down. Backends sharing the address space with the guest can only hand memory above it to a new process.
            uint64_t gdi_allocation_start(const syscall_context& c)
            {
                return c.proc.is_wow64_process ? DEFAULT_ALLOCATION_ADDRESS_32BIT : DEFAULT_ALLOCATION_ADDRESS_64BIT;
            }

            uint64_t allocate_gdi_user_block(const syscall_context& c, const uint32_t size)
            {
                const auto aligned_size = static_cast<size_t>(page_align_up(size));
                const uint64_t base =
                    c.win_emu.memory.allocate_memory(aligned_size, memory_permission::read_write, false, gdi_allocation_start(c));
                if (base == 0)
                {
                    return 0;
                }

                if (c.proc.is_wow64_process && base > std::numeric_limits<uint32_t>::max())
                {
                    c.win_emu.memory.release_memory(base, 0);
                    return 0;
                }

                std::vector<uint8_t> zeroed(size, 0);
                c.emu.write_memory(base, zeroed.data(), zeroed.size());
                return base;
            }

            // The handle table entry holds only the base type, which gdi32's SelectObject checks against the low five
            // bits of the handle's type byte: pens and extended pens are brushes there. The full type stays in Unique.
            uint8_t gdi_base_type(const uint8_t type)
            {
                return static_cast<uint8_t>(type & 0x1Fu);
            }

            uint8_t gdi_object_type(const GDI_HANDLE_ENTRY64& entry)
            {
                return static_cast<uint8_t>(entry.Unique & 0x7Fu);
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
                        writable.Owner = c.proc.process_id;
                        writable.Unique = unique;
                        writable.Type = gdi_base_type(type);
                        writable.Flags = 0;
                        writable.UserPointer = encoded_user_ptr;
                    });

                    return handle_value;
                }

                return 0;
            }

            bool read_gdi_entry_for_handle(const syscall_context& c, uint32_t handle_value, GDI_HANDLE_ENTRY64& entry,
                                           uint64_t& entry_addr);

            uint32_t allocate_gdi_object(const syscall_context& c, const uint8_t type, const uint32_t attr_size,
                                         const bool has_user_attribute)
            {
                const uint64_t attr = allocate_gdi_user_block(c, attr_size);
                if (attr == 0)
                {
                    return 0;
                }

                const uint64_t user_ptr = has_user_attribute ? attr : 0;
                const auto handle_value = allocate_gdi_handle(c, type, user_ptr, attr);
                if (handle_value == 0)
                {
                    c.win_emu.memory.release_memory(attr, 0);
                }
                return handle_value;
            }

            uint32_t allocate_gdi_object(const syscall_context& c, const uint8_t type, const uint32_t attr_size)
            {
                return allocate_gdi_object(c, type, attr_size, type != k_gdi_font_type);
            }

            bool get_gdi_object_address(const syscall_context& c, const uint32_t handle_value, const uint8_t expected_type, uint64_t& addr)
            {
                GDI_HANDLE_ENTRY64 entry{};
                uint64_t entry_addr = 0;
                if (!read_gdi_entry_for_handle(c, handle_value, entry, entry_addr) || gdi_object_type(entry) != expected_type)
                {
                    return false;
                }

                addr = entry.Object;
                return addr != 0;
            }

            // RGNATTR. gdi32 answers simple and null regions from it in user mode and edits it in place
            // (CreateRectRgn, SetRectRgn, OffsetRgn, simple CombineRgn cases), so it is the authority for
            // those; only complex regions keep their rectangles in process state.
            struct gdi_region_attr
            {
                ULONG attr_flags;
                ULONG complexity;
                RECT bounds;
            };

            static_assert(sizeof(gdi_region_attr) <= k_gdi_region_attr_size);

            using gdi_region_span = std::pair<LONG, LONG>;

            bool is_empty_rect(const RECT& rect)
            {
                return rect.left >= rect.right || rect.top >= rect.bottom;
            }

            std::vector<RECT> rect_region(const RECT& rect)
            {
                return is_empty_rect(rect) ? std::vector<RECT>{} : std::vector<RECT>{rect};
            }

            RECT region_bounds(const std::vector<RECT>& rects)
            {
                if (rects.empty())
                {
                    return {};
                }

                auto bounds = rects.front();
                for (const auto& rect : rects)
                {
                    bounds.left = (std::min)(bounds.left, rect.left);
                    bounds.top = (std::min)(bounds.top, rect.top);
                    bounds.right = (std::max)(bounds.right, rect.right);
                    bounds.bottom = (std::max)(bounds.bottom, rect.bottom);
                }

                return bounds;
            }

            ULONG region_complexity(const std::vector<RECT>& rects)
            {
                if (rects.empty())
                {
                    return NULLREGION;
                }

                return rects.size() == 1 ? SIMPLEREGION : COMPLEXREGION;
            }

            std::optional<std::vector<RECT>> read_gdi_region(const syscall_context& c, const uint32_t region)
            {
                uint64_t attr_address = 0;
                if (!get_gdi_object_address(c, region, k_gdi_region_type, attr_address))
                {
                    return std::nullopt;
                }

                gdi_region_attr attr{};
                if (!c.win_emu.memory.try_read_memory(attr_address, &attr, sizeof(attr)))
                {
                    return std::nullopt;
                }

                switch (attr.complexity)
                {
                case NULLREGION:
                    return std::vector<RECT>{};
                case SIMPLEREGION:
                    return rect_region(attr.bounds);
                case COMPLEXREGION:
                    if (const auto it = c.proc.gdi_regions.find(region); it != c.proc.gdi_regions.end())
                    {
                        return it->second;
                    }
                    break;
                default:
                    break;
                }

                c.win_emu.log.error("Region 0x%X has unsupported complexity %u\n", region, static_cast<uint32_t>(attr.complexity));
                return std::nullopt;
            }

            int32_t store_gdi_region(const syscall_context& c, const uint32_t region, std::vector<RECT> rects)
            {
                uint64_t attr_address = 0;
                if (!get_gdi_object_address(c, region, k_gdi_region_type, attr_address))
                {
                    return RGN_ERROR;
                }

                gdi_region_attr attr{};
                if (!c.win_emu.memory.try_read_memory(attr_address, &attr, sizeof(attr)))
                {
                    return RGN_ERROR;
                }

                attr.complexity = region_complexity(rects);
                attr.bounds = region_bounds(rects);
                if (!c.win_emu.memory.try_write_memory(attr_address, &attr, sizeof(attr)))
                {
                    return RGN_ERROR;
                }

                if (attr.complexity == COMPLEXREGION)
                {
                    c.proc.gdi_regions[region] = std::move(rects);
                }
                else
                {
                    c.proc.gdi_regions.erase(region);
                }

                return static_cast<int32_t>(attr.complexity);
            }

            bool region_mode_includes(const int32_t mode, const bool in_first, const bool in_second)
            {
                switch (mode)
                {
                case RGN_AND:
                    return in_first && in_second;
                case RGN_OR:
                    return in_first || in_second;
                case RGN_XOR:
                    return in_first != in_second;
                case RGN_DIFF:
                    return in_first && !in_second;
                case RGN_COPY:
                    return in_first;
                default:
                    return false;
                }
            }

            struct gdi_region_source_rect
            {
                RECT rect;
                bool from_second;
            };

            struct gdi_region_edge
            {
                LONG x;
                bool from_second;
                int32_t depth_change;
            };

            std::vector<gdi_region_span> combine_region_band(const std::vector<gdi_region_source_rect>& band_rects, const int32_t mode)
            {
                std::vector<gdi_region_edge> edges{};
                edges.reserve(band_rects.size() * 2);
                for (const auto& [rect, from_second] : band_rects)
                {
                    edges.push_back({.x = rect.left, .from_second = from_second, .depth_change = 1});
                    edges.push_back({.x = rect.right, .from_second = from_second, .depth_change = -1});
                }

                std::ranges::sort(edges, {}, &gdi_region_edge::x);

                std::vector<gdi_region_span> spans{};
                int32_t first_depth = 0;
                int32_t second_depth = 0;
                for (size_t i = 0; i < edges.size();)
                {
                    const auto left = edges[i].x;
                    for (; i < edges.size() && edges[i].x == left; ++i)
                    {
                        (edges[i].from_second ? second_depth : first_depth) += edges[i].depth_change;
                    }

                    if (i == edges.size() || !region_mode_includes(mode, first_depth > 0, second_depth > 0))
                    {
                        continue;
                    }

                    const auto right = edges[i].x;
                    if (!spans.empty() && spans.back().second == left)
                    {
                        spans.back().second = right;
                    }
                    else
                    {
                        spans.emplace_back(left, right);
                    }
                }

                return spans;
            }

            // GetRegionData exposes the y-x banded form Windows keeps regions in.
            std::vector<RECT> combine_regions(const std::vector<RECT>& first, const std::vector<RECT>& second, const int32_t mode)
            {
                std::vector<LONG> band_edges{};
                std::vector<gdi_region_source_rect> pending{};
                const auto add_source = [&](const std::vector<RECT>& rects, const bool from_second) {
                    for (const auto& rect : rects)
                    {
                        band_edges.insert(band_edges.end(), {rect.top, rect.bottom});
                        pending.push_back({.rect = rect, .from_second = from_second});
                    }
                };

                add_source(first, false);
                add_source(second, true);

                std::ranges::sort(band_edges);
                band_edges.erase(std::ranges::unique(band_edges).begin(), band_edges.end());
                std::ranges::sort(pending, {}, [](const gdi_region_source_rect& source) { return source.rect.top; });

                std::vector<RECT> result{};
                std::vector<gdi_region_span> previous_spans{};
                size_t previous_band_start = 0;
                std::vector<gdi_region_source_rect> band_rects{};
                size_t next_pending = 0;

                for (size_t i = 0; i + 1 < band_edges.size(); ++i)
                {
                    const auto top = band_edges[i];
                    const auto bottom = band_edges[i + 1];

                    std::erase_if(band_rects, [&](const gdi_region_source_rect& source) { return source.rect.bottom <= top; });
                    for (; next_pending < pending.size() && pending[next_pending].rect.top <= top; ++next_pending)
                    {
                        band_rects.push_back(pending[next_pending]);
                    }

                    auto spans = combine_region_band(band_rects, mode);
                    if (spans.empty())
                    {
                        continue;
                    }

                    if (spans == previous_spans && result.back().bottom == top)
                    {
                        for (auto j = previous_band_start; j < result.size(); ++j)
                        {
                            result[j].bottom = bottom;
                        }
                        continue;
                    }

                    previous_band_start = result.size();
                    for (const auto& [left, right] : spans)
                    {
                        result.push_back(RECT{.left = left, .top = top, .right = right, .bottom = bottom});
                    }
                    previous_spans = std::move(spans);
                }

                return result;
            }

            void sync_surface_from_guest_dib(const syscall_context& c, gdi_bitmap_surface& surface)
            {
                if (surface.guest_bits == 0 || surface.guest_stride == 0 || surface.width == 0 || surface.height == 0)
                {
                    return;
                }

                if (surface.pixels.size() < static_cast<size_t>(surface.width) * surface.height)
                {
                    return;
                }

                if (surface.guest_bpp == 32)
                {
                    const size_t row_bytes = static_cast<size_t>(surface.width) * sizeof(uint32_t);
                    if (surface.guest_stride < row_bytes)
                    {
                        return;
                    }

                    for (uint32_t y = 0; y < surface.height; ++y)
                    {
                        const uint32_t guest_row = surface.guest_top_down ? y : (surface.height - 1 - y);
                        auto* dst = surface.pixels.data() + static_cast<size_t>(y) * surface.width;
                        c.emu.read_memory(surface.guest_bits + static_cast<uint64_t>(guest_row) * surface.guest_stride, dst, row_bytes);
                    }
                }
                else if (surface.guest_bpp == 24)
                {
                    const size_t row_bytes = static_cast<size_t>(surface.width) * 3;
                    if (surface.guest_stride < row_bytes)
                    {
                        return;
                    }

                    std::vector<uint8_t> row(row_bytes);
                    for (uint32_t y = 0; y < surface.height; ++y)
                    {
                        const uint32_t guest_row = surface.guest_top_down ? y : (surface.height - 1 - y);
                        c.emu.read_memory(surface.guest_bits + static_cast<uint64_t>(guest_row) * surface.guest_stride, row.data(),
                                          row_bytes);

                        auto* dst = surface.pixels.data() + static_cast<size_t>(y) * surface.width;
                        for (uint32_t x = 0; x < surface.width; ++x)
                        {
                            const uint8_t* px = row.data() + static_cast<size_t>(x) * 3;
                            dst[x] = static_cast<uint32_t>(px[0]) | (static_cast<uint32_t>(px[1]) << 8) |
                                     (static_cast<uint32_t>(px[2]) << 16) | 0xFF000000u;
                        }
                    }
                }
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
                    sync_surface_from_guest_dib(c, bmp_it->second);
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
                while (win && !c.proc.is_surface_root(*win))
                {
                    off_x += win->client_x();
                    off_y += win->client_y();
                    win = win->parent_handle != 0 ? c.proc.windows.get(win->parent_handle) : nullptr;
                }

                if (!win || !win->host_surface_window || win->client_width() <= 0 || win->client_height() <= 0)
                {
                    return nullptr;
                }

                const auto top_handle = static_cast<uint32_t>(win->handle);
                const auto width = static_cast<uint32_t>(win->client_width());
                const auto height = static_cast<uint32_t>(win->client_height());

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
                sync_surface_from_guest_dib(c, surface);
                return &surface;
            }

            // Visits the children front to back: the guest's child list runs from the top of the Z order down.
            template <typename Visitor>
            void for_each_child_in_z_order(const syscall_context& c, const window& parent, Visitor&& visit)
            {
                uint64_t next = 0;
                parent.guest.access([&](const USER_WINDOW& guest_parent) { next = guest_parent.spwndChild; });
                for (size_t guard = 0; next != 0 && guard < c.proc.windows.size(); ++guard)
                {
                    USER_WINDOW guest_child{};
                    c.emu.read_memory(next, &guest_child, sizeof(guest_child));
                    next = guest_child.spwndNext;
                    const auto* child = c.proc.windows.get(guest_child.hWnd);
                    if (child && !visit(*child))
                    {
                        return;
                    }
                }
            }

            RECT window_rect_in_parent(const window& win, const POINT parent_client_origin)
            {
                return {.left = parent_client_origin.x + win.x,
                        .top = parent_client_origin.y + win.y,
                        .right = parent_client_origin.x + win.x + win.width,
                        .bottom = parent_client_origin.y + win.y + win.height};
            }

            void subtract_siblings_in_front(const syscall_context& c, gdi::clip_region& visible, const window& win, const window& parent,
                                            const POINT parent_client_origin)
            {
                for_each_child_in_z_order(c, parent, [&](const window& sibling) {
                    if (&sibling == &win)
                    {
                        return false;
                    }
                    if ((sibling.style & WS_VISIBLE) != 0)
                    {
                        visible.subtract(window_rect_in_parent(sibling, parent_client_origin));
                    }
                    return true;
                });
            }

            // A window DC paints only inside the window's client area and those of its ancestors. WS_CLIPCHILDREN
            // keeps it off the window's children, and WS_CLIPSIBLINGS keeps a window off the siblings in front of it.
            gdi::clip_region window_dc_clip(const syscall_context& c, const window& target, const gdi_bitmap_surface& surface,
                                            const POINT target_client_origin)
            {
                auto visible = gdi::clip_region::whole(surface.width, surface.height);

                if ((target.style & WS_CLIPCHILDREN) != 0)
                {
                    for_each_child_in_z_order(c, target, [&](const window& child) {
                        if ((child.style & WS_VISIBLE) != 0)
                        {
                            visible.subtract(window_rect_in_parent(child, target_client_origin));
                        }
                        return true;
                    });
                }

                const auto* win = &target;
                auto origin = target_client_origin;
                while (win)
                {
                    visible.intersect({.left = origin.x,
                                       .top = origin.y,
                                       .right = origin.x + win->client_width(),
                                       .bottom = origin.y + win->client_height()});

                    const auto* parent = c.proc.is_surface_root(*win) ? nullptr : c.proc.windows.get(win->parent_handle);
                    if (!parent)
                    {
                        break;
                    }

                    const POINT parent_origin{.x = origin.x - win->client_x(), .y = origin.y - win->client_y()};
                    if ((win->style & WS_CLIPSIBLINGS) != 0)
                    {
                        subtract_siblings_in_front(c, visible, *win, *parent, parent_origin);
                    }

                    win = parent;
                    origin = parent_origin;
                }

                return visible;
            }

            // Like resolve_dc_surface, plus the part of the surface the DC may change; only for the surface a call draws into.
            gdi_bitmap_surface* resolve_dc_draw_target(const syscall_context& c, const hdc dc, int32_t& origin_x, int32_t& origin_y,
                                                       uint32_t& present_handle, gdi::clip_region& visible)
            {
                auto* surface = resolve_dc_surface(c, dc, origin_x, origin_y, present_handle);
                if (!surface)
                {
                    return nullptr;
                }

                const auto& dc_state = c.proc.gdi_dc_states.at(static_cast<uint32_t>(dc));
                const auto* target = dc_state.selected_bitmap == 0 ? c.proc.windows.get(dc_state.target_window) : nullptr;
                visible = target ? window_dc_clip(c, *target, *surface, POINT{.x = origin_x, .y = origin_y})
                                 : gdi::clip_region::whole(surface->width, surface->height);
                return surface;
            }

            bool get_dc_state_and_draw_target(const syscall_context& c, const hdc dc, gdi_dc_state*& dc_state, gdi_bitmap_surface*& surface,
                                              int32_t& origin_x, int32_t& origin_y, gdi::clip_region& visible,
                                              uint32_t* present_handle = nullptr)
            {
                dc_state = nullptr;
                surface = nullptr;
                origin_x = 0;
                origin_y = 0;
                if (present_handle != nullptr)
                {
                    *present_handle = 0;
                }

                const auto dc_it = c.proc.gdi_dc_states.find(static_cast<uint32_t>(dc));
                if (dc_it == c.proc.gdi_dc_states.end())
                {
                    return false;
                }

                dc_state = &dc_it->second;
                uint32_t resolved_present_handle = 0;
                surface = resolve_dc_draw_target(c, dc, origin_x, origin_y, resolved_present_handle, visible);
                if (present_handle != nullptr)
                {
                    *present_handle = resolved_present_handle;
                }
                return surface != nullptr;
            }

            void sync_surface_to_guest_dib(const syscall_context& c, const gdi_bitmap_surface& surface)
            {
                if (surface.guest_bits == 0 || surface.guest_stride == 0 || surface.width == 0 || surface.height == 0)
                {
                    return;
                }

                if (surface.pixels.size() < static_cast<size_t>(surface.width) * surface.height)
                {
                    return;
                }

                if (surface.guest_bpp != 24 && surface.guest_bpp != 32)
                {
                    return;
                }

                const size_t bytes_per_pixel = surface.guest_bpp / 8;
                const size_t row_bytes = static_cast<size_t>(surface.width) * bytes_per_pixel;
                if (surface.guest_stride < row_bytes)
                {
                    return;
                }

                std::vector<uint8_t> row(row_bytes);
                for (uint32_t y = 0; y < surface.height; ++y)
                {
                    const uint32_t guest_row = surface.guest_top_down ? y : (surface.height - 1 - y);
                    const auto* src = surface.pixels.data() + static_cast<size_t>(y) * surface.width;
                    for (uint32_t x = 0; x < surface.width; ++x)
                    {
                        uint8_t* px = row.data() + static_cast<size_t>(x) * bytes_per_pixel;
                        const uint32_t pixel = src[x];
                        px[0] = static_cast<uint8_t>(pixel & 0xFF);
                        px[1] = static_cast<uint8_t>((pixel >> 8) & 0xFF);
                        px[2] = static_cast<uint8_t>((pixel >> 16) & 0xFF);
                        if (bytes_per_pixel == 4)
                        {
                            px[3] = static_cast<uint8_t>((pixel >> 24) & 0xFF);
                        }
                    }
                    c.emu.write_memory(surface.guest_bits + static_cast<uint64_t>(guest_row) * surface.guest_stride, row.data(), row_bytes);
                }
            }

            void present_win_surface(const syscall_context& c, uint32_t present_handle, gdi_bitmap_surface* surface)
            {
                if (surface == nullptr)
                {
                    return;
                }

                sync_surface_to_guest_dib(c, *surface);

                if (present_handle == 0 || surface->width <= 0 || surface->height <= 0 || surface->pixels.empty())
                {
                    return;
                }

                c.win_emu.ui().present_surface(present_handle, make_window_surface_desc(*surface, c.proc.windows.get(present_handle)));
            }

            void set_surface_pixel(gdi_bitmap_surface& surface, const int x, const int y, const uint32_t color)
            {
                if (x < 0 || y < 0 || x >= static_cast<int>(surface.width) || y >= static_cast<int>(surface.height))
                {
                    return;
                }

                surface.pixels[static_cast<size_t>(y) * surface.width + static_cast<size_t>(x)] = color;
            }

            void set_clipped_pixel(gdi_bitmap_surface& surface, const gdi::clip_region& visible, const int x, const int y,
                                   const uint32_t color)
            {
                if (visible.contains(x, y))
                {
                    set_surface_pixel(surface, x, y, color);
                }
            }

            RECT clamped_to_surface(const gdi_bitmap_surface& surface, const int64_t left, const int64_t top, const int64_t right,
                                    const int64_t bottom)
            {
                return {.left = static_cast<LONG>(std::clamp<int64_t>(left, 0, surface.width)),
                        .top = static_cast<LONG>(std::clamp<int64_t>(top, 0, surface.height)),
                        .right = static_cast<LONG>(std::clamp<int64_t>(right, 0, surface.width)),
                        .bottom = static_cast<LONG>(std::clamp<int64_t>(bottom, 0, surface.height))};
            }

            uint32_t* surface_row(gdi_bitmap_surface& surface, const LONG y)
            {
                return surface.pixels.data() + static_cast<size_t>(y) * surface.width;
            }

            // BITMAPCOREHEADER is only 12 bytes and lays out its fields differently, so the BITMAPINFOHEADER
            // offsets are only valid once biSize says the header is at least that large.
            std::optional<EMU_BITMAPINFOHEADER> read_bitmap_info_header(const syscall_context& c, const emulator_pointer info)
            {
                if (c.emu.read_memory<uint32_t>(info) < sizeof(EMU_BITMAPINFOHEADER))
                {
                    return std::nullopt;
                }

                return c.emu.read_memory<EMU_BITMAPINFOHEADER>(info);
            }

            bool is_supported_dib(const EMU_BITMAPINFOHEADER& header, const uint32_t color_use)
            {
                constexpr uint32_t bi_rgb = 0;
                constexpr uint32_t dib_rgb_colors = 0;
                const auto bit_count = header.biBitCount;
                const bool valid_bit_count =
                    bit_count == 1 || bit_count == 4 || bit_count == 8 || bit_count == 16 || bit_count == 24 || bit_count == 32;
                // DIB_PAL_COLORS colour tables hold 16-bit logical-palette indices, not RGBQUADs.
                const bool valid_color_table = bit_count > 8 || color_use == dib_rgb_colors;
                return valid_bit_count && valid_color_table && header.biCompression == bi_rgb && header.biWidth > 0;
            }

            std::vector<uint32_t> read_dib_palette(const syscall_context& c, const emulator_pointer info,
                                                   const EMU_BITMAPINFOHEADER& header)
            {
                if (header.biBitCount > 8)
                {
                    return {};
                }

                const uint32_t max_colors = 1u << header.biBitCount;
                const uint32_t palette_entries = header.biClrUsed != 0 ? std::min<uint32_t>(header.biClrUsed, max_colors) : max_colors;
                constexpr size_t rgbquad_size = 4;
                const auto color_table = c.emu.read_memory(info + header.biSize, static_cast<size_t>(palette_entries) * rgbquad_size);

                // Pixel data may index past biClrUsed, so size the palette by bit depth rather than by the table.
                std::vector<uint32_t> palette(max_colors, 0xFF000000u);
                for (uint32_t i = 0; i < palette_entries; ++i)
                {
                    const std::byte* bgrx = color_table.data() + static_cast<size_t>(i) * rgbquad_size;
                    palette[i] = 0xFF000000u | (std::to_integer<uint32_t>(bgrx[2]) << 16) | (std::to_integer<uint32_t>(bgrx[1]) << 8) |
                                 std::to_integer<uint32_t>(bgrx[0]);
                }
                return palette;
            }

            size_t dib_stride(const uint64_t width, const uint32_t bit_count)
            {
                return static_cast<size_t>(((width * bit_count + 31u) / 32u) * 4u);
            }

            uint32_t dib_pixel_to_bgra32(const uint8_t* row, const uint32_t x, const uint16_t bpp, const std::vector<uint32_t>& palette)
            {
                switch (bpp)
                {
                case 1:
                    return palette[(row[x / 8u] >> (7u - (x & 7u))) & 1u];
                case 4: {
                    const uint8_t packed = row[x / 2u];
                    const uint8_t index = (x & 1u) == 0u ? packed >> 4u : packed & 0x0Fu;
                    return palette[index];
                }
                case 8:
                    return palette[row[x]];
                case 16: {
                    const uint8_t* p = row + static_cast<size_t>(x) * 2;
                    const uint32_t v = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8);
                    // BI_RGB 16bpp is RGB555, not RGB565.
                    const uint32_t r = ((v >> 10u) & 0x1Fu) * 255u / 31u;
                    const uint32_t g = ((v >> 5u) & 0x1Fu) * 255u / 31u;
                    const uint32_t b = (v & 0x1Fu) * 255u / 31u;
                    return 0xFF000000u | (r << 16) | (g << 8) | b;
                }
                case 24:
                case 32: {
                    const uint8_t* p = row + static_cast<size_t>(x) * (bpp / 8u);
                    return 0xFF000000u | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[1]) << 8) | p[0];
                }
                default:
                    return 0xFF000000u;
                }
            }

            std::optional<uint32_t> get_surface_pixel(const gdi_bitmap_surface& surface, const int x, const int y)
            {
                if (x < 0 || y < 0 || x >= static_cast<int>(surface.width) || y >= static_cast<int>(surface.height))
                {
                    return std::nullopt;
                }

                return surface.pixels[static_cast<size_t>(y) * surface.width + static_cast<size_t>(x)];
            }

            uint32_t colorref_to_bgra(const uint32_t colorref)
            {
                return 0xFF000000u | ((colorref & 0x000000FFu) << 16) | (colorref & 0x0000FF00u) | ((colorref & 0x00FF0000u) >> 16);
            }

            uint32_t bgra_to_colorref(const uint32_t bgra)
            {
                return ((bgra & 0x00FF0000u) >> 16) | (bgra & 0x0000FF00u) | ((bgra & 0x000000FFu) << 16);
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

            uint32_t read_dc_attr_colorref(const syscall_context& c, const uint64_t address)
            {
                uint32_t colorref = 0;
                c.win_emu.memory.try_read_memory(address, &colorref, sizeof(colorref));
                return colorref_to_bgra(colorref);
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

                if (pen_handle == static_cast<uint32_t>(read_gdi_object_slot(c, k_stock_dc_pen_index)))
                {
                    return read_dc_attr_colorref(c, dc_attr + k_gdi_dc_attr_dc_pen_color_offset);
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

            std::optional<uint32_t> get_brush_color_in_dc(const syscall_context& c, const hdc dc, const uint32_t brush_handle)
            {
                if (brush_handle == static_cast<uint32_t>(read_gdi_object_slot(c, k_stock_null_brush_index)))
                {
                    return std::nullopt;
                }

                uint64_t dc_attr = 0;
                if (brush_handle == static_cast<uint32_t>(read_gdi_object_slot(c, k_stock_dc_brush_index)) &&
                    get_gdi_object_address(c, static_cast<uint32_t>(dc), k_gdi_dc_type, dc_attr))
                {
                    return read_dc_attr_colorref(c, dc_attr + k_gdi_dc_attr_dc_brush_color_offset);
                }

                return get_brush_color(c, brush_handle);
            }

            std::optional<uint32_t> get_dc_brush_color(const syscall_context& c, const hdc dc)
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

                return get_brush_color_in_dc(c, dc, brush_handle);
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

            void draw_line(gdi_bitmap_surface& surface, const gdi::clip_region& visible, int x0, int y0, const int x1, const int y1,
                           const uint32_t color)
            {
                int dx = std::abs(x1 - x0);
                const int sx = x0 < x1 ? 1 : -1;
                int dy = -std::abs(y1 - y0);
                const int sy = y0 < y1 ? 1 : -1;
                int err = dx + dy;

                for (;;)
                {
                    set_clipped_pixel(surface, visible, x0, y0, color);
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

            void set_text_pixel(gdi_bitmap_surface& surface, const gdi::clip_region& visible, const int x, const int y,
                                const uint32_t color, const RECT* clip)
            {
                if (point_in_rect(x, y, clip))
                {
                    set_clipped_pixel(surface, visible, x, y, color);
                }
            }

            void draw_text_glyph(gdi_bitmap_surface& surface, const gdi::clip_region& visible, const int x, const int y, char32_t codepoint,
                                 const uint32_t color, const RECT* clip)
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
                            set_text_pixel(surface, visible, x + col, y + k_text_glyph_y_offset + row, color, clip);
                        }
                    }
                }
            }

            void draw_text(gdi_bitmap_surface& surface, const gdi::clip_region& visible, const int x, const int y,
                           const std::u16string_view text, const uint32_t color, const RECT* clip = nullptr, const uint32_t* dx = nullptr)
            {
                int pen_x = x;
                for (size_t i = 0; i < text.size(); ++i)
                {
                    draw_text_glyph(surface, visible, pen_x, y, static_cast<char32_t>(text[i]), color, clip);
                    pen_x += dx != nullptr ? static_cast<int32_t>(dx[i]) : k_text_cell_width;
                }
            }

            // SetTextAlign chooses which point of the text (x, y) names; draw_text starts at the top-left corner.
            POINT text_top_left(const int x, const int y, const uint32_t text_align, const size_t count, const uint32_t* dx)
            {
                constexpr uint32_t ta_right = 2;
                constexpr uint32_t ta_center = 6;
                constexpr uint32_t ta_bottom = 8;
                constexpr uint32_t ta_baseline = 24;

                int width = 0;
                for (size_t i = 0; i < count; ++i)
                {
                    width += dx != nullptr ? static_cast<int32_t>(dx[i]) : k_text_cell_width;
                }

                POINT origin{.x = x, .y = y};
                if ((text_align & ta_center) == ta_center)
                {
                    origin.x -= width / 2;
                }
                else if ((text_align & ta_right) != 0)
                {
                    origin.x -= width;
                }

                if ((text_align & ta_baseline) == ta_baseline)
                {
                    origin.y -= static_cast<int>(k_default_font_ascent);
                }
                else if ((text_align & ta_bottom) != 0)
                {
                    origin.y -= k_text_cell_height;
                }

                return origin;
            }

            bool is_stock_object_selected(const syscall_context& c, const hdc dc, const uint32_t attr_offset, const uint32_t stock_index)
            {
                uint64_t dc_attr = 0;
                if (!get_gdi_object_address(c, static_cast<uint32_t>(dc), k_gdi_dc_type, dc_attr))
                {
                    return false;
                }

                uint32_t selected = 0;
                c.win_emu.memory.try_read_memory(dc_attr + attr_offset, &selected, sizeof(selected));
                return selected != 0 && selected == static_cast<uint32_t>(read_gdi_object_slot(c, stock_index));
            }

            void fill_rect(gdi_bitmap_surface& surface, const gdi::clip_region& visible, const int64_t left, const int64_t top,
                           const int64_t right, const int64_t bottom, const uint32_t color)
            {
                visible.for_each_span(clamped_to_surface(surface, left, top, right, bottom),
                                      [&](const LONG x_begin, const LONG x_end, const LONG y) {
                                          auto* row = surface_row(surface, y);
                                          std::fill(row + x_begin, row + x_end, color);
                                      });
            }

            uint8_t get_dc_rop2(const syscall_context& c, const hdc dc)
            {
                uint8_t rop2 = k_r2_copypen;
                uint64_t dc_attr = 0;
                if (get_gdi_object_address(c, static_cast<uint32_t>(dc), k_gdi_dc_type, dc_attr))
                {
                    c.win_emu.memory.try_read_memory(dc_attr + k_gdi_dc_attr_rop2_offset, &rop2, sizeof(rop2));
                }
                return rop2;
            }

            // An R2_* code minus one is the operation's truth table, indexed by (pen bit << 1) | destination bit.
            uint32_t apply_rop2(const uint8_t rop2, const uint32_t pen, const uint32_t destination)
            {
                const auto table = static_cast<uint32_t>(rop2 - 1);
                uint32_t result = 0;
                if ((table & 1) != 0)
                {
                    result |= ~pen & ~destination;
                }
                if ((table & 2) != 0)
                {
                    result |= ~pen & destination;
                }
                if ((table & 4) != 0)
                {
                    result |= pen & ~destination;
                }
                if ((table & 8) != 0)
                {
                    result |= pen & destination;
                }
                return 0xFF000000u | (result & 0x00FFFFFFu);
            }

            void fill_rect_rop2(gdi_bitmap_surface& surface, const gdi::clip_region& visible, const int64_t left, const int64_t top,
                                const int64_t right, const int64_t bottom, const uint32_t color, const uint8_t rop2)
            {
                visible.for_each_span(clamped_to_surface(surface, left, top, right, bottom),
                                      [&](const LONG x_begin, const LONG x_end, const LONG y) {
                                          auto* row = surface_row(surface, y);
                                          for (auto x = x_begin; x < x_end; ++x)
                                          {
                                              row[x] = apply_rop2(rop2, color, row[x]);
                                          }
                                      });
            }

            // The outline is the closed polyline through the corners (left, top) and (right - 1, bottom - 1), each
            // segment drawn without its last pixel, so a 1x1 rectangle draws nothing and every pixel is hit once.
            void draw_rectangle_outline(gdi_bitmap_surface& surface, const gdi::clip_region& visible, const int64_t left, const int64_t top,
                                        const int64_t right, const int64_t bottom, const uint32_t color, const uint8_t rop2)
            {
                const auto right_edge = right - 1;
                const auto bottom_edge = bottom - 1;
                if (right_edge < left || bottom_edge < top || (right_edge == left && bottom_edge == top))
                {
                    return;
                }

                fill_rect_rop2(surface, visible, left, top, right, top + 1, color, rop2);
                if (bottom_edge > top)
                {
                    fill_rect_rop2(surface, visible, left, bottom_edge, right, bottom, color, rop2);
                }
                fill_rect_rop2(surface, visible, left, top + 1, left + 1, bottom_edge, color, rop2);
                if (right_edge > left)
                {
                    fill_rect_rop2(surface, visible, right_edge, top + 1, right, bottom_edge, color, rop2);
                }
            }

            void delete_batched_object(const syscall_context& c, const uint32_t handle_value, const uint8_t base_type)
            {
                GDI_HANDLE_ENTRY64 entry{};
                uint64_t entry_addr = 0;
                if (read_gdi_entry_for_handle(c, handle_value, entry, entry_addr) && entry.Type == base_type)
                {
                    handle_NtGdiDeleteObjectApp(c, handle_value);
                }
            }

            template <typename Batch>
            bool flush_gdi_text_batch(const syscall_context& c, const Batch& batch)
            {
                const auto batch_offset = batch.Offset & ~k_gdibs_no_rect;
                if (batch_offset == 0 || batch_offset > sizeof(batch.Buffer))
                {
                    return true;
                }

                const auto dc = static_cast<hdc>(batch.HDC);
                gdi_dc_state* dc_state = nullptr;
                gdi_bitmap_surface* surface = nullptr;
                int32_t origin_x = 0;
                int32_t origin_y = 0;
                uint32_t present_handle = 0;
                gdi::clip_region visible{};
                (void)get_dc_state_and_draw_target(c, dc, dc_state, surface, origin_x, origin_y, visible, &present_handle);

                const auto* bytes = reinterpret_cast<const uint8_t*>(batch.Buffer);
                size_t offset = 0;
                while (offset + sizeof(gdi_batch_header) <= batch_offset)
                {
                    const auto* header = reinterpret_cast<const gdi_batch_header*>(bytes + offset);
                    if (header->size <= 0 || offset + static_cast<size_t>(header->size) > batch_offset)
                    {
                        break;
                    }

                    if ((header->cmd == k_gdi_batch_cmd_delete_object || header->cmd == k_gdi_batch_cmd_delete_region) &&
                        static_cast<size_t>(header->size) >= sizeof(gdi_batch_delete_object))
                    {
                        gdi_batch_delete_object delete_object{};
                        std::memcpy(&delete_object, bytes + offset, sizeof(delete_object));
                        delete_batched_object(c, static_cast<uint32_t>(delete_object.handle),
                                              header->cmd == k_gdi_batch_cmd_delete_object ? k_gdi_brush_type : k_gdi_region_type);
                    }
                    else if (surface && header->cmd == k_gdi_batch_cmd_text_out &&
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
                            fill_rect(*surface, visible, clip_rect.left, clip_rect.top, clip_rect.right, clip_rect.bottom,
                                      colorref_to_bgra(text_out->background));
                        }

                        // The payload after `string` holds the text, then the advances. dx_offset is 0 without advances and
                        // otherwise the text size rounded up to a ULONG (GDIBSTEXTOUT::Size). The guest controls the batch, so
                        // both parts are checked to fit inside this command before being read.
                        const auto payload_capacity = static_cast<size_t>(header->size) - offsetof(gdi_batch_text_out, string);
                        const auto dx_offset = static_cast<size_t>(text_out->dx_offset);
                        const auto text_bytes = static_cast<size_t>(text_out->count) * sizeof(char16_t);
                        const auto dx_bytes = dx_offset != 0 ? static_cast<size_t>(text_out->count) * sizeof(uint32_t) : 0;
                        if (text_bytes <= payload_capacity &&
                            (dx_offset == 0 ||
                             (dx_offset >= text_bytes && dx_offset <= payload_capacity && dx_bytes <= payload_capacity - dx_offset)))
                        {
                            const auto* text = reinterpret_cast<const char16_t*>(text_out->string.data());
                            const auto* advances = dx_offset != 0
                                                       ? reinterpret_cast<const uint32_t*>(
                                                             reinterpret_cast<const std::byte*>(text_out->string.data()) + dx_offset)
                                                       : nullptr;
                            const auto top_left = text_top_left(text_out->x + text_out->viewport_org.x + origin_x,
                                                                text_out->y + text_out->viewport_org.y + origin_y, text_out->text_align,
                                                                text_out->count, advances);
                            draw_text(*surface, visible, top_left.x, top_left.y, std::u16string_view(text, text_out->count),
                                      colorref_to_bgra(text_out->foreground),
                                      has_rect && (text_out->options & ETO_CLIPPED) != 0 ? &clip_rect : nullptr, advances);
                        }
                    }
                    else if (surface && header->cmd == k_gdi_batch_cmd_text_out_rect &&
                             static_cast<size_t>(header->size) >= sizeof(gdi_batch_text_out_rect))
                    {
                        const auto* text_out_rect = reinterpret_cast<const gdi_batch_text_out_rect*>(header);
                        if ((text_out_rect->options & ETO_OPAQUE) != 0)
                        {
                            const int64_t shift_x = static_cast<int64_t>(text_out_rect->viewport_org.x) + origin_x;
                            const int64_t shift_y = static_cast<int64_t>(text_out_rect->viewport_org.y) + origin_y;
                            const auto& rect = text_out_rect->rect;
                            fill_rect(*surface, visible, std::min(rect.left, rect.right) + shift_x,
                                      std::min(rect.top, rect.bottom) + shift_y, std::max(rect.left, rect.right) + shift_x,
                                      std::max(rect.top, rect.bottom) + shift_y, colorref_to_bgra(text_out_rect->background));
                        }
                    }
                    else if (surface && header->cmd == k_gdi_batch_cmd_poly_pat_blt &&
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

                            const auto color =
                                brush != 0 ? get_brush_color_in_dc(c, dc, static_cast<uint32_t>(brush)) : get_dc_brush_color(c, dc);
                            if (color)
                            {
                                fill_rect(*surface, visible, rect.x + origin_x, rect.y + origin_y, rect.x + rect.width + origin_x,
                                          rect.y + rect.height + origin_y, *color);
                            }
                        }
                    }

                    offset += static_cast<size_t>(header->size);
                }

                if (surface)
                {
                    present_win_surface(c, present_handle, surface);
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

                c.proc.user_handles.get_server_info().access([&](USER_SERVERINFO& server_info) {
                    std::ranges::copy(k_default_system_colors, server_info.systemColors);
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

                    const uint32_t handle = allocate_gdi_object(c, k_gdi_brush_type, k_gdi_brush_attr_size, false);
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
                    const uint32_t handle = allocate_gdi_object(c, k_gdi_brush_type, k_gdi_brush_attr_size, false);
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

            uint32_t stock_brush_color(const uint32_t index)
            {
                switch (index)
                {
                case k_stock_ltgray_brush_index:
                    return 0x00C0C0C0u;
                case k_stock_gray_brush_index:
                    return 0x00808080u;
                case k_stock_dkgray_brush_index:
                    return 0x00404040u;
                case k_stock_black_brush_index:
                    return 0x00000000u;
                default:
                    return 0x00FFFFFFu;
                }
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

                constexpr std::array<EMU_PALETTEENTRY, 20> default_palette_entries = {
                    EMU_PALETTEENTRY{.peRed = 0x00, .peGreen = 0x00, .peBlue = 0x00, .peFlags = 0x00},
                    EMU_PALETTEENTRY{.peRed = 0x80, .peGreen = 0x00, .peBlue = 0x00, .peFlags = 0x00},
                    EMU_PALETTEENTRY{.peRed = 0x00, .peGreen = 0x80, .peBlue = 0x00, .peFlags = 0x00},
                    EMU_PALETTEENTRY{.peRed = 0x80, .peGreen = 0x80, .peBlue = 0x00, .peFlags = 0x00},
                    EMU_PALETTEENTRY{.peRed = 0x00, .peGreen = 0x00, .peBlue = 0x80, .peFlags = 0x00},
                    EMU_PALETTEENTRY{.peRed = 0x80, .peGreen = 0x00, .peBlue = 0x80, .peFlags = 0x00},
                    EMU_PALETTEENTRY{.peRed = 0x00, .peGreen = 0x80, .peBlue = 0x80, .peFlags = 0x00},
                    EMU_PALETTEENTRY{.peRed = 0xC0, .peGreen = 0xC0, .peBlue = 0xC0, .peFlags = 0x00},
                    EMU_PALETTEENTRY{.peRed = 0xC0, .peGreen = 0xDC, .peBlue = 0xC0, .peFlags = 0x00},
                    EMU_PALETTEENTRY{.peRed = 0xA6, .peGreen = 0xCA, .peBlue = 0xF0, .peFlags = 0x00},
                    EMU_PALETTEENTRY{.peRed = 0xFF, .peGreen = 0xFB, .peBlue = 0xF0, .peFlags = 0x00},
                    EMU_PALETTEENTRY{.peRed = 0xA0, .peGreen = 0xA0, .peBlue = 0xA4, .peFlags = 0x00},
                    EMU_PALETTEENTRY{.peRed = 0x80, .peGreen = 0x80, .peBlue = 0x80, .peFlags = 0x00},
                    EMU_PALETTEENTRY{.peRed = 0xFF, .peGreen = 0x00, .peBlue = 0x00, .peFlags = 0x00},
                    EMU_PALETTEENTRY{.peRed = 0x00, .peGreen = 0xFF, .peBlue = 0x00, .peFlags = 0x00},
                    EMU_PALETTEENTRY{.peRed = 0xFF, .peGreen = 0xFF, .peBlue = 0x00, .peFlags = 0x00},
                    EMU_PALETTEENTRY{.peRed = 0x00, .peGreen = 0x00, .peBlue = 0xFF, .peFlags = 0x00},
                    EMU_PALETTEENTRY{.peRed = 0xFF, .peGreen = 0x00, .peBlue = 0xFF, .peFlags = 0x00},
                    EMU_PALETTEENTRY{.peRed = 0x00, .peGreen = 0xFF, .peBlue = 0xFF, .peFlags = 0x00},
                    EMU_PALETTEENTRY{.peRed = 0xFF, .peGreen = 0xFF, .peBlue = 0xFF, .peFlags = 0x00},
                };

                constexpr std::array<stock_seed, 14> seeds = {
                    stock_seed{
                        .index = k_stock_white_brush_index,
                        .type = k_gdi_brush_type,
                        .attr_size = k_gdi_brush_attr_size,
                        .mirror_to_default_gui_slot = false,
                    },
                    stock_seed{
                        .index = k_stock_ltgray_brush_index,
                        .type = k_gdi_brush_type,
                        .attr_size = k_gdi_brush_attr_size,
                        .mirror_to_default_gui_slot = false,
                    },
                    stock_seed{
                        .index = k_stock_gray_brush_index,
                        .type = k_gdi_brush_type,
                        .attr_size = k_gdi_brush_attr_size,
                        .mirror_to_default_gui_slot = false,
                    },
                    stock_seed{
                        .index = k_stock_dkgray_brush_index,
                        .type = k_gdi_brush_type,
                        .attr_size = k_gdi_brush_attr_size,
                        .mirror_to_default_gui_slot = false,
                    },
                    stock_seed{
                        .index = k_stock_black_brush_index,
                        .type = k_gdi_brush_type,
                        .attr_size = k_gdi_brush_attr_size,
                        .mirror_to_default_gui_slot = false,
                    },
                    stock_seed{
                        .index = k_stock_null_brush_index,
                        .type = k_gdi_brush_type,
                        .attr_size = k_gdi_brush_attr_size,
                        .mirror_to_default_gui_slot = false,
                    },
                    stock_seed{
                        .index = k_stock_white_pen_index,
                        .type = k_gdi_pen_type,
                        .attr_size = k_gdi_pen_attr_size,
                        .mirror_to_default_gui_slot = false,
                    },
                    stock_seed{
                        .index = k_stock_black_pen_index,
                        .type = k_gdi_pen_type,
                        .attr_size = k_gdi_pen_attr_size,
                        .mirror_to_default_gui_slot = false,
                    },
                    stock_seed{
                        .index = k_stock_null_pen_index,
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
                        .index = k_stock_default_palette,
                        .type = k_gdi_palette_type,
                        .attr_size = k_gdi_palette_attr_size,
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
                        set_gdi_object_color(c, handle, k_gdi_brush_type, stock_brush_color(seed.index));
                    }
                    else if (seed.type == k_gdi_pen_type)
                    {
                        const uint32_t color = seed.index == k_stock_white_pen_index ? 0x00FFFFFFu : 0x00000000u;
                        set_gdi_object_color(c, handle, k_gdi_pen_type, color);
                    }
                    else if (seed.type == k_gdi_font_type)
                    {
                        c.proc.gdi_fonts[handle] = stock_font_logfont(seed.index);
                    }
                    else if (seed.type == k_gdi_palette_type)
                    {
                        c.proc.gdi_palettes[handle].assign(default_palette_entries.begin(), default_palette_entries.end());
                    }

                    if (seed.mirror_to_default_gui_slot)
                    {
                        write_gdi_object96_slot(c, k_stock_default_gui_font_index, handle64);
                    }
                }

                if (read_gdi_object96_slot(c, k_stock_default_gui_font_index) == 0)
                {
                    const auto default_gui_font = read_gdi_object_slot(c, k_stock_default_gui_font_index);
                    if (default_gui_font != 0)
                    {
                        write_gdi_object96_slot(c, k_stock_default_gui_font_index, default_gui_font);
                    }
                }

                seed_user_system_color_brushes(c);
            }

            void initialize_dc_attr(const syscall_context& c, const uint64_t dc_attr)
            {
                std::array<uint8_t, k_gdi_dc_attr_size> zeroed{};
                c.emu.write_memory(dc_attr, zeroed.data(), zeroed.size());
                c.emu.write_memory(dc_attr + k_gdi_dc_attr_rop2_offset, &k_r2_copypen, sizeof(k_r2_copypen));

                constexpr uint32_t white = 0x00FFFFFFu;
                c.emu.write_memory(dc_attr + k_gdi_dc_attr_dc_brush_color_offset, &white, sizeof(white));

                // A new DC draws opaque text on white; gdi32 batches text with the realized background color.
                c.emu.write_memory(dc_attr + k_gdi_dc_attr_background_color_offset, &white, sizeof(white));
                c.emu.write_memory(dc_attr + k_gdi_dc_attr_realized_background_color_offset, &white, sizeof(white));

                const std::array<std::pair<uint32_t, uint32_t>, 3> default_objects{{
                    {k_stock_system_font_index, k_gdi_dc_attr_font_offset},
                    {k_stock_white_brush_index, k_gdi_dc_attr_hbrush_offset},
                    {k_stock_black_pen_index, k_gdi_dc_attr_hpen_offset},
                }};
                for (const auto& [stock_index, attr_offset] : default_objects)
                {
                    const uint64_t stock_object = read_gdi_object_slot(c, stock_index);
                    if (stock_object != 0)
                    {
                        const auto handle_value = static_cast<uint32_t>(stock_object);
                        c.emu.write_memory(dc_attr + attr_offset, &handle_value, sizeof(handle_value));
                    }
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
                else
                {
                    c.win_emu.memory.release_memory(dc_attr, 0);
                    dc_attr = 0;
                }
                return handle_value;
            }

            uint32_t create_gdi_bitmap_surface(const syscall_context& c, const uint32_t width, const uint32_t height,
                                               const uint32_t fill = k_default_bitmap_fill,
                                               gdi_bitmap_surface** const created_surface = nullptr)
            {
                if (created_surface != nullptr)
                {
                    *created_surface = nullptr;
                }

                const auto handle_value = allocate_gdi_object(c, k_gdi_bitmap_type, k_gdi_bitmap_attr_size);
                if (handle_value == 0)
                {
                    return 0;
                }

                auto& surface = c.proc.gdi_bitmap_surfaces[handle_value];
                surface.width = width;
                surface.height = height;
                surface.pixels.assign(static_cast<size_t>(width) * static_cast<size_t>(height), fill);
                if (created_surface != nullptr)
                {
                    *created_surface = &surface;
                }
                return handle_value;
            }

            uint32_t ensure_memory_dc_default_bitmap(const syscall_context& c)
            {
                const auto default_bitmap = c.proc.gdi_memory_dc_default_bitmap_handle;
                if (default_bitmap != 0 && c.proc.gdi_bitmap_surfaces.contains(default_bitmap))
                {
                    return default_bitmap;
                }

                gdi_bitmap_surface* surface = nullptr;
                const auto created_bitmap = create_gdi_bitmap_surface(c, 1, 1, 0xFF000000u, &surface);
                if (surface != nullptr)
                {
                    surface->guest_bpp = 1;
                }

                c.proc.gdi_memory_dc_default_bitmap_handle = created_bitmap;
                return created_bitmap;
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

            uint32_t get_device_caps_value(const process_context& proc, const uint32_t index)
            {
                switch (index)
                {
                case 4: // HORZSIZE
                    return (proc.display_width * 254) / (k_default_dpi * 10);
                case 6: // VERTSIZE
                    return (proc.display_height * 254) / (k_default_dpi * 10);
                case 8:    // HORZRES
                case 0x76: // DESKTOPHORZRES
                    return proc.display_width;
                case 0xA:  // VERTRES
                case 0x75: // DESKTOPVERTRES
                    return proc.display_height;
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
                static constexpr std::array<uint32_t, 9> k_cap_indices = {4, 6, 8, 0xA, 0x28, 0x2A, 0x2C, 0x58, 0x5A};

                std::vector<uint32_t> caps(count, 0);
                for (const auto index : k_cap_indices)
                {
                    if (index < caps.size())
                    {
                        caps[index] = get_device_caps_value(c.proc, index);
                    }
                }

                if (caps_ptr != 0)
                {
                    c.emu.write_memory(caps_ptr, caps.data(), caps.size() * sizeof(uint32_t));
                }
            }

            std::span<EMU_PALETTEENTRY> palette_range(std::vector<EMU_PALETTEENTRY>& palette, const uint16_t start, const uint16_t count)
            {
                if (start >= palette.size())
                {
                    return {};
                }

                return std::span{palette}.subspan(start, std::min<size_t>(count, palette.size() - start));
            }

            LONG read_palette_entries(const syscall_context& c, std::vector<EMU_PALETTEENTRY>& palette, const uint16_t start,
                                      const uint16_t count, const emulator_pointer entries)
            {
                const auto range = palette_range(palette, start, count);
                if (!c.win_emu.memory.try_write_memory(entries, range.data(), range.size_bytes()))
                {
                    return 0;
                }

                return static_cast<LONG>(range.size());
            }

            // AnimatePalette only replaces the entries that were created with PC_RESERVED.
            LONG write_palette_entries(const syscall_context& c, std::vector<EMU_PALETTEENTRY>& palette, const uint16_t start,
                                       const uint16_t count, const emulator_pointer entries, const bool reserved_only)
            {
                const auto range = palette_range(palette, start, count);
                std::vector<EMU_PALETTEENTRY> source(range.size());
                if (!c.win_emu.memory.try_read_memory(entries, source.data(), source.size() * sizeof(EMU_PALETTEENTRY)))
                {
                    return 0;
                }

                for (size_t i = 0; i < range.size(); ++i)
                {
                    if (!reserved_only || (range[i].peFlags & k_palette_entry_reserved) != 0)
                    {
                        range[i] = source[i];
                    }
                }

                return static_cast<LONG>(range.size());
            }

            uint32_t find_nearest_palette_index(const std::vector<EMU_PALETTEENTRY>& palette, const COLORREF color)
            {
                const auto distance = [color](const EMU_PALETTEENTRY& entry) {
                    const auto red = static_cast<int32_t>(entry.peRed) - static_cast<int32_t>(color & 0xFF);
                    const auto green = static_cast<int32_t>(entry.peGreen) - static_cast<int32_t>((color >> 8) & 0xFF);
                    const auto blue = static_cast<int32_t>(entry.peBlue) - static_cast<int32_t>((color >> 16) & 0xFF);
                    return red * red + green * green + blue * blue;
                };

                const auto nearest = std::ranges::min_element(palette, {}, distance);
                return static_cast<uint32_t>(std::distance(palette.begin(), nearest));
            }

            uint32_t get_gdi_object_size(const uint8_t type)
            {
                switch (type)
                {
                case k_gdi_palette_type:
                    return k_palette_object_size;
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

            bool rop3_uses_pattern(const uint8_t rop3)
            {
                return ((rop3 >> 4) & 0x0Fu) != (rop3 & 0x0Fu);
            }

            bool rop3_uses_source(const uint8_t rop3)
            {
                // ROP3 truth-table index: bit0=D, bit1=S, bit2=P.
                // If changing S can change the output for any D/P pair, the source DC is required.
                for (uint32_t p = 0; p <= 1; ++p)
                {
                    for (uint32_t d = 0; d <= 1; ++d)
                    {
                        const uint32_t i0 = (p << 2) | d;
                        const uint32_t i1 = (p << 2) | 0x02u | d;
                        if (((rop3 >> i0) & 1u) != ((rop3 >> i1) & 1u))
                        {
                            return true;
                        }
                    }
                }

                return false;
            }

            uint32_t apply_rop3(const uint8_t rop3, const uint32_t src, const uint32_t dst, const uint32_t pat)
            {
                uint32_t out = 0;

                // Each bit of rop3 selects the output for one D/S/P boolean combination.
                // Index layout matches Win32 ROP3 encoding:
                //   bit0 = D, bit1 = S, bit2 = P
                for (uint32_t i = 0; i < 8; ++i)
                {
                    if (((rop3 >> i) & 1u) == 0)
                    {
                        continue;
                    }

                    uint32_t mask = (i & 0x01u) ? dst : ~dst;
                    mask &= (i & 0x02u) ? src : ~src;
                    mask &= (i & 0x04u) ? pat : ~pat;
                    out |= mask;
                }

                return out;
            }

            // Windows fails a request whose output would exceed this many bytes before writing anything.
            constexpr uint64_t max_char_metric_output_size = 0x2710000;

            bool is_char_metric_request_valid(const syscall_context& c, const hdc dc, const UINT char_count, const size_t entry_size,
                                              const emulator_pointer chars, const emulator_pointer buffer)
            {
                return dc != 0 && buffer != 0 && char_count != 0 &&
                       static_cast<uint64_t>(char_count) * entry_size <= max_char_metric_output_size &&
                       (chars == 0 || is_readable_range(c.win_emu.memory, chars, static_cast<uint64_t>(char_count) * sizeof(char16_t)));
            }

            bool write_guest_block(const syscall_context& c, const emulator_pointer address, const void* data, const size_t size)
            {
                return is_writable_range(c.win_emu.memory, address, size) && c.emu.try_write_memory(address, data, size);
            }

            bool write_guest_bytes_up_to_fault(const syscall_context& c, const emulator_pointer address, const void* data,
                                               const size_t size)
            {
                const auto* bytes = static_cast<const std::byte*>(data);
                for (size_t offset = 0; offset < size;)
                {
                    const auto current = address + offset;
                    const auto piece = static_cast<size_t>(std::min<uint64_t>(size - offset, page_align_up(current + 1) - current));
                    if (!write_guest_block(c, current, bytes + offset, piece))
                    {
                        return false;
                    }

                    offset += piece;
                }

                return true;
            }

            template <typename Entry>
            bool write_repeated_entries(const syscall_context& c, const emulator_pointer buffer, const UINT count, const Entry& entry)
            {
                std::array<Entry, 0x100> chunk{};
                chunk.fill(entry);
                for (UINT written = 0; written < count;)
                {
                    const auto chunk_count = std::min<size_t>(chunk.size(), count - written);
                    if (!write_guest_bytes_up_to_fault(c, buffer + (static_cast<uint64_t>(written) * sizeof(Entry)), chunk.data(),
                                                       chunk_count * sizeof(Entry)))
                    {
                        return false;
                    }

                    written += static_cast<UINT>(chunk_count);
                }

                return true;
            }

            // Windows measures at most this many characters, with or without a fit count or partial extents.
            constexpr uint64_t max_text_extent_chars = max_char_metric_output_size / (sizeof(char16_t) + sizeof(ULONG));

            bool write_fixed_width_extents(const syscall_context& c, const emulator_pointer extents, const ULONG count)
            {
                std::array<ULONG, 0x100> chunk{};
                for (ULONG written = 0; written < count;)
                {
                    const auto chunk_count = std::min<size_t>(chunk.size(), count - written);
                    for (size_t i = 0; i < chunk_count; ++i)
                    {
                        chunk[i] = static_cast<ULONG>((written + i + 1) * k_default_font_width);
                    }

                    if (!write_guest_bytes_up_to_fault(c, extents + (static_cast<uint64_t>(written) * sizeof(ULONG)), chunk.data(),
                                                       chunk_count * sizeof(ULONG)))
                    {
                        return false;
                    }

                    written += static_cast<ULONG>(chunk_count);
                }

                return true;
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

        uint32_t handle_NtGdiGetDeviceCaps(const syscall_context& c, const hdc /*dc*/, const uint32_t index)
        {
            return get_device_caps_value(c.proc, index);
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
            gdi::clip_region visible{};
            if (!get_dc_state_and_draw_target(c, dc, dc_state, surface, origin_x, origin_y, visible) || !surface)
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
                        set_clipped_pixel(*surface, visible, ox + center + dx, oy + center + dy, color);
                    }
                }
                return;
            }

            fill_rect(*surface, visible, ox, oy, ox + k_glyph, oy + k_glyph, interior);
            for (int i = 0; i < k_glyph; ++i)
            {
                set_clipped_pixel(*surface, visible, ox + i, oy, k_shadow);              // top edge
                set_clipped_pixel(*surface, visible, ox, oy + i, k_shadow);              // left edge
                set_clipped_pixel(*surface, visible, ox + i, oy + k_glyph - 1, k_white); // bottom edge
                set_clipped_pixel(*surface, visible, ox + k_glyph - 1, oy + i, k_white); // right edge
            }
            for (int i = 1; i < k_glyph - 1; ++i)
            {
                set_clipped_pixel(*surface, visible, ox + i, oy + 1, k_frame); // inner top
                set_clipped_pixel(*surface, visible, ox + 1, oy + i, k_frame); // inner left
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
                            set_clipped_pixel(*surface, visible, ox + col, oy + row, mark);
                        }
                    }
                }
            }
        }

        BOOL handle_NtGdiFlush(const syscall_context& c)
        {
            auto& thread = c.thread();
            if (thread.teb64)
            {
                BOOL result = TRUE;
                thread.teb64->access([&](TEB64& teb) {
                    result = flush_gdi_text_batch(c, teb.GdiTebBatch) ? TRUE : FALSE;
                    teb.GdiTebBatch = {};
                    teb.GdiBatchCount = 0;
                });
                return result;
            }

            if (thread.teb32)
            {
                BOOL result = TRUE;
                thread.teb32->access([&](TEB32& teb) {
                    result = flush_gdi_text_batch(c, teb.GdiTebBatch) ? TRUE : FALSE;
                    teb.GdiTebBatch = {};
                    teb.GdiBatchCount = 0;
                });
                return result;
            }

            return TRUE;
        }

        void flush_gdi_batch(const syscall_context& c)
        {
            const auto& thread = c.thread();
            ULONG batch_count = 0;
            if (thread.teb64)
            {
                c.emu.read_memory(thread.teb64->value() + offsetof(TEB64, GdiBatchCount), &batch_count, sizeof(batch_count));
            }
            if (batch_count == 0 && thread.teb32)
            {
                c.emu.read_memory(thread.teb32->value() + offsetof(TEB32, GdiBatchCount), &batch_count, sizeof(batch_count));
            }

            if (batch_count != 0)
            {
                (void)handle_NtGdiFlush(c);
            }
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
            return allocate_gdi_object(c, k_gdi_brush_type, k_gdi_brush_attr_size, false);
        }

        uint64_t handle_NtGdiCreatePen(const syscall_context& c, const uint32_t style, const uint32_t width, const uint32_t color)
        {
            // On Windows only solid pens of width 0, the pens gdi32 can cache, have a user attribute. gdi32 deletes the
            // others at once instead of queueing the delete in the GDI batch.
            const auto handle = allocate_gdi_object(c, k_gdi_pen_type, k_gdi_pen_attr_size, style == k_ps_solid && width == 0);
            uint64_t pen_attr = 0;
            if (handle != 0 && get_gdi_object_address(c, static_cast<uint32_t>(handle), k_gdi_pen_type, pen_attr))
            {
                c.emu.write_memory(pen_attr + sizeof(uint32_t), &color, sizeof(color));
            }
            return handle;
        }

        uint64_t handle_NtGdiCreatePaletteInternal(const syscall_context& c, const emulator_pointer log_palette, const uint32_t entry_count)
        {
            if (entry_count == 0 || entry_count > k_max_palette_entries)
            {
                return 0;
            }

            std::vector<EMU_PALETTEENTRY> entries(entry_count);
            if (!c.win_emu.memory.try_read_memory(log_palette + k_log_palette_entries_offset, entries.data(),
                                                  entries.size() * sizeof(EMU_PALETTEENTRY)))
            {
                return 0;
            }

            const auto palette = allocate_gdi_object(c, k_gdi_palette_type, k_gdi_palette_attr_size);
            if (palette != 0)
            {
                c.proc.gdi_palettes[palette] = std::move(entries);
            }

            return palette;
        }

        uint64_t handle_NtGdiCreateHalftonePalette(const syscall_context& c)
        {
            return allocate_gdi_object(c, k_gdi_palette_type, k_gdi_palette_attr_size);
        }

        LONG handle_NtGdiDoPalette(const syscall_context& c, const handle object, const uint16_t start, const uint16_t count,
                                   const emulator_pointer entries, const uint32_t function, const BOOL /*inbound*/)
        {
            const auto palette = c.proc.gdi_palettes.find(static_cast<uint32_t>(object.bits));
            if (palette == c.proc.gdi_palettes.end())
            {
                return 0;
            }

            switch (function)
            {
            case k_gdi_pal_get_entries:
                return entries == 0 ? static_cast<LONG>(palette->second.size())
                                    : read_palette_entries(c, palette->second, start, count, entries);
            case k_gdi_pal_set_entries:
                return write_palette_entries(c, palette->second, start, count, entries, false);
            case k_gdi_pal_animate:
                return write_palette_entries(c, palette->second, start, count, entries, true) != 0 ? TRUE : FALSE;
            default:
                return 0;
            }
        }

        uint32_t handle_NtGdiGetNearestPaletteIndex(const syscall_context& c, const handle palette, const COLORREF color)
        {
            const auto it = c.proc.gdi_palettes.find(static_cast<uint32_t>(palette.bits));
            if (it == c.proc.gdi_palettes.end())
            {
                return k_clr_invalid;
            }

            return find_nearest_palette_index(it->second, color);
        }

        uint32_t count_process_gdi_objects(const syscall_context& c)
        {
            uint64_t table = 0;
            c.proc.peb64.access([&](const PEB64& peb) { table = peb.GdiSharedHandleTable; });
            if (table == 0)
            {
                return 0;
            }

            std::unordered_set<uint32_t> system_objects{};
            any_system_gdi_object(c, [&](const uint32_t object) {
                system_objects.insert(object);
                return false;
            });

            std::vector<GDI_HANDLE_ENTRY64> entries(GDI_MAX_HANDLE_COUNT);
            c.emu.read_memory(table, entries.data(), entries.size() * sizeof(GDI_HANDLE_ENTRY64));

            uint32_t count = 0;
            for (uint32_t index = 0; index < entries.size(); ++index)
            {
                const auto& entry = entries[index];
                const auto handle_value = (static_cast<uint32_t>(entry.Unique) << 16) | index;
                if (entry.Type != 0 && (entry.Owner & ~1u) == c.proc.process_id && !system_objects.contains(handle_value))
                {
                    ++count;
                }
            }

            return count;
        }

        uint64_t handle_NtGdiCreateCompatibleDC(const syscall_context& c, const hdc /*dc*/)
        {
            uint64_t dc_attr = 0;
            const auto dc = allocate_gdi_dc(c, dc_attr);
            if (dc == 0)
            {
                return 0;
            }

            const auto it = c.proc.gdi_dc_states.find(static_cast<uint32_t>(dc));
            if (it == c.proc.gdi_dc_states.end())
            {
                return 0;
            }

            it->second.is_memory_dc = true;

            const auto default_bitmap = ensure_memory_dc_default_bitmap(c);
            if (default_bitmap == 0)
            {
                return 0;
            }

            it->second.selected_bitmap = default_bitmap;
            return dc;
        }

        bool is_gdi_brush(const syscall_context& c, const uint64_t handle)
        {
            GDI_HANDLE_ENTRY64 entry{};
            uint64_t entry_address = 0;
            return handle != 0 && read_gdi_entry_for_handle(c, static_cast<uint32_t>(handle), entry, entry_address) &&
                   gdi_object_type(entry) == k_gdi_brush_type;
        }

        hdc create_gdi_window_dc(const syscall_context& c, const hwnd window)
        {
            uint64_t dc_attr = 0;
            const auto dc = allocate_gdi_dc(c, dc_attr);
            if (dc != 0)
            {
                auto& state = c.proc.gdi_dc_states[dc];
                state.target_window = window;
                state.is_window_dc = true;
            }
            return dc;
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

        uint64_t handle_NtGdiAddFontMemResourceEx(const syscall_context& c, const emulator_pointer /*buffer*/,
                                                  const uint32_t /*buffer_size*/, const emulator_pointer /*design_vector*/,
                                                  const uint32_t /*design_vector_size*/, const emulator_object<uint32_t> num_fonts)
        {
            const auto handle_value = allocate_gdi_object(c, k_gdi_font_type, k_gdi_font_attr_size);
            if (handle_value == 0)
            {
                return 0;
            }

            if (num_fonts)
            {
                num_fonts.write(1);
            }

            return handle_value;
        }

        int32_t handle_NtGdiAddFontResourceW(const syscall_context&, const emulator_pointer files, const uint32_t character_count,
                                             const uint32_t file_count, const uint32_t /*flags*/, const uint32_t /*thread_id*/,
                                             const emulator_pointer /*design_vector*/)
        {
            if (files == 0 || character_count == 0 || file_count == 0)
            {
                return 0;
            }

            return static_cast<int32_t>(file_count);
        }

        BOOL handle_NtGdiRemoveFontMemResourceEx(const syscall_context& /*c*/, const uint64_t font_handle)
        {
            return font_handle != 0 ? TRUE : FALSE;
        }

        uint64_t handle_NtGdiCreateCompatibleBitmap(const syscall_context& c, const hdc /*dc*/, const uint32_t width, const uint32_t height)
        {
            return create_gdi_bitmap_surface(c, width, height);
        }

        uint64_t handle_NtGdiCreateBitmap(const syscall_context& c, const uint32_t width, const uint32_t height, const uint32_t planes,
                                          const uint32_t bits_pixel, const emulator_pointer bits)
        {
            gdi_bitmap_surface* surface = nullptr;
            const auto monochrome = planes == 1 && bits_pixel == 1;
            const auto handle_value = create_gdi_bitmap_surface(c, width, height, monochrome ? 0 : k_default_bitmap_fill, &surface);
            if (surface != nullptr)
            {
                if (planes == 1 && bits_pixel != 0)
                {
                    surface->guest_bpp = bits_pixel;
                }

                if (bits != 0 && monochrome)
                {
                    const auto stride = ((static_cast<size_t>(width) + 15u) / 16u) * 2u;
                    std::vector<uint8_t> input(stride * height);
                    c.emu.read_memory(bits, input.data(), input.size());
                    for (uint32_t y = 0; y < height; ++y)
                    {
                        for (uint32_t x = 0; x < width; ++x)
                        {
                            const auto bit = (input[static_cast<size_t>(y) * stride + x / 8u] >> (7u - (x & 7u))) & 1u;
                            surface->pixels[static_cast<size_t>(y) * width + x] = bit == 0 ? 0xFF000000u : 0xFFFFFFFFu;
                        }
                    }
                }
                else if (bits != 0 && planes == 1 && bits_pixel == 32)
                {
                    const auto byte_count = static_cast<size_t>(width) * static_cast<size_t>(height) * sizeof(uint32_t);
                    c.emu.read_memory(bits, surface->pixels.data(), byte_count);
                }
            }
            return handle_value;
        }

        int32_t handle_NtGdiGetBitmapBits(const syscall_context& c, const handle bitmap, const int32_t count, const emulator_pointer bits)
        {
            const auto it = c.proc.gdi_bitmap_surfaces.find(static_cast<uint32_t>(bitmap.bits));

            if (it == c.proc.gdi_bitmap_surfaces.end())
            {
                return 0;
            }

            auto& surface = it->second;
            sync_surface_from_guest_dib(c, surface);

            const uint32_t bpp = surface.guest_bpp;

            if (bpp != 1 && bpp != 24 && bpp != 32)
            {
                c.win_emu.log.warn("NtGdiGetBitmapBits: Unsupported bitmap bit depth: %u bpp", bpp);
                return 0;
            }

            // GetBitmapBits scanlines are aligned to 16-bit boundaries.
            const size_t stride = ((static_cast<size_t>(surface.width) * bpp + 15) / 16) * 2;

            const size_t total_size = stride * surface.height;

            if (total_size > static_cast<size_t>(std::numeric_limits<int32_t>::max()))
            {
                return 0;
            }

            // A null output buffer is a size query.
            if (bits == 0)
            {
                return static_cast<int32_t>(total_size);
            }

            const size_t copy_size = count < 0 ? total_size : std::min(static_cast<size_t>(count), total_size);

            if (copy_size == 0)
            {
                return 0;
            }

            std::vector<uint8_t> output(total_size, 0);

            for (uint32_t y = 0; y < surface.height; ++y)
            {
                const uint32_t source_y = surface.guest_top_down ? y : surface.height - 1 - y;

                const auto* source = surface.pixels.data() + static_cast<size_t>(source_y) * surface.width;

                auto* destination = output.data() + static_cast<size_t>(y) * stride;

                if (bpp == 1)
                {
                    for (uint32_t x = 0; x < surface.width; ++x)
                    {
                        if ((source[x] & 0x00FFFFFFu) != 0)
                        {
                            destination[x / 8u] |= static_cast<uint8_t>(0x80u >> (x & 7u));
                        }
                    }
                }
                else if (bpp == 32)
                {
                    std::memcpy(destination, source, static_cast<size_t>(surface.width) * sizeof(uint32_t));
                }
                else
                {
                    for (uint32_t x = 0; x < surface.width; ++x)
                    {
                        const uint32_t pixel = source[x];

                        destination[x * 3 + 0] = static_cast<uint8_t>(pixel);
                        destination[x * 3 + 1] = static_cast<uint8_t>(pixel >> 8);
                        destination[x * 3 + 2] = static_cast<uint8_t>(pixel >> 16);
                    }
                }
            }

            c.emu.write_memory(bits, output.data(), copy_size);
            return static_cast<int32_t>(copy_size);
        }

        uint64_t handle_NtGdiCreateDIBSection(const syscall_context& c, const hdc /*dc*/, const uint64_t /*section_app*/,
                                              const uint32_t /*offset*/, const emulator_pointer info, const uint32_t /*usage*/,
                                              const uint32_t header_size, const uint32_t /*flags*/, const uint64_t /*color_space*/,
                                              const emulator_object<emulator_pointer> bits)
        {
            if (info == 0 || !bits || header_size < sizeof(EMU_BITMAPINFOHEADER))
            {
                return 0;
            }

            EMU_BITMAPINFOHEADER header{};
            c.emu.read_memory(info, &header, sizeof(header));

            if (header.biSize < sizeof(EMU_BITMAPINFOHEADER) || header.biWidth <= 0 || header.biHeight == 0 || header.biBitCount == 0)
            {
                return 0;
            }

            const auto width = static_cast<uint32_t>(header.biWidth);
            const auto abs_height = header.biHeight > 0 ? static_cast<uint32_t>(header.biHeight)
                                                        : static_cast<uint32_t>(-static_cast<int64_t>(header.biHeight));

            const auto stride = static_cast<uint32_t>(dib_stride(width, header.biBitCount));

            const auto allocation_bytes = static_cast<uint64_t>(stride) * abs_height;

            if (allocation_bytes == 0 || allocation_bytes > std::numeric_limits<size_t>::max())
            {
                return 0;
            }

            const auto guest_bits = c.win_emu.memory.allocate_memory(static_cast<size_t>(page_align_up(allocation_bytes)),
                                                                     memory_permission::read_write, false, gdi_allocation_start(c));

            if (guest_bits == 0)
            {
                return 0;
            }

            std::vector<uint8_t> zeroed(static_cast<size_t>(allocation_bytes), 0);
            c.emu.write_memory(guest_bits, zeroed.data(), zeroed.size());

            gdi_bitmap_surface* surface = nullptr;
            const auto handle_value = create_gdi_bitmap_surface(c, width, abs_height, 0, &surface);
            if (handle_value == 0)
            {
                c.win_emu.memory.release_memory(guest_bits, 0);
                return 0;
            }

            surface->guest_bits = guest_bits;
            surface->guest_stride = stride;
            surface->guest_bpp = header.biBitCount;
            surface->guest_top_down = header.biHeight < 0;
            surface->guest_owns_memory = true;

            bits.write(guest_bits);
            return handle_value;
        }

        uint64_t handle_NtGdiCreateDIBitmapInternal(const syscall_context& c, const hdc /*dc*/, const int32_t width, const int32_t height,
                                                    const uint32_t init, const emulator_pointer bits, const emulator_pointer info,
                                                    const uint32_t usage, const uint32_t /*max_info*/, const uint32_t max_bits,
                                                    const uint32_t /*flags*/, const uint64_t /*color_transform*/)
        {
            // CreateDIBitmap passes the DIB's height through, which is negative for a top-down DIB.
            if (width <= 0 || height == 0)
            {
                return 0;
            }

            const auto surface_width = static_cast<uint32_t>(width);
            const auto surface_height = static_cast<uint32_t>(std::abs(static_cast<int64_t>(height)));
            gdi_bitmap_surface* surface = nullptr;
            const auto handle_value = create_gdi_bitmap_surface(c, surface_width, surface_height, k_default_bitmap_fill, &surface);
            if (!surface || (init & CBM_INIT) == 0 || bits == 0 || info == 0)
            {
                return handle_value;
            }

            const auto header = read_bitmap_info_header(c, info);
            if (!header || !is_supported_dib(*header, usage) || header->biHeight == 0)
            {
                c.win_emu.log.warn("NtGdiCreateDIBitmapInternal: unsupported initial DIB (usage=%u)\n", usage);
                return handle_value;
            }

            const bool top_down = header->biHeight < 0;
            const auto src_width = static_cast<uint32_t>(header->biWidth);
            const auto src_height = static_cast<uint32_t>(std::abs(static_cast<int64_t>(header->biHeight)));
            const auto bit_count = header->biBitCount;
            const auto stride = dib_stride(src_width, bit_count);
            const auto palette = read_dib_palette(c, info, *header);

            std::vector<uint8_t> data(stride * src_height);
            if (max_bits != 0 && data.size() > max_bits)
            {
                data.resize(max_bits);
            }
            c.emu.read_memory(bits, data.data(), data.size());
            const auto available_rows = static_cast<uint32_t>(data.size() / stride);

            for (uint32_t y = 0; y < std::min(surface_height, src_height); ++y)
            {
                const auto stored_row = top_down ? y : (src_height - 1 - y);
                if (stored_row >= available_rows)
                {
                    continue;
                }

                const uint8_t* row = data.data() + static_cast<size_t>(stored_row) * stride;
                for (uint32_t x = 0; x < std::min(surface_width, src_width); ++x)
                {
                    set_surface_pixel(*surface, static_cast<int>(x), static_cast<int>(y), dib_pixel_to_bgra32(row, x, bit_count, palette));
                }
            }

            return handle_value;
        }

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

            // With a buffer, the caller's header describes the requested format, and a negative height asks for top-down rows.
            int32_t requested_height = 0;
            if (bits != 0)
            {
                c.emu.read_memory(info + 8, &requested_height, sizeof(requested_height));
            }
            const bool top_down = requested_height < 0;

            const auto bi_width = static_cast<int32_t>(surface.width);
            const auto bi_height = top_down ? -static_cast<int32_t>(surface.height) : static_cast<int32_t>(surface.height);
            const uint16_t planes = 1;
            const uint16_t bit_count = surface.guest_bpp == 1 ? 1 : 32;
            const uint32_t compression = 0; // BI_RGB
            const auto stride = dib_stride(surface.width, bit_count);
            const auto size_image = static_cast<uint32_t>(stride * surface.height);
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
                const uint32_t src_y = top_down ? dib_row : surface.height - 1 - dib_row;
                const auto* source = surface.pixels.data() + static_cast<size_t>(src_y) * surface.width;
                if (bit_count == 1)
                {
                    std::vector<uint8_t> packed_row(stride);
                    for (uint32_t x = 0; x < surface.width; ++x)
                    {
                        if ((source[x] & 0x00FFFFFFu) != 0)
                        {
                            packed_row[x / 8u] |= static_cast<uint8_t>(0x80u >> (x & 7u));
                        }
                    }
                    c.emu.write_memory(bits + dst_offset, packed_row.data(), packed_row.size());
                }
                else
                {
                    c.emu.write_memory(bits + dst_offset, source, stride);
                }
                ++copied;
            }

            return static_cast<int>(copied);
        }

        int handle_NtGdiSetDIBitsToDeviceInternal(const syscall_context& c, const hdc dc, const int x_dest, const int y_dest,
                                                  const uint32_t width, const uint32_t height, const int x_src, const int y_src,
                                                  const uint32_t /*start_scan*/, const uint32_t scan_lines, const emulator_pointer bits,
                                                  const emulator_pointer info, const uint32_t color_use, const uint32_t max_bits,
                                                  const uint32_t /*max_info*/, const uint32_t /*transform_coordinates*/,
                                                  const uint64_t /*color_transform*/)
        {
            if (bits == 0 || info == 0)
            {
                return 0;
            }

            int32_t origin_x = 0;
            int32_t origin_y = 0;
            uint32_t present_handle = 0;
            gdi::clip_region visible{};
            gdi_bitmap_surface* surface = resolve_dc_draw_target(c, dc, origin_x, origin_y, present_handle, visible);
            if (!surface)
            {
                return 0;
            }

            const auto header = read_bitmap_info_header(c, info);
            if (!header)
            {
                c.win_emu.log.warn("NtGdiSetDIBitsToDeviceInternal: unsupported DIB header (size=%u)\n", c.emu.read_memory<uint32_t>(info));
                return 0;
            }

            const auto bit_count = header->biBitCount;
            if (!is_supported_dib(*header, color_use))
            {
                c.win_emu.log.warn("NtGdiSetDIBitsToDeviceInternal: unsupported DIB (bpp=%u compression=%u usage=%u width=%d)\n", bit_count,
                                   static_cast<uint32_t>(header->biCompression), color_use, static_cast<int32_t>(header->biWidth));
                return 0;
            }

            const bool top_down = header->biHeight < 0;
            const auto src_width = static_cast<uint32_t>(header->biWidth);
            const auto src_height = static_cast<uint32_t>(top_down ? -header->biHeight : header->biHeight);
            const auto stored_rows = std::min(scan_lines, src_height);
            const size_t stride = dib_stride(src_width, bit_count);

            const auto palette = read_dib_palette(c, info, *header);

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
                const int64_t left = static_cast<int64_t>(x_dest) + origin_x;
                const int64_t out_y = static_cast<int64_t>(y_dest) + origin_y + j;
                const auto first_column = std::max<int64_t>(0, -static_cast<int64_t>(x_src));
                const auto end_column = std::min<int64_t>(width, static_cast<int64_t>(src_width) - x_src);
                visible.for_each_span(clamped_to_surface(*surface, left + first_column, out_y, left + end_column, out_y + 1),
                                      [&](const LONG x_begin, const LONG x_end, const LONG y) {
                                          auto* out = surface_row(*surface, y);
                                          for (auto x = x_begin; x < x_end; ++x)
                                          {
                                              const auto src_x = static_cast<uint32_t>(x_src + (x - left));
                                              out[x] = dib_pixel_to_bgra32(row, src_x, bit_count, palette);
                                          }
                                      });
                ++copied;
            }

            present_win_surface(c, present_handle, surface);
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
                                              const uint32_t color_use, const uint32_t /*rop*/, const uint32_t /*max_info*/,
                                              const uint32_t max_bits, const uint64_t /*color_transform*/)
        {
            if (bits == 0 || info == 0 || dst_width == 0 || dst_height == 0 || src_width <= 0 || src_height <= 0)
            {
                return 0;
            }

            int32_t origin_x = 0;
            int32_t origin_y = 0;
            uint32_t present_handle = 0;
            gdi::clip_region visible{};
            gdi_bitmap_surface* surface = resolve_dc_draw_target(c, dc, origin_x, origin_y, present_handle, visible);
            if (!surface)
            {
                return 0;
            }

            const auto header = read_bitmap_info_header(c, info);
            if (!header)
            {
                c.win_emu.log.warn("NtGdiStretchDIBitsInternal: unsupported DIB header (size=%u)\n", c.emu.read_memory<uint32_t>(info));
                return 0;
            }

            const auto bit_count = header->biBitCount;
            if (!is_supported_dib(*header, color_use) || header->biHeight == 0)
            {
                c.win_emu.log.warn("NtGdiStretchDIBitsInternal: unsupported DIB (bpp=%u compression=%u usage=%u width=%d)\n", bit_count,
                                   static_cast<uint32_t>(header->biCompression), color_use, static_cast<int32_t>(header->biWidth));
                return 0;
            }

            const bool top_down = header->biHeight < 0;
            const auto img_width = static_cast<uint32_t>(header->biWidth);
            const auto img_height = static_cast<uint32_t>(top_down ? -header->biHeight : header->biHeight);

            const size_t stride = dib_stride(img_width, bit_count);

            const auto palette = read_dib_palette(c, info, *header);

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
                const int64_t out_y = static_cast<int64_t>(y_dst) + origin_y + (flip_y ? (dst_h - 1 - dy) : dy);
                const int64_t left = static_cast<int64_t>(x_dst) + origin_x;

                visible.for_each_span(clamped_to_surface(*surface, left, out_y, left + dst_w, out_y + 1),
                                      [&](const LONG x_begin, const LONG x_end, const LONG y) {
                                          auto* out = surface_row(*surface, y);
                                          for (auto x = x_begin; x < x_end; ++x)
                                          {
                                              const auto offset = x - left;
                                              const auto dx = flip_x ? dst_w - 1 - offset : offset;
                                              const auto img_x = static_cast<uint32_t>(x_src) +
                                                                 static_cast<uint32_t>(static_cast<int64_t>(dx) * src_width / dst_w);
                                              if (img_x < img_width)
                                              {
                                                  out[x] = dib_pixel_to_bgra32(row, img_x, bit_count, palette);
                                              }
                                          }
                                      });
            }

            present_win_surface(c, present_handle, surface);

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

            if (is_system_gdi_object(c, handle_value))
            {
                return 1;
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

            if (const auto bmp_it = c.proc.gdi_bitmap_surfaces.find(handle_value);
                bmp_it != c.proc.gdi_bitmap_surfaces.end() && bmp_it->second.guest_owns_memory && bmp_it->second.guest_bits != 0)
            {
                c.win_emu.memory.release_memory(bmp_it->second.guest_bits, 0);
            }
            if (entry.Object != 0)
            {
                c.win_emu.memory.release_memory(entry.Object, 0);
            }

            c.proc.gdi_dc_states.erase(handle_value);
            c.proc.gdi_bitmap_surfaces.erase(handle_value);
            c.proc.gdi_fonts.erase(handle_value);
            c.proc.gdi_regions.erase(handle_value);
            c.proc.gdi_palettes.erase(handle_value);
            return 1;
        }

        uint64_t handle_NtGdiSelectBitmap(const syscall_context& c, const hdc dc, const handle bitmap)
        {
            const auto it = c.proc.gdi_dc_states.find(static_cast<uint32_t>(dc));
            if (it == c.proc.gdi_dc_states.end())
            {
                return 0;
            }

            if (!it->second.is_memory_dc)
            {
                return 0;
            }

            const auto bitmap_handle = static_cast<uint32_t>(bitmap.bits);
            if (!c.proc.gdi_bitmap_surfaces.contains(bitmap_handle))
            {
                return 0;
            }

            if (bitmap_handle != c.proc.gdi_memory_dc_default_bitmap_handle)
            {
                for (const auto& [other_dc, state] : c.proc.gdi_dc_states)
                {
                    if (other_dc != static_cast<uint32_t>(dc) && state.selected_bitmap == bitmap_handle)
                    {
                        return 0;
                    }
                }
            }

            const auto old = it->second.selected_bitmap;
            it->second.selected_bitmap = bitmap_handle;
            return old;
        }

        uint64_t handle_NtGdiSelectFont(const syscall_context&, const hdc dc, const uint64_t font)
        {
            return dc != 0 ? font : 0;
        }

        BOOL handle_NtGdiFillRgn(const syscall_context& c, const hdc dc, const handle region, const hbrush brush)
        {
            uint64_t dc_attr = 0;
            if (!get_gdi_object_address(c, static_cast<uint32_t>(dc), k_gdi_dc_type, dc_attr))
            {
                return FALSE;
            }

            (void)handle_NtGdiFlush(c);

            const auto rects = read_gdi_region(c, static_cast<uint32_t>(region.bits));
            if (!rects)
            {
                return FALSE;
            }
            if (rects->empty())
            {
                return TRUE;
            }

            int32_t origin_x = 0;
            int32_t origin_y = 0;
            uint32_t present_handle = 0;
            gdi::clip_region visible{};
            auto* surface = resolve_dc_draw_target(c, dc, origin_x, origin_y, present_handle, visible);
            if (!surface)
            {
                return TRUE;
            }

            const auto is_visible = [&](const RECT& rect) {
                return static_cast<int64_t>(rect.left) + origin_x < surface->width &&
                       static_cast<int64_t>(rect.top) + origin_y < surface->height && static_cast<int64_t>(rect.right) + origin_x > 0 &&
                       static_cast<int64_t>(rect.bottom) + origin_y > 0;
            };
            if (!std::ranges::any_of(*rects, is_visible))
            {
                return TRUE;
            }

            const auto brush_handle = static_cast<uint32_t>(brush);
            GDI_HANDLE_ENTRY64 brush_entry{};
            uint64_t entry_address = 0;
            if (!read_gdi_entry_for_handle(c, brush_handle, brush_entry, entry_address) || brush_entry.Type != k_gdi_brush_type)
            {
                return FALSE;
            }

            auto color = get_brush_color_in_dc(c, dc, brush_handle);
            if (gdi_object_type(brush_entry) != k_gdi_brush_type)
            {
                if (brush_handle == static_cast<uint32_t>(read_gdi_object_slot(c, k_stock_null_pen_index)))
                {
                    return FALSE;
                }
                color = brush_handle == static_cast<uint32_t>(read_gdi_object_slot(c, k_stock_dc_pen_index))
                            ? read_dc_attr_colorref(c, dc_attr + k_gdi_dc_attr_dc_pen_color_offset)
                            : read_dc_attr_colorref(c, brush_entry.Object + sizeof(uint32_t));
            }
            if (!color)
            {
                return FALSE;
            }

            const auto rop2 = static_cast<uint8_t>(((get_dc_rop2(c, dc) - 1u) & 0xFu) + 1u);
            for (const auto& rect : *rects)
            {
                fill_rect_rop2(*surface, visible, static_cast<int64_t>(rect.left) + origin_x, static_cast<int64_t>(rect.top) + origin_y,
                               static_cast<int64_t>(rect.right) + origin_x, static_cast<int64_t>(rect.bottom) + origin_y, *color, rop2);
            }

            present_win_surface(c, present_handle, surface);
            return TRUE;
        }

        hdc handle_NtGdiGetDCforBitmap(const syscall_context& c, const handle bitmap)
        {
            const auto bitmap_handle = static_cast<uint32_t>(bitmap.bits);
            for (const auto& [dc_handle, dc_state] : c.proc.gdi_dc_states)
            {
                if (dc_state.is_memory_dc && dc_state.selected_bitmap == bitmap_handle)
                {
                    return static_cast<hdc>(dc_handle);
                }
            }

            return 0;
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

        uint64_t handle_NtGdiHfontCreate(const syscall_context& c, const emulator_pointer logfont, const uint32_t /*angle*/)
        {
            const auto handle = allocate_gdi_object(c, k_gdi_font_type, k_gdi_font_attr_size);
            if (handle != 0 && logfont != 0)
            {
                c.proc.gdi_fonts[handle] = emulator_object<EMU_LOGFONTW>{c.emu, logfont}.read();
            }
            return handle;
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

            const auto object_type = gdi_object_type(entry);
            const uint32_t object_size = get_gdi_object_size(object_type);
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

            std::vector<uint8_t> object_data(object_size, 0);

            if (object_type == k_gdi_palette_type)
            {
                const auto palette = c.proc.gdi_palettes.find(handle_value);
                const auto entry_count = static_cast<uint16_t>(palette == c.proc.gdi_palettes.end() ? 0 : palette->second.size());
                std::memcpy(object_data.data(), &entry_count, sizeof(entry_count));
            }
            else if (object_type == k_gdi_bitmap_type)
            {
                const auto bmp_it = c.proc.gdi_bitmap_surfaces.find(handle_value);
                if (bmp_it != c.proc.gdi_bitmap_surfaces.end())
                {
                    const auto& surface = bmp_it->second;
                    const auto bpp = static_cast<uint16_t>(surface.guest_bpp != 0 ? surface.guest_bpp : 32u);
                    const auto width_bytes =
                        surface.guest_stride != 0 ? surface.guest_stride : static_cast<uint32_t>(dib_stride(surface.width, bpp));

                    // BITMAP (32-bit layout): bmType@0, bmWidth@4, bmHeight@8, bmWidthBytes@12,
                    // bmPlanes@16 (WORD), bmBitsPixel@18 (WORD), bmBits@20 (PVOID32).
                    const utils::safe_buffer_accessor<uint8_t> bitmap{object_data};
                    bitmap.as<int32_t>(4).set(static_cast<int32_t>(surface.width));
                    bitmap.as<int32_t>(8).set(static_cast<int32_t>(surface.height));
                    bitmap.as<int32_t>(12).set(static_cast<int32_t>(width_bytes));
                    bitmap.as<uint16_t>(16).set(1);
                    bitmap.as<uint16_t>(18).set(bpp);
                    bitmap.as<uint32_t>(20).set(static_cast<uint32_t>(surface.guest_bits));
                }
            }
            else if (object_type == k_gdi_font_type)
            {
                if (const auto font_it = c.proc.gdi_fonts.find(handle_value); font_it != c.proc.gdi_fonts.end())
                {
                    std::memcpy(object_data.data(), &font_it->second, std::min(object_data.size(), sizeof(font_it->second)));
                }
            }

            c.emu.write_memory(buffer, object_data.data(), object_data.size());
            return object_size;
        }

        BOOL handle_NtGdiEnumFonts(const syscall_context& c, const hdc dc, const ULONG /*type*/, const ULONG /*win32_compat*/,
                                   const ULONG face_name_len, const emulator_pointer face_name, const ULONG charset,
                                   const emulator_pointer count, const emulator_pointer buffer)
        {
            if (dc == 0 || count == 0 || (face_name_len != 0 && face_name == 0))
            {
                return FALSE;
            }

            std::array<char16_t, 32> face_filter{};
            if (face_name_len != 0)
            {
                const auto copied_length = std::min<size_t>(face_name_len, face_filter.size() - 1);
                c.emu.read_memory(face_name, face_filter.data(), copied_length * sizeof(char16_t));
            }

            std::vector<font_enum_record> fonts;
            if (face_filter.front() == u'\0' ||
                utils::string::equals_ignore_case(std::u16string_view{face_filter.data()}, std::u16string_view{u"Segoe UI"}))
            {
                struct font_style
                {
                    std::u16string_view full_name;
                    std::u16string_view style;
                    LONG weight;
                    bool italic;
                    bool supports_arabic_and_hebrew;
                };

                constexpr std::array styles{
                    font_style{.full_name = u"Segoe UI",
                               .style = u"Regular",
                               .weight = FW_NORMAL,
                               .italic = false,
                               .supports_arabic_and_hebrew = true},
                    font_style{.full_name = u"Segoe UI Bold",
                               .style = u"Bold",
                               .weight = FW_BOLD,
                               .italic = false,
                               .supports_arabic_and_hebrew = true},
                    font_style{.full_name = u"Segoe UI Bold Italic",
                               .style = u"Bold Italic",
                               .weight = FW_BOLD,
                               .italic = true,
                               .supports_arabic_and_hebrew = false},
                    font_style{.full_name = u"Segoe UI Italic",
                               .style = u"Italic",
                               .weight = FW_NORMAL,
                               .italic = true,
                               .supports_arabic_and_hebrew = false},
                };
                constexpr std::array<std::pair<BYTE, std::u16string_view>, 10> scripts{
                    std::pair{static_cast<BYTE>(ANSI_CHARSET), u"Western"sv},
                    std::pair{static_cast<BYTE>(SHIFTJIS_CHARSET), u"Japanese"sv},
                    std::pair{static_cast<BYTE>(HEBREW_CHARSET), u"Hebrew"sv},
                    std::pair{static_cast<BYTE>(ARABIC_CHARSET), u"Arabic"sv},
                    std::pair{static_cast<BYTE>(GREEK_CHARSET), u"Greek"sv},
                    std::pair{static_cast<BYTE>(TURKISH_CHARSET), u"Turkish"sv},
                    std::pair{static_cast<BYTE>(BALTIC_CHARSET), u"Baltic"sv},
                    std::pair{static_cast<BYTE>(EASTEUROPE_CHARSET), u"Central European"sv},
                    std::pair{static_cast<BYTE>(RUSSIAN_CHARSET), u"Cyrillic"sv},
                    std::pair{static_cast<BYTE>(VIETNAMESE_CHARSET), u"Vietnamese"sv},
                };

                for (const auto& style : styles)
                {
                    gdi_font_signature signature{};
                    for (const auto& supported_script : scripts)
                    {
                        const auto font_charset = supported_script.first;
                        if (style.supports_arabic_and_hebrew || (font_charset != ARABIC_CHARSET && font_charset != HEBREW_CHARSET))
                        {
                            add_gdi_font_charset(signature, font_charset);
                        }
                    }

                    for (const auto& [font_charset, script] : scripts)
                    {
                        if ((!style.supports_arabic_and_hebrew && (font_charset == ARABIC_CHARSET || font_charset == HEBREW_CHARSET)) ||
                            (charset != DEFAULT_CHARSET && charset != font_charset))
                        {
                            continue;
                        }

                        auto& font = fonts.emplace_back();
                        font.size = sizeof(font);
                        font.text_metric_offset = offsetof(font_enum_record, text_metric) - offsetof(font_enum_record, type);
                        font.type = TRUETYPE_FONTTYPE;
                        font.log_font.elfLogFont.lfHeight = k_default_font_height;
                        font.log_font.elfLogFont.lfWeight = style.weight;
                        font.log_font.elfLogFont.lfItalic = style.italic ? UINT8_MAX : 0;
                        font.log_font.elfLogFont.lfCharSet = font_charset;
                        font.log_font.elfLogFont.lfPitchAndFamily = DEFAULT_PITCH | FF_SWISS;
                        utils::string::copy(font.log_font.elfLogFont.lfFaceName, u"Segoe UI");
                        utils::string::copy(font.log_font.elfFullName, style.full_name);
                        utils::string::copy(font.log_font.elfStyle, style.style);
                        utils::string::copy(font.log_font.elfScript, script);
                        font.text_metric.ntmTm.tmHeight = k_default_font_height;
                        font.text_metric.ntmTm.tmAscent = k_default_font_ascent;
                        font.text_metric.ntmTm.tmDescent = k_default_font_descent;
                        font.text_metric.ntmTm.tmAveCharWidth = k_default_font_width;
                        font.text_metric.ntmTm.tmMaxCharWidth = k_default_font_width;
                        font.text_metric.ntmTm.tmWeight = style.weight;
                        font.text_metric.ntmTm.tmItalic = style.italic ? UINT8_MAX : 0;
                        font.text_metric.ntmTm.tmCharSet = font_charset;
                        font.text_metric.ntmTm.tmPitchAndFamily = DEFAULT_PITCH | FF_SWISS;
                        font.text_metric.ntmTm.ntmFlags =
                            (style.weight == FW_BOLD ? NTM_BOLD : NTM_REGULAR) | (style.italic ? NTM_ITALIC : 0);

                        std::ranges::copy(signature.unicode_subsets, font.text_metric.fsUsb);
                        std::ranges::copy(signature.code_pages, font.text_metric.fsCsb);
                    }
                }
            }

            const auto required_size = static_cast<uint32_t>(fonts.size() * sizeof(font_enum_record));
            if (buffer == 0)
            {
                c.emu.write_memory(count, &required_size, sizeof(required_size));
                return TRUE;
            }

            uint32_t buffer_size = 0;
            c.emu.read_memory(count, &buffer_size, sizeof(buffer_size));
            const auto written_size = std::min(buffer_size, required_size) / sizeof(font_enum_record) * sizeof(font_enum_record);
            if (written_size != 0)
            {
                c.emu.write_memory(buffer, fonts.data(), written_size);
            }
            c.emu.write_memory(count, &written_size, sizeof(written_size));
            return TRUE;
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

            static constexpr std::u16string_view k_default_font_name = u"Segoe UI";
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
                c.emu.write_memory(face_name, k_default_font_name.data(), static_cast<size_t>(writable_chars) * sizeof(char16_t));
            }

            const char16_t terminator = u'\0';
            c.emu.write_memory(face_name + static_cast<uint64_t>(writable_chars) * sizeof(char16_t), &terminator, sizeof(terminator));
            return required;
        }

        uint32_t handle_NtGdiGetKerningPairs(const syscall_context&, const hdc /*dc*/, const uint32_t /*pair_count*/,
                                             const emulator_pointer /*pairs*/)
        {
            return 0;
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

        BOOL handle_NtGdiGetTextExtentExW(const syscall_context& c, const hdc dc, const emulator_pointer text, const ULONG char_count,
                                          const ULONG max_extent, const emulator_object<ULONG> fit, const emulator_pointer extents,
                                          const emulator_pointer size, const ULONG /*flags*/)
        {
            // Windows answers an empty string without looking at the DC or the text.
            if (char_count == 0)
            {
                constexpr SIZE empty_size{};
                constexpr ULONG no_fit = 0;
                if (size == 0 || !write_guest_block(c, size, &empty_size, sizeof(empty_size)))
                {
                    return FALSE;
                }

                if (fit && !write_guest_block(c, fit.value(), &no_fit, sizeof(no_fit)))
                {
                    return FALSE;
                }

                return TRUE;
            }

            // Windows reads the whole text before writing anything, although the fixed-width measure does not need it.
            if (dc == 0 || size == 0 || char_count > max_text_extent_chars ||
                !is_readable_range(c.win_emu.memory, text, static_cast<uint64_t>(char_count) * sizeof(char16_t)))
            {
                return FALSE;
            }

            const SIZE text_size{
                .cx = static_cast<LONG>(k_default_font_width * char_count),
                .cy = static_cast<LONG>(k_default_font_height),
            };
            if (!write_guest_block(c, size, &text_size, sizeof(text_size)))
            {
                return FALSE;
            }

            const ULONG fitting = std::min<ULONG>(char_count, max_extent / k_default_font_width);
            if (fit && !write_guest_block(c, fit.value(), &fitting, sizeof(fitting)))
            {
                return FALSE;
            }

            const ULONG extent_count = fit ? fitting : char_count;
            if (extents != 0 && !write_fixed_width_extents(c, extents, extent_count))
            {
                return FALSE;
            }

            return TRUE;
        }

        BOOL handle_NtGdiGetCharWidthW(const syscall_context& c, const hdc dc, const UINT /*first_char*/, const UINT char_count,
                                       const emulator_pointer chars, const UINT /*flags*/, const emulator_pointer buffer)
        {
            if (!is_char_metric_request_valid(c, dc, char_count, sizeof(int32_t), chars, buffer))
            {
                return FALSE;
            }

            const auto width = static_cast<int32_t>(k_default_font_width);
            return write_repeated_entries(c, buffer, char_count, width) ? TRUE : FALSE;
        }

        BOOL handle_NtGdiGetCharABCWidthsW(const syscall_context& c, const hdc dc, const UINT /*first_char*/, const UINT char_count,
                                           const emulator_pointer chars, const UINT /*flags*/, const emulator_pointer buffer)
        {
            if (!is_char_metric_request_valid(c, dc, char_count, sizeof(ABC), chars, buffer))
            {
                return FALSE;
            }

            const ABC metrics{
                .abcA = 0,
                .abcB = k_default_font_width,
                .abcC = 0,
            };
            return write_repeated_entries(c, buffer, char_count, metrics) ? TRUE : FALSE;
        }

        uint32_t handle_NtGdiGetGlyphOutline(const syscall_context& c, const hdc dc, const UINT character, const UINT format,
                                             const emulator_pointer glyph_metrics, const DWORD buffer_size, const emulator_pointer buffer,
                                             const emulator_pointer mat2)
        {
            static constexpr uint32_t gdi_error = 0xFFFFFFFF;
            static constexpr uint32_t ggo_metrics = 0;
            static constexpr uint32_t ggo_bitmap = 1;
            static constexpr uint32_t ggo_native = 2;
            static constexpr uint32_t ggo_gray2_bitmap = 4;
            static constexpr uint32_t ggo_gray4_bitmap = 5;
            static constexpr uint32_t ggo_gray8_bitmap = 6;
            static constexpr uint32_t format_mask = 0x7;
            static constexpr uint32_t glyph_width = 8;
            static constexpr uint32_t glyph_height = 16;
            static constexpr int32_t glyph_ascent = 12;

            if (dc == 0 || glyph_metrics == 0 || mat2 == 0)
            {
                return gdi_error;
            }

            const auto write_u32 = [&](const uint32_t offset, const uint32_t value) {
                c.emu.write_memory(glyph_metrics + offset, &value, sizeof(value));
            };
            const auto write_i32 = [&](const uint32_t offset, const int32_t value) {
                c.emu.write_memory(glyph_metrics + offset, &value, sizeof(value));
            };
            const auto write_i16 = [&](const uint32_t offset, const int16_t value) {
                c.emu.write_memory(glyph_metrics + offset, &value, sizeof(value));
            };

            write_u32(0x00, glyph_width);
            write_u32(0x04, glyph_height);
            write_i32(0x08, 0);
            write_i32(0x0C, glyph_ascent);
            write_i16(0x10, static_cast<int16_t>(glyph_width));
            write_i16(0x12, 0);

            const auto glyph_format = format & format_mask;
            if (glyph_format == ggo_metrics)
            {
                return 0;
            }

            const auto make_pointfx = [](const int16_t x, const int16_t y) {
                return POINTFX{
                    .x = {.fract = 0, .value = x},
                    .y = {.fract = 0, .value = y},
                };
            };
            const auto append_outline = []<typename T>(std::vector<uint8_t>& outline, const T& value) {
                const auto* const bytes = reinterpret_cast<const uint8_t*>(&value);
                outline.insert(outline.end(), bytes, bytes + sizeof(value));
            };
            const auto make_native_outline = [&]() {
                std::vector<uint8_t> outline;
                const std::array<POINTFX, 4> points{
                    make_pointfx(static_cast<int16_t>(glyph_width), 0),
                    make_pointfx(static_cast<int16_t>(glyph_width), static_cast<int16_t>(glyph_height)),
                    make_pointfx(0, static_cast<int16_t>(glyph_height)),
                    make_pointfx(0, 0),
                };

                const EMU_TTPOLYCURVE_HEADER curve{
                    .wType = 1,
                    .cpfx = static_cast<uint16_t>(points.size()),
                };
                const EMU_TTPOLYGONHEADER header{
                    .cb = static_cast<uint32_t>(sizeof(EMU_TTPOLYGONHEADER) + sizeof(EMU_TTPOLYCURVE_HEADER) + sizeof(points)),
                    .dwType = 24,
                    .pfxStart = make_pointfx(0, 0),
                };

                outline.reserve(header.cb);
                append_outline(outline, header);
                append_outline(outline, curve);
                append_outline(outline, points);
                return outline;
            };

            std::vector<uint8_t> native_outline;
            uint32_t required_size = 0;
            switch (glyph_format)
            {
            case ggo_bitmap:
                required_size = ((glyph_width + 31) / 32) * 4 * glyph_height;
                break;
            case ggo_gray2_bitmap:
            case ggo_gray4_bitmap:
            case ggo_gray8_bitmap:
                required_size = glyph_width * glyph_height;
                break;
            case ggo_native:
                native_outline = make_native_outline();
                required_size = static_cast<uint32_t>(native_outline.size());
                break;
            default:
                return gdi_error;
            }

            if (buffer == 0 || buffer_size == 0)
            {
                return required_size;
            }

            if (buffer_size < required_size)
            {
                return gdi_error;
            }

            if (glyph_format == ggo_native)
            {
                c.emu.write_memory(buffer, native_outline.data(), native_outline.size());
            }
            else
            {
                std::vector<uint8_t> bitmap(required_size);
                gdi_bitmap_surface glyph_surface{
                    .width = glyph_width,
                    .height = glyph_height,
                    .pixels = std::vector<uint32_t>(glyph_width * glyph_height),
                };
                const auto glyph_character = static_cast<char16_t>(
                    character >= debug_font::first_codepoint && character <= debug_font::last_codepoint ? character : u'.');
                draw_text(glyph_surface, gdi::clip_region::whole(glyph_width, glyph_height), 0, 0, std::u16string_view{&glyph_character, 1},
                          0xFFFFFFFFu);

                if (glyph_format == ggo_bitmap)
                {
                    constexpr auto row_size = ((glyph_width + 31) / 32) * 4;
                    for (uint32_t y = 0; y < glyph_height; ++y)
                    {
                        for (uint32_t x = 0; x < glyph_width; ++x)
                        {
                            if (glyph_surface.pixels[y * glyph_width + x] != 0)
                            {
                                bitmap[y * row_size + x / 8] |= static_cast<uint8_t>(0x80u >> (x % 8));
                            }
                        }
                    }
                }
                else
                {
                    uint8_t intensity = 64;
                    if (glyph_format == ggo_gray2_bitmap)
                    {
                        intensity = 4;
                    }
                    else if (glyph_format == ggo_gray4_bitmap)
                    {
                        intensity = 16;
                    }
                    for (uint32_t y = 0; y < glyph_height; ++y)
                    {
                        for (uint32_t x = 0; x < glyph_width; ++x)
                        {
                            if (glyph_surface.pixels[y * glyph_width + x] != 0)
                            {
                                bitmap[y * glyph_width + x] = intensity;
                            }
                        }
                    }
                }
                c.emu.write_memory(buffer, bitmap.data(), bitmap.size());
            }
            return required_size;
        }

        uint32_t handle_NtGdiGetOutlineTextMetricsInternalW(const syscall_context& c, const hdc dc, const uint32_t cj_copy,
                                                            const emulator_pointer metrics, const emulator_pointer unknown)
        {
            if (dc == 0)
            {
                return 0;
            }

            static constexpr uint32_t k_outline_text_metric_fixed_size = 0xE8;
            static constexpr uint32_t k_outline_text_metric_text_metric_offset = 0x04;
            static constexpr std::u16string_view k_family_name = u"Segoe UI";
            static constexpr std::u16string_view k_face_name = u"Segoe UI";
            static constexpr std::u16string_view k_style_name = u"Normal";
            static constexpr std::u16string_view k_full_name = u"Segoe UI Regular";

            const auto string_bytes = [](const std::u16string_view str) {
                return static_cast<uint32_t>((str.size() + 1) * sizeof(char16_t));
            };
            const auto ansi_string_bytes = [](const std::u16string_view str) { return static_cast<uint32_t>(str.size() + 1); };

            const auto required_size = k_outline_text_metric_fixed_size + string_bytes(k_family_name) + string_bytes(k_face_name) +
                                       string_bytes(k_style_name) + string_bytes(k_full_name);
            const auto required_size_a = k_outline_text_metric_fixed_size + ansi_string_bytes(k_family_name) +
                                         ansi_string_bytes(k_face_name) + ansi_string_bytes(k_style_name) + ansi_string_bytes(k_full_name);

            if (unknown != 0)
            {
                const auto required_size_a64 = static_cast<uint64_t>(required_size_a);
                c.emu.write_memory(unknown, &required_size_a64, sizeof(required_size_a64));
            }

            if (metrics == 0 || cj_copy == 0)
            {
                return required_size;
            }

            if (cj_copy < required_size)
            {
                return 0;
            }

            std::vector<uint8_t> zeroed(required_size);
            c.emu.write_memory(metrics, zeroed.data(), zeroed.size());

            const auto write_u32 = [&](const uint32_t offset, const uint32_t value) {
                c.emu.write_memory(metrics + offset, &value, sizeof(value));
            };
            const auto write_i32 = [&](const uint32_t offset, const int32_t value) {
                c.emu.write_memory(metrics + offset, &value, sizeof(value));
            };
            const auto write_u16 = [&](const uint32_t offset, const uint16_t value) {
                c.emu.write_memory(metrics + offset, &value, sizeof(value));
            };
            const auto write_u8 = [&](const uint32_t offset, const uint8_t value) {
                c.emu.write_memory(metrics + offset, &value, sizeof(value));
            };
            const auto write_offset = [&](const uint32_t pointer_offset, const uint32_t string_offset_value) {
                const auto value64 = static_cast<uint64_t>(string_offset_value);
                c.emu.write_memory(metrics + pointer_offset, &value64, sizeof(value64));
            };

            write_u32(0x00, required_size);

            const auto tm = k_outline_text_metric_text_metric_offset;
            write_u32(tm + 0x00, k_default_font_height);
            write_u32(tm + 0x04, k_default_font_ascent);
            write_u32(tm + 0x08, k_default_font_descent);
            write_u32(tm + 0x14, k_default_font_width);
            write_u32(tm + 0x18, k_default_font_width);
            write_u32(tm + 0x1C, k_default_font_weight);
            write_u16(tm + 0x2C, 0x20);
            write_u16(tm + 0x2E, 0x7E);
            write_u16(tm + 0x30, 0x3F);
            write_u16(tm + 0x32, 0x20);
            write_u8(tm + 0x38, 0x01);

            write_u32(0x60, 2048);
            write_i32(0x64, k_default_font_ascent);
            write_i32(0x68, -static_cast<int32_t>(k_default_font_descent));
            write_u32(0x6C, 0);
            write_u32(0x70, k_default_font_ascent);
            write_u32(0x74, k_default_font_height / 2);
            write_i32(0x78, 0);
            write_i32(0x7C, -static_cast<int32_t>(k_default_font_descent));
            write_i32(0x80, k_default_font_width);
            write_i32(0x84, k_default_font_ascent);
            write_i32(0x88, k_default_font_ascent);
            write_i32(0x8C, -static_cast<int32_t>(k_default_font_descent));
            write_u32(0x90, 0);
            write_u32(0x94, 8);
            write_i32(0x98, k_default_font_width / 2);
            write_i32(0x9C, k_default_font_height / 2);
            write_i32(0xA0, 0);
            write_i32(0xA4, -static_cast<int32_t>(k_default_font_descent / 2));
            write_i32(0xA8, k_default_font_width / 2);
            write_i32(0xAC, k_default_font_height / 2);
            write_i32(0xB0, 0);
            write_i32(0xB4, k_default_font_ascent / 2);
            write_u32(0xB8, 1);
            write_i32(0xBC, k_default_font_height / 3);
            write_i32(0xC0, 1);
            write_i32(0xC4, -1);

            auto string_offset = k_outline_text_metric_fixed_size;
            const auto write_string = [&](const uint32_t pointer_offset, const std::u16string_view str) {
                const auto string_ptr = metrics + string_offset;
                write_offset(pointer_offset, string_offset);
                c.emu.write_memory(string_ptr, str.data(), str.size() * sizeof(char16_t));

                constexpr char16_t terminator = u'\0';
                c.emu.write_memory(string_ptr + str.size() * sizeof(char16_t), &terminator, sizeof(terminator));
                string_offset += string_bytes(str);
            };

            write_string(0xC8, k_family_name);
            write_string(0xD0, k_face_name);
            write_string(0xD8, k_style_name);
            write_string(0xE0, k_full_name);

            return required_size;
        }

        NTSTATUS handle_NtGdiExtCreateRegion()
        {
            return STATUS_SUCCESS;
        }

        uint64_t handle_NtGdiCreateRectRgn(const syscall_context& c, const LONG x_left, const LONG y_top, const LONG x_right,
                                           const LONG y_bottom)
        {
            const auto region = allocate_gdi_object(c, k_gdi_region_type, k_gdi_region_attr_size);
            const RECT rect{
                .left = (std::min)(x_left, x_right),
                .top = (std::min)(y_top, y_bottom),
                .right = (std::max)(x_left, x_right),
                .bottom = (std::max)(y_top, y_bottom),
            };
            (void)store_gdi_region(c, region, rect_region(rect));
            return region;
        }

        bool set_gdi_region_rect(const syscall_context& c, const handle region, const RECT& rect)
        {
            return store_gdi_region(c, static_cast<uint32_t>(region.bits), rect_region(rect)) != RGN_ERROR;
        }

        int32_t handle_NtGdiCombineRgn(const syscall_context& c, const handle destination, const handle first_source,
                                       const handle second_source, const int32_t mode)
        {
            if (mode < RGN_AND || mode > RGN_COPY)
            {
                c.win_emu.log.error("Unsupported region combine mode: %d\n", mode);
                set_guest_last_error(c, k_error_invalid_parameter);
                return RGN_ERROR;
            }

            const auto first = read_gdi_region(c, static_cast<uint32_t>(first_source.bits));
            const auto second =
                mode == RGN_COPY ? std::optional{std::vector<RECT>{}} : read_gdi_region(c, static_cast<uint32_t>(second_source.bits));
            if (!first || !second)
            {
                set_guest_last_error(c, k_error_invalid_handle);
                return RGN_ERROR;
            }

            const auto result = store_gdi_region(c, static_cast<uint32_t>(destination.bits), combine_regions(*first, *second, mode));
            if (result == RGN_ERROR)
            {
                set_guest_last_error(c, k_error_invalid_handle);
            }

            return result;
        }

        BOOL handle_NtGdiEqualRgn(const syscall_context& c, const handle first_region, const handle second_region)
        {
            const auto first = read_gdi_region(c, static_cast<uint32_t>(first_region.bits));
            const auto second = read_gdi_region(c, static_cast<uint32_t>(second_region.bits));
            if (!first || !second)
            {
                return FALSE;
            }

            return combine_regions(*first, *second, RGN_XOR).empty() ? TRUE : FALSE;
        }

        int32_t handle_NtGdiGetRandomRgn(const syscall_context&, const hdc dc, const uint64_t region, const LONG /*index*/)
        {
            return (dc != 0 && region != 0) ? 1 : 0;
        }

        uint32_t handle_NtGdiGetRegionData(const syscall_context& c, const handle hrgn, const ULONG buffer_size,
                                           const emulator_pointer region_data)
        {
            const auto rects = read_gdi_region(c, static_cast<uint32_t>(hrgn.bits));
            if (!rects)
            {
                return 0;
            }

            const auto rects_bytes = rects->size() * sizeof(RECT);
            if (rects_bytes > std::numeric_limits<DWORD>::max() - sizeof(RGNDATAHEADER))
            {
                return 0;
            }

            const auto rects_size = static_cast<DWORD>(rects_bytes);
            const auto required_size = static_cast<DWORD>(sizeof(RGNDATAHEADER) + rects_bytes);
            if (region_data == 0)
            {
                return required_size;
            }

            if (buffer_size < required_size)
            {
                set_guest_last_error(c, k_error_invalid_parameter);
                return 0;
            }

            RGNDATAHEADER header{};
            header.dwSize = sizeof(header);
            header.iType = RDH_RECTANGLES;
            header.nCount = static_cast<DWORD>(rects->size());
            header.nRgnSize = rects_size;
            header.rcBound = region_bounds(*rects);
            c.emu.write_memory(region_data, &header, sizeof(header));
            if (!rects->empty())
            {
                c.emu.write_memory(region_data + sizeof(header), rects->data(), rects_size);
            }

            return required_size;
        }

        int32_t handle_NtGdiGetAppClipBox(const syscall_context& c, const hdc dc, const emulator_object<RECT> rect)
        {
            if (dc == 0 || !rect)
            {
                return 0;
            }

            const auto dc_it = c.proc.gdi_dc_states.find(static_cast<uint32_t>(dc));
            if (dc_it == c.proc.gdi_dc_states.end())
            {
                return 0;
            }

            RECT clip_rect{};
            const auto& dc_state = dc_it->second;

            if (dc_state.selected_bitmap != 0)
            {
                const auto bmp_it = c.proc.gdi_bitmap_surfaces.find(dc_state.selected_bitmap);
                if (bmp_it == c.proc.gdi_bitmap_surfaces.end())
                {
                    return 0;
                }

                clip_rect.right = static_cast<LONG>(bmp_it->second.width);
                clip_rect.bottom = static_cast<LONG>(bmp_it->second.height);
                rect.write(clip_rect);
                return 2;
            }

            const auto* win = dc_state.target_window != 0 ? c.proc.windows.get(dc_state.target_window) : nullptr;
            if (!win)
            {
                return 0;
            }

            clip_rect.right = win->width;
            clip_rect.bottom = win->height;
            rect.write(clip_rect);
            return 2;
        }

        int32_t handle_NtGdiExcludeClipRect(const syscall_context&, const hdc dc, const LONG /*x_left*/, const LONG /*y_top*/,
                                            const LONG /*x_right*/, const LONG /*y_bottom*/)
        {
            return dc != 0 ? 2 : 0;
        }

        int32_t handle_NtGdiIntersectClipRect(const syscall_context&, const hdc dc, const LONG /*x_left*/, const LONG /*y_top*/,
                                              const LONG /*x_right*/, const LONG /*y_bottom*/)
        {
            return dc != 0 ? 2 : 0;
        }

        BOOL handle_NtGdiRectVisible(const syscall_context& c, const hdc dc, const emulator_object<RECT> rect)
        {
            if (dc == 0 || !rect)
            {
                return FALSE;
            }

            RECT target = rect.read();
            if (target.left >= target.right || target.top >= target.bottom)
            {
                return FALSE;
            }

            const auto dc_it = c.proc.gdi_dc_states.find(static_cast<uint32_t>(dc));
            if (dc_it == c.proc.gdi_dc_states.end())
            {
                return FALSE;
            }

            RECT clip{};
            const auto& dc_state = dc_it->second;
            if (dc_state.selected_bitmap != 0)
            {
                const auto bmp_it = c.proc.gdi_bitmap_surfaces.find(dc_state.selected_bitmap);
                if (bmp_it == c.proc.gdi_bitmap_surfaces.end())
                {
                    return FALSE;
                }

                clip.right = static_cast<LONG>(bmp_it->second.width);
                clip.bottom = static_cast<LONG>(bmp_it->second.height);
            }
            else
            {
                const auto* win = dc_state.target_window != 0 ? c.proc.windows.get(dc_state.target_window) : nullptr;
                if (!win)
                {
                    return FALSE;
                }

                clip.right = win->width;
                clip.bottom = win->height;
            }

            const LONG visible_left = (std::max)(target.left, clip.left);
            const LONG visible_top = (std::max)(target.top, clip.top);
            const LONG visible_right = (std::min)(target.right, clip.right);
            const LONG visible_bottom = (std::min)(target.bottom, clip.bottom);
            return (visible_left < visible_right && visible_top < visible_bottom) ? TRUE : FALSE;
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
            gdi::clip_region visible{};
            if (!get_dc_state_and_draw_target(c, dc, dc_state, surface, origin_x, origin_y, visible) || !dc_state || !surface)
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

            if (!is_stock_object_selected(c, dc, k_gdi_dc_attr_hpen_offset, k_stock_null_pen_index))
            {
                const auto color = get_dc_pen_color(c, dc);
                draw_line(*surface, visible, dc_state->current_x + origin_x, dc_state->current_y + origin_y, x_end + origin_x,
                          y_end + origin_y, color);
            }

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
            uint64_t dc_attr = 0;
            if (!get_gdi_object_address(c, static_cast<uint32_t>(dc), k_gdi_dc_type, dc_attr))
            {
                set_guest_last_error(c, k_error_invalid_handle);
                return FALSE;
            }

            (void)handle_NtGdiFlush(c);

            gdi_dc_state* dc_state = nullptr;
            gdi_bitmap_surface* surface = nullptr;
            int32_t origin_x = 0;
            int32_t origin_y = 0;
            uint32_t present_handle = 0;
            gdi::clip_region visible{};
            if (!get_dc_state_and_draw_target(c, dc, dc_state, surface, origin_x, origin_y, visible, &present_handle) || !surface)
            {
                return TRUE;
            }

            const auto x0 = static_cast<int64_t>((std::min)(left, right)) + origin_x;
            const auto y0 = static_cast<int64_t>((std::min)(top, bottom)) + origin_y;
            const auto x1 = static_cast<int64_t>((std::max)(left, right)) + origin_x;
            const auto y1 = static_cast<int64_t>((std::max)(top, bottom)) + origin_y;
            const auto rop2 = get_dc_rop2(c, dc);
            const auto has_outline = !is_stock_object_selected(c, dc, k_gdi_dc_attr_hpen_offset, k_stock_null_pen_index);

            if (const auto brush_color = get_dc_brush_color(c, dc))
            {
                const auto inset = has_outline ? 1 : 0;
                fill_rect_rop2(*surface, visible, x0 + inset, y0 + inset, x1 - 1, y1 - 1, *brush_color, rop2);
            }

            if (has_outline)
            {
                draw_rectangle_outline(*surface, visible, x0, y0, x1, y1, get_dc_pen_color(c, dc), rop2);
            }

            present_win_surface(c, present_handle, surface);
            return TRUE;
        }

        BOOL handle_NtGdiBitBlt(const syscall_context& c, const hdc dst_dc, const int x_dst, const int y_dst, const int width,
                                const int height, const hdc src_dc, const int x_src, const int y_src, const DWORD rop,
                                const DWORD /*cr_back_color*/, const FLONG /*fl*/)
        {
            if (dst_dc == 0 || width <= 0 || height <= 0)
            {
                return FALSE;
            }

            // Make pending batched GDI output visible before using either DC as source/destination.
            (void)handle_NtGdiFlush(c);

            int32_t dst_origin_x = 0;
            int32_t dst_origin_y = 0;
            uint32_t present_handle = 0;
            gdi::clip_region visible{};
            gdi_bitmap_surface* dst_surface = resolve_dc_draw_target(c, dst_dc, dst_origin_x, dst_origin_y, present_handle, visible);
            if (!dst_surface || dst_surface->width == 0 || dst_surface->height == 0 || dst_surface->pixels.empty())
            {
                return FALSE;
            }

            const auto rop3 = static_cast<uint8_t>((rop >> 16) & 0xFFu);
            const bool needs_source = rop3_uses_source(rop3);

            int32_t src_origin_x = 0;
            int32_t src_origin_y = 0;
            gdi_bitmap_surface* src_surface = nullptr;

            if (needs_source)
            {
                uint32_t unused_present_handle = 0;
                if (src_dc == 0)
                {
                    return FALSE;
                }

                src_surface = resolve_dc_surface(c, src_dc, src_origin_x, src_origin_y, unused_present_handle);
                if (!src_surface || src_surface->width == 0 || src_surface->height == 0 || src_surface->pixels.empty())
                {
                    return FALSE;
                }
            }

            auto dx = static_cast<int64_t>(x_dst) + dst_origin_x;
            auto dy = static_cast<int64_t>(y_dst) + dst_origin_y;
            auto sx = static_cast<int64_t>(x_src) + src_origin_x;
            auto sy = static_cast<int64_t>(y_src) + src_origin_y;
            int64_t w = width;
            int64_t h = height;

            // Clip to destination surface.
            if (dx < 0)
            {
                const int64_t delta = -dx;
                dx = 0;
                sx += delta;
                w -= delta;
            }
            if (dy < 0)
            {
                const int64_t delta = -dy;
                dy = 0;
                sy += delta;
                h -= delta;
            }
            if (dx + w > static_cast<int64_t>(dst_surface->width))
            {
                w = static_cast<int64_t>(dst_surface->width) - dx;
            }
            if (dy + h > static_cast<int64_t>(dst_surface->height))
            {
                h = static_cast<int64_t>(dst_surface->height) - dy;
            }

            // Clip to source surface only when the ROP actually depends on S.
            if (needs_source)
            {
                if (sx < 0)
                {
                    const int64_t delta = -sx;
                    sx = 0;
                    dx += delta;
                    w -= delta;
                }
                if (sy < 0)
                {
                    const int64_t delta = -sy;
                    sy = 0;
                    dy += delta;
                    h -= delta;
                }
                if (sx + w > static_cast<int64_t>(src_surface->width))
                {
                    w = static_cast<int64_t>(src_surface->width) - sx;
                }
                if (sy + h > static_cast<int64_t>(src_surface->height))
                {
                    h = static_cast<int64_t>(src_surface->height) - sy;
                }
            }

            if (w <= 0 || h <= 0)
            {
                return TRUE;
            }

            const auto blt_width = static_cast<size_t>(w);
            const auto blt_height = static_cast<size_t>(h);
            const auto pixel_count = blt_width * blt_height;

            if (blt_width != 0 && pixel_count / blt_width != blt_height)
            {
                return FALSE;
            }

            std::vector<uint32_t> dst_snapshot(pixel_count);
            std::vector<uint32_t> src_snapshot;
            if (needs_source)
            {
                src_snapshot.resize(pixel_count);
            }

            // Snapshot first. This avoids corrupting overlapping source/destination blits.
            for (size_t row = 0; row < blt_height; ++row)
            {
                const auto dst_index = (static_cast<size_t>(dy) + row) * dst_surface->width + static_cast<size_t>(dx);
                std::memcpy(dst_snapshot.data() + row * blt_width, dst_surface->pixels.data() + dst_index, blt_width * sizeof(uint32_t));

                if (needs_source)
                {
                    const auto src_index = (static_cast<size_t>(sy) + row) * src_surface->width + static_cast<size_t>(sx);
                    std::memcpy(src_snapshot.data() + row * blt_width, src_surface->pixels.data() + src_index,
                                blt_width * sizeof(uint32_t));
                }
            }

            const auto pattern_color = get_dc_brush_color(c, dst_dc);
            if (!pattern_color && !needs_source && rop3_uses_pattern(rop3))
            {
                return TRUE;
            }
            const uint32_t pattern = pattern_color.value_or(0xFFFFFFFFu);

            const RECT blt_area{.left = static_cast<LONG>(dx),
                                .top = static_cast<LONG>(dy),
                                .right = static_cast<LONG>(dx + w),
                                .bottom = static_cast<LONG>(dy + h)};
            visible.for_each_span(blt_area, [&](const LONG x_begin, const LONG x_end, const LONG y) {
                auto* dst_row = surface_row(*dst_surface, y);
                const auto row = static_cast<size_t>(y - dy);
                for (auto x = x_begin; x < x_end; ++x)
                {
                    const size_t i = row * blt_width + static_cast<size_t>(x - dx);
                    const uint32_t src = needs_source ? src_snapshot[i] : 0;

                    // Your renderer presents BGRA8 surfaces. Keeping alpha opaque matches the rest of this file's
                    // DIB-to-surface paths. Remove the OR if you later model real alpha-carrying 32-bpp DIB sections.
                    dst_row[x] = apply_rop3(rop3, src, dst_snapshot[i], pattern) | 0xFF000000u;
                }
            });

            present_win_surface(c, present_handle, dst_surface);

            return TRUE;
        }

        // Like BitBlt, but nearest-neighbour scales the source rectangle onto the destination rectangle.
        // Used by the user32 SS_BITMAP static control to paint its image (e.g. the open-iw5 splash).
        BOOL handle_NtGdiStretchBlt(const syscall_context& c, const hdc dst_dc, const int x_dst, const int y_dst, const int dst_width,
                                    const int dst_height, const hdc src_dc, const int x_src, const int y_src, const int src_width,
                                    const int src_height, const DWORD rop, const DWORD /*cr_back_color*/)
        {
            if (dst_dc == 0 || dst_width == 0 || dst_height == 0 || src_width == 0 || src_height == 0)
            {
                return FALSE;
            }

            (void)handle_NtGdiFlush(c);

            int32_t dst_origin_x = 0;
            int32_t dst_origin_y = 0;
            uint32_t present_handle = 0;
            gdi::clip_region visible{};
            gdi_bitmap_surface* dst_surface = resolve_dc_draw_target(c, dst_dc, dst_origin_x, dst_origin_y, present_handle, visible);
            if (!dst_surface || dst_surface->width == 0 || dst_surface->height == 0 || dst_surface->pixels.empty())
            {
                return FALSE;
            }

            const auto rop3 = static_cast<uint8_t>((rop >> 16) & 0xFFu);
            const bool needs_source = rop3_uses_source(rop3);

            int32_t src_origin_x = 0;
            int32_t src_origin_y = 0;
            gdi_bitmap_surface* src_surface = nullptr;
            if (needs_source)
            {
                if (src_dc == 0)
                {
                    return FALSE;
                }

                uint32_t unused_present_handle = 0;
                src_surface = resolve_dc_surface(c, src_dc, src_origin_x, src_origin_y, unused_present_handle);
                if (!src_surface || src_surface->width == 0 || src_surface->height == 0 || src_surface->pixels.empty())
                {
                    return FALSE;
                }
            }

            const int dst_w = std::abs(dst_width);
            const int dst_h = std::abs(dst_height);
            const int src_w = std::abs(src_width);
            const int src_h = std::abs(src_height);
            const bool flip_x = dst_width < 0;
            const bool flip_y = dst_height < 0;
            // Negative source extents mirror the source image (GDI semantics), independently of the
            // destination flip; sample the mirrored position within the (absolute) source rectangle.
            const bool flip_src_x = src_width < 0;
            const bool flip_src_y = src_height < 0;

            const auto pattern_color = get_dc_brush_color(c, dst_dc);
            if (!pattern_color && !needs_source && rop3_uses_pattern(rop3))
            {
                return TRUE;
            }
            const uint32_t pattern = pattern_color.value_or(0xFFFFFFFFu);

            const int64_t left = static_cast<int64_t>(x_dst) + dst_origin_x;
            for (int dy = 0; dy < dst_h; ++dy)
            {
                const int64_t out_y = static_cast<int64_t>(y_dst) + dst_origin_y + (flip_y ? (dst_h - 1 - dy) : dy);

                int sy = 0;
                if (needs_source)
                {
                    const int src_dy = static_cast<int>(static_cast<int64_t>(dy) * src_h / dst_h);
                    sy = y_src + src_origin_y + (flip_src_y ? (src_h - 1 - src_dy) : src_dy);
                    if (sy < 0 || sy >= static_cast<int>(src_surface->height))
                    {
                        continue;
                    }
                }

                visible.for_each_span(
                    clamped_to_surface(*dst_surface, left, out_y, left + dst_w, out_y + 1),
                    [&](const LONG x_begin, const LONG x_end, const LONG y) {
                        auto* dst_row = surface_row(*dst_surface, y);
                        for (auto out_x = x_begin; out_x < x_end; ++out_x)
                        {
                            const auto offset = static_cast<int>(out_x - left);
                            const int dx = flip_x ? dst_w - 1 - offset : offset;

                            uint32_t src = 0;
                            if (needs_source)
                            {
                                const int src_dx = static_cast<int>(static_cast<int64_t>(dx) * src_w / dst_w);
                                const int sx = x_src + src_origin_x + (flip_src_x ? (src_w - 1 - src_dx) : src_dx);
                                if (sx < 0 || sx >= static_cast<int>(src_surface->width))
                                {
                                    continue;
                                }
                                src = src_surface->pixels[static_cast<size_t>(sy) * src_surface->width + static_cast<size_t>(sx)];
                            }

                            dst_row[out_x] = apply_rop3(rop3, src, dst_row[out_x], pattern) | 0xFF000000u;
                        }
                    });
            }

            present_win_surface(c, present_handle, dst_surface);
            return TRUE;
        }

        BOOL handle_NtGdiTransparentBlt(const syscall_context& c, const hdc dst_dc, const int x_dst, const int y_dst, const int dst_width,
                                        const int dst_height, const hdc src_dc, const int x_src, const int y_src, const int src_width,
                                        const int src_height, const COLORREF /*transparent_color*/)
        {
            // TODO: This is only an approximation; honor the transparent color instead of using SRCCOPY.
            constexpr DWORD srccopy = 0x00CC0020u;
            return handle_NtGdiStretchBlt(c, dst_dc, x_dst, y_dst, dst_width, dst_height, src_dc, x_src, y_src, src_width, src_height,
                                          srccopy, 0);
        }

        // BLENDFUNCTION is passed by value: BlendOp, BlendFlags, SourceConstantAlpha, AlphaFormat from the low byte up.
        BOOL handle_NtGdiAlphaBlend(const syscall_context& c, const hdc dst_dc, const LONG x_dst, const LONG y_dst, const LONG dst_width,
                                    const LONG dst_height, const hdc src_dc, const LONG x_src, const LONG y_src, const LONG src_width,
                                    const LONG src_height, const DWORD blend_function, const handle /*color_transform*/)
        {
            constexpr uint8_t ac_src_over = 0x00;
            constexpr uint8_t ac_src_alpha = 0x01;

            if (dst_dc == 0 || src_dc == 0 || dst_width <= 0 || dst_height <= 0 || src_width <= 0 || src_height <= 0 ||
                (blend_function & 0xFFu) != ac_src_over)
            {
                return FALSE;
            }

            const auto constant_alpha = static_cast<uint32_t>((blend_function >> 16) & 0xFFu);
            const bool per_pixel_alpha = ((blend_function >> 24) & ac_src_alpha) != 0;

            (void)handle_NtGdiFlush(c);

            int32_t dst_origin_x = 0;
            int32_t dst_origin_y = 0;
            uint32_t present_handle = 0;
            gdi::clip_region visible{};
            gdi_bitmap_surface* dst_surface = resolve_dc_draw_target(c, dst_dc, dst_origin_x, dst_origin_y, present_handle, visible);
            if (!dst_surface || dst_surface->width == 0 || dst_surface->height == 0 || dst_surface->pixels.empty())
            {
                return FALSE;
            }

            int32_t src_origin_x = 0;
            int32_t src_origin_y = 0;
            uint32_t unused_present_handle = 0;
            const gdi_bitmap_surface* src_surface = resolve_dc_surface(c, src_dc, src_origin_x, src_origin_y, unused_present_handle);
            if (!src_surface || src_surface->width == 0 || src_surface->height == 0 || src_surface->pixels.empty())
            {
                return FALSE;
            }

            // Windows fails rather than clips when the source rectangle leaves the source surface, and when source and
            // destination overlap on the same surface.
            const auto src_left = static_cast<int64_t>(x_src) + src_origin_x;
            const auto src_top = static_cast<int64_t>(y_src) + src_origin_y;
            if (src_left < 0 || src_top < 0 || src_left + src_width > static_cast<int64_t>(src_surface->width) ||
                src_top + src_height > static_cast<int64_t>(src_surface->height))
            {
                return FALSE;
            }

            const auto dst_left = static_cast<int64_t>(x_dst) + dst_origin_x;
            const auto dst_top = static_cast<int64_t>(y_dst) + dst_origin_y;
            if (src_surface == dst_surface && dst_left < src_left + src_width && src_left < dst_left + dst_width &&
                dst_top < src_top + src_height && src_top < dst_top + dst_height)
            {
                return FALSE;
            }

            const auto blend_channel = [](const uint32_t src, const uint32_t dst, const int shift, const uint32_t src_weight,
                                          const uint32_t dst_weight) {
                const auto s = (src >> shift) & 0xFFu;
                const auto d = (dst >> shift) & 0xFFu;
                return std::min<uint32_t>(255, (s * src_weight + d * dst_weight + 127) / 255) << shift;
            };

            const auto dst_area = clamped_to_surface(*dst_surface, dst_left, dst_top, dst_left + dst_width, dst_top + dst_height);
            visible.for_each_span(dst_area, [&](const LONG x_begin, const LONG x_end, const LONG out_y) {
                const auto sy = src_top + ((out_y - dst_top) * src_height / dst_height);
                auto* dst_row = surface_row(*dst_surface, out_y);
                const auto* src_row = src_surface->pixels.data() + static_cast<size_t>(sy) * src_surface->width;

                for (auto out_x = x_begin; out_x < x_end; ++out_x)
                {
                    const uint32_t src = src_row[src_left + ((out_x - dst_left) * src_width / dst_width)];
                    const uint32_t dst = dst_row[out_x];

                    // With AC_SRC_ALPHA the source is premultiplied.
                    const auto src_alpha = per_pixel_alpha ? (src >> 24) & 0xFFu : 255u;
                    const auto dst_weight = 255 - ((src_alpha * constant_alpha + 127) / 255);

                    dst_row[out_x] =
                        blend_channel(src, dst, 0, constant_alpha, dst_weight) | blend_channel(src, dst, 8, constant_alpha, dst_weight) |
                        blend_channel(src, dst, 16, constant_alpha, dst_weight) | blend_channel(src, dst, 24, constant_alpha, dst_weight);
                }
            });

            present_win_surface(c, present_handle, dst_surface);
            return TRUE;
        }

        BOOL handle_NtGdiPatBlt(const syscall_context& c, const hdc dc, const LONG x, const LONG y, const LONG width, const LONG height,
                                const DWORD rop)
        {
            gdi_dc_state* dc_state = nullptr;
            gdi_bitmap_surface* surface = nullptr;
            int32_t origin_x = 0;
            int32_t origin_y = 0;
            gdi::clip_region visible{};
            if (!get_dc_state_and_draw_target(c, dc, dc_state, surface, origin_x, origin_y, visible) || !dc_state || !surface)
            {
                return FALSE;
            }

            const auto rop3 = static_cast<uint8_t>((rop >> 16) & 0xFFu);
            const auto color = get_dc_brush_color(c, dc);
            if (!color && rop3_uses_pattern(rop3))
            {
                return TRUE;
            }

            const auto pattern = color.value_or(0);
            const int64_t left = static_cast<int64_t>(x) + origin_x;
            const int64_t top = static_cast<int64_t>(y) + origin_y;
            visible.for_each_span(clamped_to_surface(*surface, left, top, left + width, top + height),
                                  [&](const LONG x_begin, const LONG x_end, const LONG row_y) {
                                      auto* row = surface_row(*surface, row_y);
                                      for (auto px = x_begin; px < x_end; ++px)
                                      {
                                          row[px] = apply_rop3(rop3, 0, row[px], pattern) | 0xFF000000u;
                                      }
                                  });
            return TRUE;
        }

        BOOL handle_NtGdiPolyPatBlt(const syscall_context& c, const hdc dc, const DWORD /*rop*/, const emulator_pointer poly,
                                    const DWORD count, const DWORD /*mode*/)
        {
            gdi_dc_state* dc_state = nullptr;
            gdi_bitmap_surface* surface = nullptr;
            int32_t origin_x = 0;
            int32_t origin_y = 0;
            gdi::clip_region visible{};
            if (!get_dc_state_and_draw_target(c, dc, dc_state, surface, origin_x, origin_y, visible) || !surface)
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

                const auto color = brush != 0 ? get_brush_color_in_dc(c, dc, static_cast<uint32_t>(brush)) : get_dc_brush_color(c, dc);
                if (color)
                {
                    fill_rect(*surface, visible, rect[0] + origin_x, rect[1] + origin_y, rect[0] + rect[2] + origin_x,
                              rect[1] + rect[3] + origin_y, *color);
                }
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
            gdi::clip_region visible{};
            if (!get_dc_state_and_draw_target(c, dc, dc_state, surface, origin_x, origin_y, visible) || !dc_state || !surface)
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
                    fill_rect(*surface, visible, clip_rect.left, clip_rect.top, clip_rect.right, clip_rect.bottom,
                              get_dc_background_color(c, dc));
                }
            }

            std::vector<uint32_t> advances{};
            if (dx != 0)
            {
                advances.resize(count);
                c.emu.read_memory(dx, advances.data(), advances.size() * sizeof(uint32_t));
            }

            uint64_t dc_attr = 0;
            uint32_t text_align = 0;
            if (get_gdi_object_address(c, static_cast<uint32_t>(dc), k_gdi_dc_type, dc_attr))
            {
                c.emu.read_memory(dc_attr + k_gdi_dc_attr_text_align_offset, &text_align, sizeof(text_align));
            }

            const auto color = get_dc_text_color(c, dc);
            const auto* dx_values = advances.empty() ? nullptr : advances.data();
            const auto top_left = text_top_left(x + origin_x, y + origin_y, text_align, glyphs.size(), dx_values);
            draw_text(*surface, visible, top_left.x, top_left.y, glyphs, color,
                      has_rect && (options & ETO_CLIPPED) != 0 ? &clip_rect : nullptr, dx_values);

            dc_state->current_x = x + static_cast<int32_t>(count * k_text_cell_width);
            dc_state->current_y = y;
            return TRUE;
        }

        // The caller sets the size: 16 bytes for GetFontRealizationInfo, 24 when it also wants the face, and 812 for the
        // variant that lists the font files. The emulated fonts are raster fonts without font files, so everything after
        // the common header stays zero.
        BOOL handle_NtGdiGetRealizationInfo(const syscall_context& c, const hdc dc, const emulator_pointer realization_info)
        {
            struct font_realization_info_header
            {
                uint32_t size;
                uint32_t flags;
                uint32_t cache_num;
                uint32_t instance_id;
            };

            constexpr std::array<uint32_t, 3> supported_sizes{sizeof(font_realization_info_header), 24, 812};
            constexpr uint32_t realized = 1;

            uint64_t dc_attr = 0;
            if (dc == 0 || realization_info == 0 || !get_gdi_object_address(c, static_cast<uint32_t>(dc), k_gdi_dc_type, dc_attr))
            {
                return FALSE;
            }

            uint32_t size = 0;
            c.emu.read_memory(realization_info, &size, sizeof(size));
            if (std::ranges::find(supported_sizes, size) == supported_sizes.end())
            {
                return FALSE;
            }

            uint32_t font_handle = 0;
            c.emu.read_memory(dc_attr + k_gdi_dc_attr_font_offset, &font_handle, sizeof(font_handle));

            std::vector<uint8_t> info(size);
            const font_realization_info_header header{
                .size = size,
                .flags = realized,
                .cache_num = font_handle,
                .instance_id = font_handle,
            };
            std::memcpy(info.data(), &header, sizeof(header));

            c.emu.write_memory(realization_info, info.data(), info.size());
            return TRUE;
        }

        BOOL handle_NtGdiFontIsLinked(const syscall_context&, const hdc /*dc*/)
        {
            return FALSE;
        }

        // The emulated fonts are raster fonts without TrueType tables, which GetFontData reports with GDI_ERROR.
        uint32_t handle_NtGdiGetFontData(const syscall_context&, const hdc /*dc*/, const uint32_t /*table*/, const uint32_t /*offset*/,
                                         const emulator_pointer /*buffer*/, const uint32_t /*size*/)
        {
            constexpr uint32_t gdi_error = 0xFFFFFFFF;
            return gdi_error;
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
                entry32.OwnerValue = entry.Owner;
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

        int32_t handle_NtGdiSetIcmMode()
        {
            return 0;
        }

        NTSTATUS handle_NtGdiSetLayout()
        {
            return STATUS_SUCCESS;
        }

        std::optional<uint32_t> get_dc_attr_object_offset(const int32_t object_type)
        {
            switch (object_type)
            {
            case k_gdi_dc_object_font:
                return k_gdi_dc_attr_font_offset;
            case k_gdi_dc_object_brush:
                return k_gdi_dc_attr_hbrush_offset;
            case k_gdi_dc_object_pen:
                return k_gdi_dc_attr_hpen_offset;
            default:
                return std::nullopt;
            }
        }

        uint64_t handle_NtGdiGetDCObject(const syscall_context& c, const hdc dc, const int32_t object_type)
        {
            if (const auto offset = get_dc_attr_object_offset(object_type))
            {
                uint64_t dc_attr = 0;
                if (!get_gdi_object_address(c, static_cast<uint32_t>(dc), k_gdi_dc_type, dc_attr))
                {
                    return 0;
                }

                uint32_t object = 0;
                c.win_emu.memory.try_read_memory(dc_attr + *offset, &object, sizeof(object));
                return object;
            }

            if (object_type != k_gdi_dc_object_bitmap)
            {
                return 0;
            }

            const auto dc_it = c.proc.gdi_dc_states.find(static_cast<uint32_t>(dc));
            if (dc_it == c.proc.gdi_dc_states.end())
            {
                return 0;
            }

            const auto bitmap = dc_it->second.selected_bitmap;
            return c.proc.gdi_bitmap_surfaces.contains(bitmap) ? bitmap : 0;
        }

        BOOL handle_NtGdiUnrealizeObject(const syscall_context& c, const handle h)
        {
            GDI_HANDLE_ENTRY64 entry{};
            uint64_t entry_addr = 0;
            return read_gdi_entry_for_handle(c, static_cast<uint32_t>(h.bits), entry, entry_addr) ? TRUE : FALSE;
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
                GUID adapter_guid = k_dxgk_adapter_unique_id;
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

                ids.VendorID = emulated_display::vendor_id;
                ids.DeviceID = emulated_display::device_id;
                ids.SubSystemID = 0;
                ids.RevisionID = emulated_display::revision_id;
                ids.BusNumber = 0;
                ids.DeviceNumber = 0;
                ids.FunctionNumber = 0;

                return write_query_adapter_info(c, query, ids);
            }

            case KMTQAITYPE::KMTQAITYPE_ADAPTERTYPE_RENDER: {
                return write_query_adapter_info(c, query, 1);
            }

            case KMTQAITYPE::KMTQAITYPE_QUERY_ADAPTER_UNIQUE_GUID: {
                GUID unique_guid = k_dxgk_adapter_unique_id;
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

        void complete_warp_sync_command(const syscall_context& c, const uint64_t command_ptr, const UINT32 command_length)
        {
            constexpr uint32_t k_sync_magic = 0x434E5953;
            constexpr uint32_t k_ack_magic = 0x4B415953;
            constexpr uint32_t k_ack_length = 8;
            if (command_ptr == 0 || command_length < 24)
            {
                return;
            }

            uint32_t magic{};
            if (!c.emu.try_read_memory(command_ptr, &magic, sizeof(magic)) || magic != k_sync_magic)
            {
                return;
            }

            // SYNC carries a UM completion event at +8 and an error event at +16. After the KM
            // consumes the DMA buffer it overwrites the header with a sync-ack (magic + length 8).
            // Leaving SYNC in place is treated as E_OUTOFMEMORY and the D3D11 device is removed.
            // Signal only the completion event; the error event is DXGI_ERROR_DEVICE_REMOVED.
            uint64_t completion{};
            if (!c.emu.try_read_memory(command_ptr + 8, &completion, sizeof(completion)))
            {
                return;
            }

            uint32_t ack_magic = k_ack_magic;
            uint32_t ack_length = k_ack_length;
            if (!c.emu.try_write_memory(command_ptr, &ack_magic, sizeof(ack_magic)))
            {
                return;
            }

            c.emu.try_write_memory(command_ptr + 4, &ack_length, sizeof(ack_length));
            if (auto* entry = c.proc.events.get(completion))
            {
                entry->signaled = true;
            }
        }

        void reserve_dxgk_submission_buffers(const syscall_context& c, const uint32_t command_buffer_size,
                                             const uint32_t allocation_list_count, const uint32_t patch_location_list_count)
        {
            auto& dxgk = c.proc.dxgk;
            auto& memory = c.win_emu.memory;

            using dxgk_state = process_context::dxgk_state;

            dxgk_state::reserve_buffer(memory, dxgk.command_buffer, command_buffer_size);
            dxgk_state::reserve_buffer(memory, dxgk.allocation_list, allocation_list_count * k_dxgk_allocation_list_entry_size);
            dxgk_state::reserve_buffer(memory, dxgk.patch_location_list, patch_location_list_count * k_dxgk_patch_location_list_entry_size);
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

                reserve_dxgk_submission_buffers(c, k_dxgk_command_buffer_size, k_dxgk_allocation_list_count,
                                                k_dxgk_patch_location_list_count);

                const auto& dxgk = c.proc.dxgk;

                create_context.pCommandBuffer = dxgk.command_buffer.address;
                create_context.CommandBuffer = dxgk.command_buffer.address;
                create_context.CommandBufferSize = dxgk.command_buffer.size;
                create_context.pAllocationList = dxgk.allocation_list.address;
                create_context.AllocationListSize = dxgk.allocation_list.size / k_dxgk_allocation_list_entry_size;
                create_context.pPatchLocationList = dxgk.patch_location_list.address;
                create_context.PatchLocationListSize = dxgk.patch_location_list.size / k_dxgk_patch_location_list_entry_size;

                dxgk_info(c, "NtGdiDdDDICreateContext: Created Context 0x%X on Device 0x%X (CmdBuf: 0x%llX)", create_context.hContext,
                          create_context.hDevice, dxgk.command_buffer.address);
            });

            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtGdiDdDDIRender(const syscall_context& c, const emulator_object<EMU_D3DKMT_RENDER> render_desc)
        {
            if (!render_desc)
            {
                return STATUS_INVALID_PARAMETER;
            }

            render_desc.access([&](EMU_D3DKMT_RENDER& render) {
                if (render.hContext != k_dxgk_context_handle && render.hContext != k_dxgk_device_handle)
                {
                    dxgk_warn(c, "NtGdiDdDDIRender: Unknown context 0x%X", render.hContext);
                }

                const auto& command_buffer = c.proc.dxgk.command_buffer;
                const auto command_offset = static_cast<uint64_t>(render.CommandOffset);
                const auto command_length = static_cast<uint64_t>(render.CommandLength);
                if (command_buffer.address != 0 && command_offset <= command_buffer.size &&
                    command_length <= static_cast<uint64_t>(command_buffer.size) - command_offset)
                {
                    complete_warp_sync_command(c, command_buffer.address + command_offset, render.CommandLength);
                }

                // Clamp the guest-controlled sizes: at least the defaults, but never above the caps above.
                reserve_dxgk_submission_buffers(
                    c, std::clamp(render.NewCommandBufferSize, k_dxgk_command_buffer_size, k_dxgk_max_command_buffer_size),
                    std::clamp(render.NewAllocationListSize, k_dxgk_allocation_list_count, k_dxgk_max_list_count),
                    std::clamp(render.NewPatchLocationListSize, k_dxgk_patch_location_list_count, k_dxgk_max_list_count));

                const auto& dxgk = c.proc.dxgk;

                render.pNewCommandBuffer = dxgk.command_buffer.address;
                render.NewCommandBuffer = dxgk.command_buffer.address;
                render.NewCommandBufferSize = dxgk.command_buffer.size;
                render.pNewAllocationList = dxgk.allocation_list.address;
                render.NewAllocationListSize = dxgk.allocation_list.size / k_dxgk_allocation_list_entry_size;
                render.pNewPatchLocationList = dxgk.patch_location_list.address;
                render.NewPatchLocationListSize = dxgk.patch_location_list.size / k_dxgk_patch_location_list_entry_size;
                render.QueuedBufferCount = 0;

                dxgk_info(c, "NtGdiDdDDIRender: Context 0x%X CommandOffset=0x%X CommandLength=0x%X Allocations=%u Patches=%u",
                          render.hContext, render.CommandOffset, render.CommandLength, render.AllocationCount, render.PatchLocationCount);
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
            write_private(descriptor_offset + 0x04, c.proc.display_width);
            write_private(descriptor_offset + 0x08, c.proc.display_height);
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

                const auto backing_size = static_cast<uint64_t>(c.proc.display_width) * static_cast<uint64_t>(c.proc.display_height) * 4ull;
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
                mode.access([&](EMU_D3DKMT_DISPLAYMODE& current_mode) {
                    current_mode.Width = c.proc.display_width;
                    current_mode.Height = c.proc.display_height;
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

        NTSTATUS handle_NtGdiDdDDINetDispQueryMiracastDisplayDeviceSupport(
            const syscall_context&, const emulator_object<EMU_D3DKMT_MIRACAST_DISPLAY_DEVICE_CAPS> display_caps)
        {
            if (!display_caps)
            {
                return STATUS_INVALID_PARAMETER;
            }

            display_caps.write({});
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

        NTSTATUS handle_NtGdiDdDDICreateDCFromMemory(const syscall_context& c,
                                                     const emulator_object<EMU_D3DKMT_CREATEDCFROMMEMORY> create_dc)
        {
            if (!create_dc)
            {
                return STATUS_INVALID_PARAMETER;
            }

            const auto params = create_dc.read();
            if (params.Width == 0 || params.Height == 0 || params.Pitch == 0 || params.pMemory == 0)
            {
                return STATUS_INVALID_PARAMETER;
            }

            if (params.hDeviceDc != 0 && !c.proc.gdi_dc_states.contains(static_cast<uint32_t>(params.hDeviceDc)))
            {
                return STATUS_INVALID_PARAMETER;
            }

            gdi_bitmap_surface* surface = nullptr;
            const auto bitmap_handle = create_gdi_bitmap_surface(c, params.Width, params.Height, 0, &surface);
            if (bitmap_handle == 0)
            {
                return STATUS_NO_MEMORY;
            }

            const auto initialize_surface = [&] {
                constexpr uint32_t k_d3dddifmt_a8r8g8b8 = 21;
                constexpr uint32_t k_d3dddifmt_x8r8g8b8 = 22;
                constexpr uint32_t k_d3dddifmt_a8b8g8r8 = 32;
                constexpr uint32_t k_d3dddifmt_x8b8g8r8 = 33;
                constexpr size_t pixel_size = sizeof(uint32_t);

                const auto width = static_cast<size_t>(params.Width);
                const auto height = static_cast<size_t>(params.Height);
                const auto pitch = static_cast<size_t>(params.Pitch);
                const auto row_bytes = width * pixel_size;
                if (pitch < row_bytes)
                {
                    return false;
                }

                std::vector<uint32_t> row(width);
                for (size_t y = 0; y < height; ++y)
                {
                    c.emu.read_memory(params.pMemory + (y * pitch), row.data(), row_bytes);

                    auto* dst = surface->pixels.data() + (y * width);
                    switch (params.Format)
                    {
                    case k_d3dddifmt_a8r8g8b8:
                        std::memcpy(dst, row.data(), row_bytes);
                        break;

                    case k_d3dddifmt_x8r8g8b8:
                        for (size_t x = 0; x < width; ++x)
                        {
                            dst[x] = row[x] | 0xFF000000u;
                        }
                        break;

                    case k_d3dddifmt_a8b8g8r8:
                    case k_d3dddifmt_x8b8g8r8:
                        for (size_t x = 0; x < width; ++x)
                        {
                            const auto src = row[x];
                            const auto alpha = params.Format == k_d3dddifmt_x8b8g8r8 ? 0xFF000000u : (src & 0xFF000000u);
                            dst[x] = alpha | ((src & 0x000000FFu) << 16) | (src & 0x0000FF00u) | ((src & 0x00FF0000u) >> 16);
                        }
                        break;

                    default:
                        return false;
                    }
                }

                return true;
            };

            if (!initialize_surface())
            {
                handle_NtGdiDeleteObjectApp(c, bitmap_handle);
                return STATUS_NOT_SUPPORTED;
            }

            uint64_t dc_attr = 0;
            const auto dc_handle = allocate_gdi_dc(c, dc_attr);
            if (dc_handle == 0)
            {
                handle_NtGdiDeleteObjectApp(c, bitmap_handle);
                return STATUS_NO_MEMORY;
            }

            auto dc_it = c.proc.gdi_dc_states.find(dc_handle);
            if (dc_it == c.proc.gdi_dc_states.end())
            {
                handle_NtGdiDeleteObjectApp(c, dc_handle);
                handle_NtGdiDeleteObjectApp(c, bitmap_handle);
                return STATUS_NO_MEMORY;
            }

            dc_it->second.is_memory_dc = true;
            dc_it->second.selected_bitmap = bitmap_handle;

            create_dc.access([&](EMU_D3DKMT_CREATEDCFROMMEMORY& writable) {
                writable.hDc = dc_handle;
                writable.hBitmap = bitmap_handle;
            });

            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtGdiDdDDIDestroyDCFromMemory(const syscall_context& c,
                                                      const emulator_object<EMU_D3DKMT_DESTROYDCFROMMEMORY> destroy_dc)
        {
            if (!destroy_dc)
            {
                return STATUS_INVALID_PARAMETER;
            }

            const auto params = destroy_dc.read();
            const auto dc_handle = static_cast<uint32_t>(params.hDc);
            const auto bitmap_handle = static_cast<uint32_t>(params.hBitmap);
            if (dc_handle == 0 || bitmap_handle == 0)
            {
                return STATUS_INVALID_PARAMETER;
            }

            const auto dc_it = c.proc.gdi_dc_states.find(dc_handle);
            if (dc_it == c.proc.gdi_dc_states.end() || !dc_it->second.is_memory_dc)
            {
                return STATUS_INVALID_PARAMETER;
            }

            if (!c.proc.gdi_bitmap_surfaces.contains(bitmap_handle))
            {
                return STATUS_INVALID_PARAMETER;
            }

            if (handle_NtGdiDeleteObjectApp(c, dc_handle) == 0)
            {
                return STATUS_INVALID_PARAMETER;
            }

            if (handle_NtGdiDeleteObjectApp(c, bitmap_handle) == 0)
            {
                return STATUS_INVALID_PARAMETER;
            }

            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtGdiDdDDIOpenAdapterFromLuid(const syscall_context&,
                                                      const emulator_object<EMU_D3DKMT_OPENADAPTERFROMLUID> open_adapter)
        {
            if (!open_adapter)
            {
                return STATUS_INVALID_PARAMETER;
            }

            open_adapter.access([](EMU_D3DKMT_OPENADAPTERFROMLUID& params) { //
                params.hAdapter = k_dxgk_adapter_handle;
            });

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

        NTSTATUS handle_NtGdiDdDDIWaitForSynchronizationObjectFromGpu()
        {
            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtGdiDdDDIWaitForSynchronizationObjectFromCpu(
            const syscall_context& c, const emulator_object<EMU_D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU> wait_desc)
        {
            if (!wait_desc)
            {
                return STATUS_INVALID_PARAMETER;
            }

            const auto wait = wait_desc.read();
            if (wait.hAsyncEvent == 0)
            {
                return STATUS_SUCCESS;
            }

            auto* entry = c.proc.events.get(wait.hAsyncEvent);
            if (!entry)
            {
                return STATUS_INVALID_HANDLE;
            }

            entry->signaled = true;
            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtGdiDdDDISignalSynchronizationObjectFromGpu()
        {
            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtGdiDdDDISignalSynchronizationObjectFromCpu()
        {
            return STATUS_SUCCESS;
        }

        COLORREF handle_NtGdiSetPixel(const syscall_context& c, const hdc dc, const int x, const int y, const COLORREF color)
        {
            constexpr uint64_t clr_invalid = 0xFFFFFFFF;

            gdi_dc_state* dc_state = nullptr;
            gdi_bitmap_surface* surface = nullptr;
            int32_t origin_x = 0;
            int32_t origin_y = 0;
            uint32_t present_handle = 0;
            gdi::clip_region visible{};
            if (!get_dc_state_and_draw_target(c, dc, dc_state, surface, origin_x, origin_y, visible, &present_handle) || !surface)
            {
                return clr_invalid;
            }

            const auto surface_x = x + origin_x;
            const auto surface_y = y + origin_y;
            if (!get_surface_pixel(*surface, surface_x, surface_y).has_value())
            {
                return clr_invalid;
            }

            if (!visible.contains(surface_x, surface_y))
            {
                return clr_invalid;
            }

            const auto bgra = colorref_to_bgra(color);
            set_surface_pixel(*surface, surface_x, surface_y, bgra);
            present_win_surface(c, present_handle, surface);

            return bgra_to_colorref(bgra);
        }

        COLORREF handle_NtGdiGetPixel(const syscall_context& c, const hdc dc, const int x, const int y)
        {
            constexpr uint64_t clr_invalid = 0xFFFFFFFF;

            int32_t origin_x = 0;
            int32_t origin_y = 0;
            uint32_t present_handle = 0;
            const auto* surface = resolve_dc_surface(c, dc, origin_x, origin_y, present_handle);
            if (!surface)
            {
                return clr_invalid;
            }

            const auto pixel = get_surface_pixel(*surface, x + origin_x, y + origin_y);
            if (!pixel.has_value())
            {
                return clr_invalid;
            }

            return bgra_to_colorref(*pixel);
        }

        uint32_t handle_NtGdiGetPublicFontTableChangeCookie()
        {
            return 1;
        }

        COLORREF handle_NtGdiGetNearestColor(const syscall_context&, const hdc, const COLORREF color)
        {
            return static_cast<COLORREF>(color & 0x00FFFFFFu);
        }

        uint32_t handle_NtGdiSetBoundsRect(const syscall_context&, const hdc, const emulator_pointer, const uint32_t)
        {
            return 0;
        }

        BOOL handle_NtGdiGetAndSetDCDword(const syscall_context& c, const hdc dc, const uint32_t method, const uint32_t value,
                                          const emulator_pointer result)
        {
            constexpr uint32_t set_map_mode = 8;
            constexpr uint32_t text_map_mode = 1;

            if (dc == 0 || result == 0 || method != set_map_mode || value != text_map_mode)
            {
                return FALSE;
            }

            c.emu.write_memory(result, &text_map_mode, sizeof(text_map_mode));
            return TRUE;
        }

        uint32_t handle_NtGdiGetGlyphIndicesW(const syscall_context& c, const hdc dc, const emulator_pointer text, const int32_t char_count,
                                              const emulator_pointer glyph_indices, const uint32_t)
        {
            constexpr uint32_t gdi_error = 0xFFFFFFFF;
            constexpr size_t max_glyph_count = 1 << 16;
            if (dc == 0 || text == 0 || glyph_indices == 0 || char_count < 0 || static_cast<size_t>(char_count) > max_glyph_count)
            {
                return gdi_error;
            }

            std::array<char16_t, 256> characters{};
            const auto count = static_cast<size_t>(char_count);
            for (size_t offset = 0; offset < count; offset += characters.size())
            {
                const auto chunk_count = std::min(characters.size(), count - offset);
                const auto byte_count = chunk_count * sizeof(char16_t);
                if (!c.win_emu.memory.try_read_memory(text + offset * sizeof(char16_t), characters.data(), byte_count) ||
                    !c.win_emu.memory.try_write_memory(glyph_indices + offset * sizeof(char16_t), characters.data(), byte_count))
                {
                    return gdi_error;
                }
            }

            return static_cast<uint32_t>(count);
        }

        BOOL handle_NtGdiPolyPolyDraw()
        {
            return TRUE;
        }
    }

} // namespace sogen
