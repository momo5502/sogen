#pragma once

#include "emulator_utils.hpp"
#include "handles.hpp"
#include "registry/registry_manager.hpp"

#include "module/module_manager.hpp"
#include <utils/nt_handle.hpp>

#include <arch_emulator.hpp>

#include "io_device.hpp"
#include "kusd_mmio.hpp"
#include "windows_objects.hpp"
#include "emulator_thread.hpp"
#include "port.hpp"
#include "user_handle_table.hpp"

#include "apiset/apiset.hpp"

namespace sogen
{

    struct fake_environment_config;

#define PEB_SEGMENT_SIZE (20 << 20) // 20 MB
#define GS_SEGMENT_SIZE  (1 << 20)  // 1 MB

#define STACK_SIZE       0x40000ULL // 256KB

#define GDT_ADDR         0x35000
#define GDT_LIMIT        0x1000
#define GDT_ENTRY_SIZE   0x8

    // Each vCPU gets its own GDT page. Most descriptors are identical, but the WOW64 FS descriptor
    // (selector 0x53) holds a per-thread 32-bit TEB base that the guest reloads on every 64<->32
    // transition, so a shared GDT would let a WOW64 thread on one vCPU read another vCPU's TEB base.
    constexpr uint64_t gdt_base_for_vcpu(const size_t vcpu_index) noexcept
    {
        return GDT_ADDR + vcpu_index * GDT_LIMIT;
    }

// TODO: Get rid of that
#define WOW64_NATIVE_STACK_SIZE 0x40000ULL
#define WOW64_32BIT_STACK_SIZE  (1 << 20)

    struct emulator_settings;
    struct application_settings;
    class windows_version_manager;

    using knowndlls_map = std::map<std::u16string, section>;
    struct file_lock_range
    {
        uint64_t offset{};
        uint64_t length{};
        ULONG key{};
        bool exclusive{};
        handle owner{};

        void serialize(utils::buffer_serializer& buffer) const
        {
            buffer.write(this->offset);
            buffer.write(this->length);
            buffer.write(this->key);
            buffer.write(this->exclusive);
            buffer.write(this->owner);
        }

        void deserialize(utils::buffer_deserializer& buffer)
        {
            buffer.read(this->offset);
            buffer.read(this->length);
            buffer.read(this->key);
            buffer.read(this->exclusive);
            buffer.read(this->owner);
        }
    };

    struct file_lock_ranges
    {
        std::vector<file_lock_range> locks{};

        void serialize(utils::buffer_serializer& buffer) const
        {
            buffer.write_vector(this->locks);
        }

        void deserialize(utils::buffer_deserializer& buffer)
        {
            buffer.read_vector(this->locks);
        }
    };

    struct gdi_bitmap_surface
    {
        uint32_t width{};
        uint32_t height{};
        std::vector<uint32_t> pixels{};

        emulator_pointer guest_bits{};
        uint32_t guest_stride{};
        uint32_t guest_bpp{32};
        bool guest_top_down{};
        bool guest_owns_memory{};

        void serialize(utils::buffer_serializer& buffer) const
        {
            buffer.write(this->width);
            buffer.write(this->height);
            buffer.write_vector(this->pixels);
            buffer.write(this->guest_bits);
            buffer.write(this->guest_stride);
            buffer.write(this->guest_bpp);
            buffer.write(this->guest_top_down);
            buffer.write(this->guest_owns_memory);
        }

        void deserialize(utils::buffer_deserializer& buffer)
        {
            buffer.read(this->width);
            buffer.read(this->height);
            buffer.read_vector(this->pixels);
            buffer.read(this->guest_bits);
            buffer.read(this->guest_stride);
            buffer.read(this->guest_bpp);
            buffer.read(this->guest_top_down);
            buffer.read(this->guest_owns_memory);
        }
    };

    struct gdi_dc_state
    {
        uint32_t selected_bitmap{};
        hwnd target_window{};
        int32_t current_x{};
        int32_t current_y{};
        bool is_memory_dc{};

        void serialize(utils::buffer_serializer& buffer) const
        {
            buffer.write(this->selected_bitmap);
            buffer.write(this->target_window);
            buffer.write(this->current_x);
            buffer.write(this->current_y);
            buffer.write(this->is_memory_dc);
        }

        void deserialize(utils::buffer_deserializer& buffer)
        {
            buffer.read(this->selected_bitmap);
            buffer.read(this->target_window);
            buffer.read(this->current_x);
            buffer.read(this->current_y);
            buffer.read(this->is_memory_dc);
        }
    };

