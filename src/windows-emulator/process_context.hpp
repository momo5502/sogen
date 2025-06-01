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

#include "apiset/apiset.hpp"

#define PEB_SEGMENT_SIZE (20 << 20) // 20 MB
#define GS_SEGMENT_SIZE  (1 << 20)  // 1 MB

#define STACK_SIZE       0x40000ULL

#define GDT_ADDR         0x30000
#define GDT_LIMIT        0x1000
#define GDT_ENTRY_SIZE   0x8

struct emulator_settings;
struct application_settings;

struct process_context
{
    struct callbacks
    {
        utils::optional_function<void(handle h, emulator_thread& thr)> on_create_thread{};
        utils::optional_function<void(handle h, emulator_thread& thr)> on_thread_terminated{};
        utils::optional_function<void()> on_thread_switch{};
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

    process_context(x86_64_emulator& emu, memory_manager& memory, utils::clock& clock, callbacks& cb)
        : callbacks_(&cb),
          base_allocator(emu),
          peb(emu),
          process_params(emu),
          kusd(memory, clock)
    {
    }

    void setup(x86_64_emulator& emu, memory_manager& memory, registry_manager& registry,
               const application_settings& app_settings, const mapped_module& executable, const mapped_module& ntdll,
               const apiset::container& apiset_container);

    handle create_thread(memory_manager& memory, uint64_t start_address, uint64_t argument, uint64_t stack_size,
                         bool suspended);

    std::optional<uint16_t> find_atom(std::u16string_view name);
    uint16_t add_or_find_atom(std::u16string name);
    bool delete_atom(const std::u16string& name);
    bool delete_atom(uint16_t atom_id);
    const std::u16string* get_atom_name(uint16_t atom_id) const;

    void serialize(utils::buffer_serializer& buffer) const;
    void deserialize(utils::buffer_deserializer& buffer);

    generic_handle_store* get_handle_store(handle handle);

    callbacks* callbacks_{};

    uint64_t current_ip{0};
    uint64_t previous_ip{0};

    uint64_t shared_section_address{0};
    uint64_t shared_section_size{0};
    uint64_t dbwin_buffer{0};
    uint64_t dbwin_buffer_size{0};

    std::optional<uint64_t> exception_rip{};
    std::optional<NTSTATUS> exit_status{};

    emulator_allocator base_allocator;

    emulator_object<PEB64> peb;
    emulator_object<RTL_USER_PROCESS_PARAMETERS64> process_params;
    kusd_mmio kusd;

    uint64_t ntdll_image_base{};
    uint64_t ldr_initialize_thunk{};
    uint64_t rtl_user_thread_start{};
    uint64_t ki_user_apc_dispatcher{};
    uint64_t ki_user_exception_dispatcher{};

    handle_store<handle_types::event, event> events{};
    handle_store<handle_types::file, file> files{};
    handle_store<handle_types::section, section> sections{};
    handle_store<handle_types::device, io_device_container> devices{};
    handle_store<handle_types::semaphore, semaphore> semaphores{};
    handle_store<handle_types::port, port> ports{};
    handle_store<handle_types::mutant, mutant> mutants{};
    handle_store<handle_types::window, window> windows{};
    handle_store<handle_types::timer, timer> timers{};
    handle_store<handle_types::registry, registry_key, 2> registry_keys{};
    std::map<uint16_t, atom_entry> atoms{};

    std::vector<std::byte> default_register_set{};

    uint32_t spawned_thread_count{0};
    handle_store<handle_types::thread, emulator_thread> threads{};
    emulator_thread* active_thread{nullptr};
};
