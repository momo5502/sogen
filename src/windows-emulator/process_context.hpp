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

#define PEB_SEGMENT_SIZE        (20 << 20) // 20 MB
#define GS_SEGMENT_SIZE         (1 << 20)  // 1 MB

#define STACK_SIZE              0x40000ULL // 256KB

#define GDT_ADDR                0x35000
#define GDT_LIMIT               0x1000
#define GDT_ENTRY_SIZE          0x8

// TODO: Get rid of that
#define WOW64_NATIVE_STACK_SIZE 0x8000
#define WOW64_32BIT_STACK_SIZE  (1 << 20)

struct emulator_settings;
struct application_settings;
class windows_version_manager;

using knowndlls_map = std::map<std::u16string, section>;
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

        class_entry(const emulator_pointer guest_obj, const EMU_WNDCLASSEX& wnd_class, const CLSMENUNAME<EmulatorTraits<Emu64>>& menu_name)
            : guest_obj_addr(guest_obj),
              wnd_class(wnd_class),
              menu_name(menu_name)
        {
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

    void setup(x86_64_emulator& emu, memory_manager& memory, registry_manager& registry, file_system& file_system,
               windows_version_manager& version, const application_settings& app_settings, const mapped_module& executable,
               const mapped_module& ntdll, const apiset::container& apiset_container, const mapped_module* ntdll32 = nullptr);

    handle create_thread(memory_manager& memory, uint64_t start_address, uint64_t argument, uint64_t stack_size, uint32_t create_flags,
                         bool initial_thread = false);

    std::optional<uint16_t> find_atom(std::u16string_view name);
    uint16_t add_or_find_atom(std::u16string name);
    bool delete_atom(const std::u16string& name);
    bool delete_atom(uint16_t atom_id);
    const std::u16string* get_atom_name(uint16_t atom_id) const;

    template <typename T>
    void build_knowndlls_section_table(registry_manager& registry, const file_system& file_system, const apiset_map& apiset,
                                       const windows_path& system_root, bool is_32bit);

    std::optional<section> get_knowndll_section_by_name(const std::u16string& name, bool is_32bit) const;
    void add_knowndll_section(const std::u16string& name, const section& section, bool is_32bit);
    bool has_knowndll_section(const std::u16string& name, bool is_32bit) const;

    void serialize(utils::buffer_serializer& buffer) const;
    void deserialize(utils::buffer_deserializer& buffer);

    generic_handle_store* get_handle_store(handle handle);

    size_t get_live_thread_count() const;

    // WOW64 support flag - set during process setup based on executable architecture
    bool is_wow64_process{false};

    callbacks* callbacks_{};

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
    uint64_t instrumentation_callback{};
    uint64_t wow64_ki_user_callback_dispatcher{};
    uint64_t zw_callback_return{};
    uint64_t dispatch_client_message{};
    uint32_t gdi_default_dc_handle{};
    std::optional<handle> etw_notification_event{};

    // For WOW64 processes
    std::optional<emulator_object<PEB32>> peb32;
    std::optional<emulator_object<RTL_USER_PROCESS_PARAMETERS32>> process_params32;
    std::optional<uint64_t> rtl_user_thread_start32{};

    user_handle_table user_handles;
    handle default_monitor_handle{};
    handle_store<handle_types::event, event> events{};
    handle_store<handle_types::file, file> files{};
    handle_store<handle_types::section, section> sections{};
    handle_store<handle_types::device, io_device_container> devices{};
    handle_store<handle_types::semaphore, semaphore> semaphores{};
    handle_store<handle_types::io_completion, io_completion> io_completions{};
    handle_store<handle_types::wait_completion_packet, wait_completion_packet> wait_completion_packets{};
    handle_store<handle_types::worker_factory, worker_factory> worker_factories{};
    handle_store<handle_types::port, port_container> ports{};
    handle_store<handle_types::mutant, mutant> mutants{};
    handle default_desktop{};
    handle_store<handle_types::desktop, desktop> desktops{};
    user_handle_store<handle_types::window, window> windows{user_handles};
    handle_store<handle_types::timer, timer> timers{};
    handle_store<handle_types::registry, registry_key, 2> registry_keys{};
    std::map<uint16_t, atom_entry> atoms{};
    utils::insensitive_u16string_map<class_entry> classes{};

    apiset_map apiset;
    knowndlls_map knowndlls32_sections;
    knowndlls_map knowndlls64_sections;

    std::vector<std::byte> default_register_set{};

    uint32_t spawned_thread_count{0};
    handle_store<handle_types::thread, emulator_thread> threads{};
    emulator_thread* active_thread{nullptr};

    // Extended parameters from last NtMapViewOfSectionEx call
    // These can be used by other syscalls like NtAllocateVirtualMemoryEx
    uint64_t last_extended_params_numa_node{0};
    uint32_t last_extended_params_attributes{0};
    uint16_t last_extended_params_image_machine{IMAGE_FILE_MACHINE_UNKNOWN};
};