    struct process_context
    {
        struct callbacks
        {
            utils::optional_function<void(handle h, emulator_thread& thr)> on_thread_create{};
            utils::optional_function<void(handle h, emulator_thread& thr)> on_thread_terminated{};
            utils::optional_function<void(emulator_thread& current_thread, emulator_thread& new_thread)> on_thread_switch{};
            utils::optional_function<void(emulator_thread& current_thread)> on_thread_set_name{};
        };

        struct atom_entry
        {
            std::u16string name{};
            uint32_t ref_count = 0;

            void serialize(utils::buffer_serializer& buffer) const
            {
                buffer.write(this->name);
                buffer.write(this->ref_count);
            }

            void deserialize(utils::buffer_deserializer& buffer)
            {
                buffer.read(this->name);
                buffer.read(this->ref_count);
            }
        };

        struct class_entry
        {
            emulator_pointer guest_obj_addr{};
            EMU_WNDCLASSEX wnd_class{};
            CLSMENUNAME<EmulatorTraits<Emu64>> menu_name{};

            class_entry() = default;

            class_entry(const emulator_pointer guest_obj, const EMU_WNDCLASSEX& wnd_class,
                        const CLSMENUNAME<EmulatorTraits<Emu64>>& menu_name)
                : guest_obj_addr(guest_obj),
                  wnd_class(wnd_class),
                  menu_name(menu_name)
            {
            }
        };

        struct dxgk_state
        {
            struct dxgk_allocation
            {
                uint32_t resource_handle{};
                uint64_t backing_memory{};
                uint64_t backing_size{};

                void serialize(utils::buffer_serializer& buffer) const
                {
                    buffer.write(this->resource_handle);
                    buffer.write(this->backing_memory);
                    buffer.write(this->backing_size);
                }

                void deserialize(utils::buffer_deserializer& buffer)
                {
                    buffer.read(this->resource_handle);
                    buffer.read(this->backing_memory);
                    buffer.read(this->backing_size);
                }
            };

            uint32_t next_resource_handle{0x8000};
            uint32_t next_allocation_handle{0x9000};
            std::map<uint32_t, dxgk_allocation> allocations{};

            uint32_t create_resource()
            {
                return ++this->next_resource_handle;
            }

            uint32_t create_allocation(memory_manager& memory, const uint32_t resource_handle, uint64_t backing_size)
            {
                if (backing_size == 0)
                {
                    backing_size = 0x1000; // fallback size
                }

                const auto aligned_size = static_cast<uint64_t>(page_align_up(backing_size));
                const auto backing_memory = memory.allocate_memory(static_cast<size_t>(aligned_size), memory_permission::read_write);

                const uint32_t handle = ++this->next_allocation_handle;

                this->allocations[handle] = {
                    .resource_handle = resource_handle,
                    .backing_memory = backing_memory,
                    .backing_size = aligned_size,
                };

                return handle;
            }

            const dxgk_allocation* get_allocation(const uint32_t handle) const
            {
                const auto it = this->allocations.find(handle);
                if (it == this->allocations.end())
                {
                    return nullptr;
                }

                return &it->second;
            }

            bool destroy_allocation(memory_manager& memory, const uint32_t handle)
            {
                const auto it = this->allocations.find(handle);
                if (it == this->allocations.end())
                {
                    return false;
                }

                if (it->second.backing_memory != 0)
                {
                    memory.release_memory(it->second.backing_memory, 0);
                }

                this->allocations.erase(it);
                return true;
            }

            size_t destroy_resource_allocations(memory_manager& memory, const uint32_t resource_handle)
            {
                size_t destroy_count = 0;

                for (auto it = this->allocations.begin(); it != this->allocations.end();)
                {
                    if (it->second.resource_handle != resource_handle)
                    {
                        ++it;
                        continue;
                    }

                    if (it->second.backing_memory != 0)
                    {
                        memory.release_memory(it->second.backing_memory, 0);
                    }

                    it = this->allocations.erase(it);
                    ++destroy_count;
                }

                return destroy_count;
            }

            void serialize(utils::buffer_serializer& buffer) const
            {
                buffer.write(this->next_resource_handle);
                buffer.write(this->next_allocation_handle);
                buffer.write_map(this->allocations);
            }

            void deserialize(utils::buffer_deserializer& buffer)
            {
                buffer.read(this->next_resource_handle);
                buffer.read(this->next_allocation_handle);
                buffer.read_map(this->allocations);
            }
        };

        process_context(x86_64_emulator& emu, memory_manager& memory, utils::clock& clock, callbacks& cb)
            : callbacks_(&cb),
              base_allocator(emu),
              peb64(emu),
              process_params64(emu),
              kusd(memory, clock),
              user_handles(memory)
        {
        }

