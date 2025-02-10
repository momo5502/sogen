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

#define PEB_SEGMENT_SIZE (20 << 20) // 20 MB
#define GS_SEGMENT_SIZE  (1 << 20)  // 1 MB

#define IA32_GS_BASE_MSR 0xC0000101

#define STACK_SIZE       0x40000ULL

#define GDT_ADDR         0x30000
#define GDT_LIMIT        0x1000
#define GDT_ENTRY_SIZE   0x8

struct process_context
{
    process_context(x64_emulator& emu, memory_manager& memory)
        : base_allocator(emu),
          peb(emu),
          process_params(emu),
          kusd(memory, *this)
    {
    }

    uint64_t executed_instructions{0};
    uint64_t current_ip{0};
    uint64_t previous_ip{0};

    std::optional<uint64_t> exception_rip{};
    std::optional<NTSTATUS> exit_status{};

    emulator_allocator base_allocator;

    emulator_object<PEB64> peb;
    emulator_object<RTL_USER_PROCESS_PARAMETERS64> process_params;
    kusd_mmio kusd;

    // TODO: Remove this
    mapped_module* executable{};
    mapped_module* ntdll{};
    mapped_module* win32u{};

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

    void serialize(utils::buffer_serializer& buffer) const
    {
        buffer.write(this->executed_instructions);
        buffer.write(this->current_ip);
        buffer.write(this->previous_ip);
        buffer.write_optional(this->exception_rip);
        buffer.write_optional(this->exit_status);
        buffer.write(this->base_allocator);
        buffer.write(this->peb);
        buffer.write(this->process_params);
        buffer.write(this->kusd);

        buffer.write(this->executable->image_base);
        buffer.write(this->ntdll->image_base);
        buffer.write(this->win32u->image_base);

        buffer.write(this->ldr_initialize_thunk);
        buffer.write(this->rtl_user_thread_start);
        buffer.write(this->ki_user_exception_dispatcher);

        buffer.write(this->events);
        buffer.write(this->files);
        buffer.write(this->sections);
        buffer.write(this->devices);
        buffer.write(this->semaphores);
        buffer.write(this->ports);
        buffer.write(this->mutants);
        buffer.write(this->registry_keys);
        buffer.write_map(this->atoms);

        buffer.write_vector(this->default_register_set);
        buffer.write(this->spawned_thread_count);
        buffer.write(this->threads);

        buffer.write(this->threads.find_handle(this->active_thread).bits);
    }

    void deserialize(utils::buffer_deserializer& buffer)
    {
        buffer.read(this->executed_instructions);
        buffer.read(this->current_ip);
        buffer.read(this->previous_ip);
        buffer.read_optional(this->exception_rip);
        buffer.read_optional(this->exit_status);
        buffer.read(this->base_allocator);
        buffer.read(this->peb);
        buffer.read(this->process_params);
        buffer.read(this->kusd);

        const auto executable_base = buffer.read<uint64_t>();
        const auto ntdll_base = buffer.read<uint64_t>();
        const auto win32u_base = buffer.read<uint64_t>();

        auto& mod_manager = buffer.read<module_manager_wrapper>().get();

        this->executable = mod_manager.find_by_address(executable_base);
        this->ntdll = mod_manager.find_by_address(ntdll_base);
        this->win32u = mod_manager.find_by_address(win32u_base);

        buffer.read(this->ldr_initialize_thunk);
        buffer.read(this->rtl_user_thread_start);
        buffer.read(this->ki_user_exception_dispatcher);

        buffer.read(this->events);
        buffer.read(this->files);
        buffer.read(this->sections);
        buffer.read(this->devices);
        buffer.read(this->semaphores);
        buffer.read(this->ports);
        buffer.read(this->mutants);
        buffer.read(this->registry_keys);
        buffer.read_map(this->atoms);

        buffer.read_vector(this->default_register_set);
        buffer.read(this->spawned_thread_count);

        for (auto& thread : this->threads | std::views::values)
        {
            thread.leak_memory();
        }

        buffer.read(this->threads);

        this->active_thread = this->threads.get(buffer.read<uint64_t>());
    }

    handle create_thread(memory_manager& memory, const uint64_t start_address, const uint64_t argument,
                         const uint64_t stack_size)
    {
        emulator_thread t{memory, *this, start_address, argument, stack_size, ++this->spawned_thread_count};
        return this->threads.store(std::move(t));
    }
};
