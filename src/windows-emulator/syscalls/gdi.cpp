#include "../std_include.hpp"
#include "../emulator_utils.hpp"
#include "../syscall_utils.hpp"

#include <array>
#include <bit>
#include <limits>

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
        constexpr uint8_t k_gdi_bitmap_type = 0x05;
        constexpr uint8_t k_gdi_font_type = 0x0A;
        constexpr uint8_t k_gdi_brush_type = 0x10;
        constexpr uint8_t k_gdi_pen_type = 0x30;

        constexpr uint32_t k_gdi_dc_attr_size = 0x130;
        constexpr uint32_t k_gdi_brush_attr_size = 0x20;
        constexpr uint32_t k_gdi_pen_attr_size = 0x20;
        constexpr uint32_t k_gdi_bitmap_attr_size = 0x20;
        constexpr uint32_t k_gdi_font_attr_size = 0x40;

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

        constexpr uint32_t k_default_dpi = 96;
        constexpr uint32_t k_default_width = 1920;
        constexpr uint32_t k_default_height = 1080;

        constexpr uint32_t k_gdi_first_dynamic_handle = 0x2000;
        constexpr uint32_t k_gdi_default_cookie = STATUS_WAIT_1;

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

            if (client_drawing_brush == 0)
            {
                const uint32_t handle = allocate_gdi_object(c, k_gdi_brush_type, k_gdi_brush_attr_size);
                if (handle != 0)
                {
                    client_drawing_brush = handle;
                }
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
            return allocate_gdi_handle(c, k_gdi_dc_type, dc_attr, dc_attr);
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

    uint64_t handle_NtGdiCreateSolidBrush(const syscall_context& c, const uint32_t /*color*/, const uint64_t /*unused*/)
    {
        return allocate_gdi_object(c, k_gdi_brush_type, k_gdi_brush_attr_size);
    }

    uint64_t handle_NtGdiCreatePatternBrushInternal(const syscall_context& c, const handle /*bitmap*/, const uint32_t /*unused*/)
    {
        return allocate_gdi_object(c, k_gdi_brush_type, k_gdi_brush_attr_size);
    }

    uint64_t handle_NtGdiCreatePen(const syscall_context& c, const uint32_t /*style*/, const uint32_t /*width*/, const uint32_t /*color*/)
    {
        return allocate_gdi_object(c, k_gdi_pen_type, k_gdi_pen_attr_size);
    }

    uint64_t handle_NtGdiCreateCompatibleDC(const syscall_context& c, const hdc /*dc*/)
    {
        uint64_t dc_attr = 0;
        return allocate_gdi_dc(c, dc_attr);
    }

    uint64_t handle_NtGdiCreateCompatibleBitmap(const syscall_context& c, const hdc /*dc*/, const uint32_t /*width*/,
                                                const uint32_t /*height*/)
    {
        return allocate_gdi_object(c, k_gdi_bitmap_type, k_gdi_bitmap_attr_size);
    }

    uint64_t handle_NtGdiCreateDIBitmapInternal(const syscall_context& c, const hdc /*dc*/, const uint32_t /*width*/,
                                                const uint32_t /*height*/, const uint32_t /*usage*/, const emulator_pointer /*bits*/,
                                                const emulator_pointer /*info*/, const uint32_t /*info_header_size*/,
                                                const uint32_t /*init*/, const uint32_t /*offset*/, const uint32_t /*cj*/,
                                                const uint32_t /*i_usage*/)
    {
        return allocate_gdi_object(c, k_gdi_bitmap_type, k_gdi_bitmap_attr_size);
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

        return 1;
    }

    uint64_t handle_NtGdiSelectBitmap(const syscall_context&, const hdc /*dc*/, const handle bitmap)
    {
        return bitmap.bits;
    }

    hdc handle_NtGdiGetDCforBitmap(const syscall_context& c, const handle /*bitmap*/)
    {
        return ensure_default_hdc(c);
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