        void setup(windows_emulator& win_emu, const application_settings& app_settings, const mapped_module& executable,
                   const mapped_module& ntdll, const apiset::container& apiset_container, const mapped_module* ntdll32 = nullptr);

        handle create_thread(memory_manager& memory, uint64_t start_address, uint64_t argument, uint64_t stack_size, uint32_t create_flags,
                             bool initial_thread = false);
        void terminate_thread(emulator_thread& thread, NTSTATUS thread_exit_status);

        std::optional<uint16_t> find_atom(std::u16string_view name);
        uint16_t add_or_find_atom(std::u16string name);
        bool delete_atom(const std::u16string& name);
        bool delete_atom(uint16_t atom_id);
        std::optional<std::u16string> get_atom_name(uint16_t atom_id) const;

        template <typename T>
        void build_knowndlls_section_table(registry_manager& registry, const file_system& file_system, const apiset_map& apiset,
                                           const windows_path& system_root, bool is_32bit);

        std::optional<section> get_knowndll_section_by_name(const std::u16string& name, bool is_32bit) const;
        void add_knowndll_section(const std::u16string& name, const section& section, bool is_32bit);
        bool has_knowndll_section(const std::u16string& name, bool is_32bit) const;

        void serialize(utils::buffer_serializer& buffer, const emulator_thread* active_thread) const;
        void deserialize(utils::buffer_deserializer& buffer, emulator_thread*& active_thread);

        generic_handle_store* get_handle_store(handle handle);
        emulator_thread* find_thread_by_id(uint32_t thread_id);
        const emulator_thread* find_thread_by_id(uint32_t thread_id) const;
        bool is_current_process_handle(handle handle) const;
        bool is_current_thread_handle(handle handle, const emulator_thread* active_thread) const;
        bool is_object_pseudo_handle(handle handle) const;
        handle resolve_object_pseudo_handle(handle handle, const emulator_thread* active_thread) const;

        size_t get_live_thread_count() const;

        // WOW64 support flag - set during process setup based on executable architecture
        bool is_wow64_process{false};

        callbacks* callbacks_{};

        std::vector<uint8_t> sid{};

        uint64_t shared_section_address{0};
        uint64_t shared_section_size{0};
        uint64_t dbwin_buffer{0};
        uint64_t dbwin_buffer_size{0};

        std::optional<NTSTATUS> exit_status{};

        emulator_allocator base_allocator;

        emulator_object<PEB64> peb64;
        emulator_object<RTL_USER_PROCESS_PARAMETERS64> process_params64;
        kusd_mmio kusd;

        uint64_t ntdll_image_base{};
        uint64_t ldr_initialize_thunk{};
        uint64_t rtl_user_thread_start{};
        uint64_t ki_user_apc_dispatcher{};
        uint64_t ki_user_exception_dispatcher{};
        uint64_t ki_user_callback_dispatcher{};
        uint64_t instrumentation_callback{};
        uint64_t zw_callback_return{};
        uint64_t dispatch_client_message{};
        uint32_t gdi_default_dc_handle{};
        std::map<uint32_t, gdi_dc_state> gdi_dc_states{};
        // Per-DC stack of states pushed by NtGdiSaveDC and popped by NtGdiRestoreDC.
        std::map<uint32_t, std::vector<gdi_dc_state>> gdi_dc_save_states{};
        std::map<uint32_t, gdi_bitmap_surface> gdi_bitmap_surfaces{};
        // Persistent per-top-level-window paint surface; child controls composite into it at their offset.
        std::map<uint32_t, gdi_bitmap_surface> gdi_window_surfaces{};
        dxgk_state dxgk{};
        std::optional<handle> etw_notification_event{};
        hwnd mouse_capture_window{};
        // The window that currently holds keyboard focus / is the foreground window, and the last known
        // cursor position in screen coordinates. Games poll these via GetForegroundWindow/GetActiveWindow
        // and GetCursorPos to drive menu cursors and gate their input loop on the window being active.
        hwnd foreground_window{};
        int32_t cursor_x{};
        int32_t cursor_y{};
        hcursor current_cursor{};
        int32_t cursor_show_count{};
        // Whether the current cursor has a visible shape. SetCursor(NULL) clears it to hide the pointer
        // without touching the show count (some games hide the cursor that way). Defaults to true since a
        // window without an explicit SetCursor still shows its class cursor. The host pointer is shown only
        // when cursor_show_count >= 0 and this is set.
        bool cursor_shape_visible{true};
        // Per-virtual-key pressed state (0x80 = down), updated from key/mouse-button events and reported by
        // GetKeyState; games poll this for in-game input (movement, etc.) rather than window messages.
        std::array<uint8_t, 256> key_state{};
        // Per-virtual-key transition state for GetAsyncKeyState's low bit. A value of 1 means the key or
        // mouse button was pressed since the last GetAsyncKeyState query for that virtual key.
        std::array<uint8_t, 256> async_key_state{};

