#pragma once

#include "emulator_utils.hpp"
#include "handles.hpp"
#include "registry/registry_manager.hpp"

#include "module/module_manager.hpp"
#include <utils/nt_handle.hpp>

#include <x64_emulator.hpp>

#include "io_device.hpp"
#include "kusd_mmio.hpp"
#include "windows_objects.hpp"
#include "emulator_thread.hpp"

#include "apiset/apiset.hpp"

#define PEB_SEGMENT_SIZE (20 << 20) // 20 MB
#define GS_SEGMENT_SIZE  (1 << 20)  // 1 MB

#define IA32_GS_BASE_MSR 0xC0000101

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
    };

    process_context(x64_emulator& emu, memory_manager& memory, callbacks& cb)
        : callbacks_(&cb),
          base_allocator(emu),
          peb(emu),
          process_params(emu),
          kusd(memory, *this)
    {
    }

    void setup(x64_emulator& emu, memory_manager& memory, const application_settings& app_settings,
               const emulator_settings& emu_settings, const mapped_module& executable, const mapped_module& ntdll,
               const apiset::container& apiset_container);

    handle create_thread(memory_manager& memory, const uint64_t start_address, const uint64_t argument,
                         const uint64_t stack_size);

    void serialize(utils::buffer_serializer& buffer) const;
    void deserialize(utils::buffer_deserializer& buffer);

    callbacks* callbacks_{};

    uint64_t executed_instructions{0};
    uint64_t current_ip{0};
    uint64_t previous_ip{0};

    std::optional<uint64_t> exception_rip{};
    std::optional<NTSTATUS> exit_status{};

    emulator_allocator base_allocator;

    emulator_object<PEB64> peb;
    emulator_object<RTL_USER_PROCESS_PARAMETERS64> process_params;
    kusd_mmio kusd;

    uint64_t ntdll_image_base{};
    uint64_t ldr_initialize_thunk{};
    uint64_t rtl_user_thread_start{};
    uint64_t ki_user_exception_dispatcher{};

    handle_store<handle_types::event, event> events{};
    handle_store<handle_types::file, file> files{};
    handle_store<handle_types::section, section> sections{};
    handle_store<handle_types::device, io_device_container> devices{};
    handle_store<handle_types::semaphore, semaphore> semaphores{};
    handle_store<handle_types::port, port> ports{};
    handle_store<handle_types::mutant, mutant> mutants{};
    handle_store<handle_types::registry, registry_key, 2> registry_keys{};
    std::map<uint16_t, std::u16string> atoms{};

    std::vector<std::byte> default_register_set{};

    uint32_t spawned_thread_count{0};
    handle_store<handle_types::thread, emulator_thread> threads{};
    emulator_thread* active_thread{nullptr};
};