        // Raw mouse input registration (NtUserRegisterRawInputDevices). When registered, mouse motion
        // synthesizes relative-mouse RAWINPUT delivered as WM_INPUT, so in-game mouse-look works.
        // raw_mouse_target is the explicit hwndTarget; 0 means "follow focus" (deliver to the foreground
        // window), resolved at delivery time so a registration done before any window is foreground still works.
        bool raw_mouse_registered{};
        hwnd raw_mouse_target{};
        // Keyboard raw input registration (HID usage page 0x01, usage 0x06), mirroring the mouse fields above.
        bool raw_keyboard_registered{};
        hwnd raw_keyboard_target{};
        // One pending raw-input payload (mouse motion + buttons, or a keyboard make/break) keyed by the
        // HRAWINPUT token posted in WM_INPUT's lParam; consumed by NtUserGetRawInputData.
        struct raw_input_payload
        {
            bool keyboard{};              // false = mouse, true = keyboard
            int32_t dx{};                 // mouse relative motion
            int32_t dy{};                 //
            uint16_t mouse_buttons{};     // RI_MOUSE_* button transition flags
            uint16_t mouse_button_data{}; // wheel delta for RI_MOUSE_WHEEL/RI_MOUSE_HWHEEL
            uint16_t vkey{};              // keyboard virtual key
            uint16_t scan_code{};         // keyboard scan code
            uint32_t key_message{};       // corresponding WM_KEY* or WM_SYSKEY* message
            bool key_extended{};          // true when the key's scan code has an E0 prefix (lParam bit 24)
        };
        std::map<uint32_t, raw_input_payload> raw_inputs{};
        uint32_t next_raw_input_token{1};

        // For WOW64 processes
        std::optional<emulator_object<PEB32>> peb32;
        std::optional<emulator_object<RTL_USER_PROCESS_PARAMETERS32>> process_params32;
        std::optional<uint64_t> rtl_user_thread_start32{};

        user_handle_table user_handles;
        handle default_monitor_handle{};
        handle default_desktop_window_handle{};
        handle_store<handle_types::event, event> events{};
        handle_store<handle_types::file, file> files{};
        utils::insensitive_u16string_map<file_lock_ranges> file_locks{};
        handle_store<handle_types::section, section> sections{};
        handle_store<handle_types::device, io_device_container> devices{};
        handle console_handle{};
        handle_store<handle_types::semaphore, semaphore> semaphores{};
        handle_store<handle_types::io_completion, io_completion> io_completions{};
        handle_store<handle_types::wait_completion_packet, wait_completion_packet> wait_completion_packets{};
        handle_store<handle_types::worker_factory, worker_factory> worker_factories{};
        handle_store<handle_types::port, port_container> ports{};
        handle_store<handle_types::mutant, mutant> mutants{};
        handle_store<handle_types::private_namespace, private_namespace> private_namespaces{};
        handle default_desktop{};
        handle_store<handle_types::desktop, desktop> desktops{};
        user_handle_store<handle_types::window, window> windows{user_handles};
        user_handle_store<handle_types::type::menu, menu> menus{user_handles};
        handle_store<handle_types::timer, timer> timers{};
        user_handle_store<handle_types::accelerator_table, accelerator_table> accelerator_tables{user_handles};
        handle_store<handle_types::registry, registry_key, 2> registry_keys{};
        std::map<uint32_t, handle> thread_handles_by_id{};
        std::map<uint16_t, atom_entry> atoms{};
        utils::insensitive_u16string_map<class_entry> classes{};

        apiset_map apiset;
        knowndlls_map knowndlls32_sections;
        knowndlls_map knowndlls64_sections;

        std::vector<std::byte> default_register_set{};

        // Process and thread ids mimic Windows' PspCidTable: a single space of distinct multiples of 4.
        // The process keeps id 4; threads take 8, 12, 16, ... Real Windows never hands out tiny or
        // non-4-aligned ids, and some code (e.g. CEG-style anti-tamper) relies on that.
        static constexpr uint32_t process_id = 4;
        uint32_t spawned_thread_count{0};
        handle_store<handle_types::thread, emulator_thread> threads{};

        // Extended parameters from last NtMapViewOfSectionEx call
        // These can be used by other syscalls like NtAllocateVirtualMemoryEx
        uint64_t last_extended_params_numa_node{0};
        uint32_t last_extended_params_attributes{0};
        uint16_t last_extended_params_image_machine{IMAGE_FILE_MACHINE_UNKNOWN};

        uint64_t next_luid{0x1001};
    };

} // namespace sogen
