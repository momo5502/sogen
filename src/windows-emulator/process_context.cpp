#include "std_include.hpp"
#include "process_context.hpp"

#include "emulator_utils.hpp"
#include "windows_emulator.hpp"

namespace
{
    emulator_allocator create_allocator(memory_manager& memory, const size_t size)
    {
        const auto base = memory.find_free_allocation_base(size);
        memory.allocate_memory(base, size, memory_permission::read_write);

        return emulator_allocator{memory, base, size};
    }

    void setup_gdt(x86_64_emulator& emu, memory_manager& memory)
    {
        memory.allocate_memory(GDT_ADDR, GDT_LIMIT, memory_permission::read);
        emu.load_gdt(GDT_ADDR, GDT_LIMIT);

        emu.write_memory<uint64_t>(GDT_ADDR + 6 * (sizeof(uint64_t)), 0xEFFE000000FFFF);
        emu.reg<uint16_t>(x86_register::cs, 0x33);

        emu.write_memory<uint64_t>(GDT_ADDR + 5 * (sizeof(uint64_t)), 0xEFF6000000FFFF);
        emu.reg<uint16_t>(x86_register::ss, 0x2B);
    }

    std::u16string expand_environment_string(const std::u16string& input,
                                             const utils::unordered_insensitive_u16string_map<std::u16string>& env_map)
    {
        std::u16string result;
        result.reserve(input.length());
        size_t pos = 0;

        while (pos < input.length())
        {
            size_t start = input.find(u'%', pos);
            if (start == std::u16string::npos)
            {
                result.append(input.substr(pos));
                break;
            }

            result.append(input.substr(pos, start - pos));

            size_t end = input.find(u'%', start + 1);
            if (end == std::u16string::npos)
            {
                result.append(input.substr(start));
                break;
            }

            std::u16string var_name = input.substr(start + 1, end - start - 1);

            if (var_name.empty())
            {
                result.append(u"%%");
            }
            else
            {
                auto it = env_map.find(var_name);
                result.append(it != env_map.end() ? it->second : input.substr(start, end - start + 1));
            }

            pos = end + 1;
        }
        return result;
    }

    utils::unordered_insensitive_u16string_map<std::u16string> get_environment_variables(registry_manager& registry)
    {
        utils::unordered_insensitive_u16string_map<std::u16string> env_map;
        std::unordered_set<std::u16string_view> keys_to_expand;

        const auto env_key =
            registry.get_key({R"(\Registry\Machine\System\CurrentControlSet\Control\Session Manager\Environment)"});
        if (env_key)
        {
            for (size_t i = 0; const auto value_opt = registry.get_value(*env_key, i); i++)
            {
                const auto& value = *value_opt;

                if (value.type != REG_SZ && value.type != REG_EXPAND_SZ)
                {
                    continue;
                }

                if (value.data.empty() || value.data.size() % 2 != 0)
                {
                    continue;
                }

                const auto char_count = value.data.size() / sizeof(char16_t);
                const auto* data_ptr = reinterpret_cast<const char16_t*>(value.data.data());
                if (data_ptr[char_count - 1] != u'\0')
                {
                    continue;
                }

                const auto [it, inserted] =
                    env_map.emplace(u8_to_u16(value.name), std::u16string(data_ptr, char_count - 1));
                if (inserted && value.type == REG_EXPAND_SZ)
                {
                    keys_to_expand.insert(it->first);
                }
            }
        }

        env_map[u"EMULATOR"] = u"1";

        const auto* env = getenv("EMULATOR_ICICLE");
        if (env && (env == "1"sv || env == "true"sv))
        {
            env_map[u"EMULATOR_ICICLE"] = u"1";
        }

        env_map[u"COMPUTERNAME"] = u"momo";
        env_map[u"USERNAME"] = u"momo";
        env_map[u"SystemDrive"] = u"C:";
        env_map[u"SystemRoot"] = u"C:\\WINDOWS";

        for (const auto& key : keys_to_expand)
        {
            auto it = env_map.find(key);
            if (it != env_map.end())
            {
                std::u16string expanded = expand_environment_string(it->second, env_map);
                if (expanded != it->second)
                {
                    it->second = expanded;
                }
            }
        }

        return env_map;
    }
}

void process_context::setup(x86_64_emulator& emu, memory_manager& memory, registry_manager& registry,
                            const application_settings& app_settings, const mapped_module& executable,
                            const mapped_module& ntdll, const apiset::container& apiset_container)
{
    setup_gdt(emu, memory);

    this->kusd.setup();

    this->base_allocator = create_allocator(memory, PEB_SEGMENT_SIZE);
    auto& allocator = this->base_allocator;

    this->peb = allocator.reserve<PEB64>();

    /* Values of the following fields must be
     * allocated relative to the process_params themselves
     * and included in the length:
     *
     * CurrentDirectory
     * DllPath
     * ImagePathName
     * CommandLine
     * WindowTitle
     * DesktopInfo
     * ShellInfo
     * RuntimeData
     * RedirectionDllName
     */

    this->process_params = allocator.reserve<RTL_USER_PROCESS_PARAMETERS64>();

    this->process_params.access([&](RTL_USER_PROCESS_PARAMETERS64& proc_params) {
        proc_params.Flags = 0x6001; //| 0x80000000; // Prevent CsrClientConnectToServer

        proc_params.ConsoleHandle = CONSOLE_HANDLE.h;
        proc_params.StandardOutput = STDOUT_HANDLE.h;
        proc_params.StandardInput = STDIN_HANDLE.h;
        proc_params.StandardError = proc_params.StandardOutput;

        proc_params.Environment = allocator.copy_string(u"=::=::\\");

        const auto env_map = get_environment_variables(registry);
        for (const auto& [name, value] : env_map)
        {
            std::u16string entry;
            entry += name;
            entry += u"=";
            entry += value;
            allocator.copy_string(entry);
        }

        allocator.copy_string(u"");

        const auto application_str = app_settings.application.u16string();

        std::u16string command_line = u"\"" + application_str + u"\"";

        for (const auto& arg : app_settings.arguments)
        {
            command_line.push_back(u' ');
            command_line.append(arg);
        }

        allocator.make_unicode_string(proc_params.CommandLine, command_line);
        allocator.make_unicode_string(proc_params.CurrentDirectory.DosPath,
                                      app_settings.working_directory.u16string() + u"\\", 1024);
        allocator.make_unicode_string(proc_params.ImagePathName, application_str);

        const auto total_length = allocator.get_next_address() - this->process_params.value();

        proc_params.Length = static_cast<uint32_t>(std::max(static_cast<uint64_t>(sizeof(proc_params)), total_length));
        proc_params.MaximumLength = proc_params.Length;
    });

    this->peb.access([&](PEB64& p) {
        p.BeingDebugged = 0;
        p.ImageBaseAddress = executable.image_base;
        p.ProcessParameters = this->process_params.value();
        p.ApiSetMap = apiset::clone(emu, allocator, apiset_container).value();

        p.ProcessHeap = 0;
        p.ProcessHeaps = 0;
        p.HeapSegmentReserve = 0x0000000000100000; // TODO: Read from executable
        p.HeapSegmentCommit = 0x0000000000002000;
        p.HeapDeCommitTotalFreeThreshold = 0x0000000000010000;
        p.HeapDeCommitFreeBlockThreshold = 0x0000000000001000;
        p.NumberOfHeaps = 0x00000000;
        p.MaximumNumberOfHeaps = 0x00000010;

        p.OSPlatformId = 2;
        p.OSMajorVersion = 0x0000000a;
        p.OSBuildNumber = 0x00006c51;

        // p.AnsiCodePageData = allocator.reserve<CPTABLEINFO>().value();
        // p.OemCodePageData = allocator.reserve<CPTABLEINFO>().value();
        p.UnicodeCaseTableData = allocator.reserve<NLSTABLEINFO>().value();
    });

    this->ntdll_image_base = ntdll.image_base;
    this->ldr_initialize_thunk = ntdll.find_export("LdrInitializeThunk");
    this->rtl_user_thread_start = ntdll.find_export("RtlUserThreadStart");
    this->ki_user_apc_dispatcher = ntdll.find_export("KiUserApcDispatcher");
    this->ki_user_exception_dispatcher = ntdll.find_export("KiUserExceptionDispatcher");

    this->default_register_set = emu.save_registers();
}

void process_context::serialize(utils::buffer_serializer& buffer) const
{
    buffer.write(this->current_ip);
    buffer.write(this->previous_ip);
    buffer.write(this->shared_section_address);
    buffer.write(this->shared_section_size);
    buffer.write(this->dbwin_buffer);
    buffer.write(this->dbwin_buffer_size);
    buffer.write_optional(this->exception_rip);
    buffer.write_optional(this->exit_status);
    buffer.write(this->base_allocator);
    buffer.write(this->peb);
    buffer.write(this->process_params);
    buffer.write(this->kusd);

    buffer.write(this->ntdll_image_base);
    buffer.write(this->ldr_initialize_thunk);
    buffer.write(this->rtl_user_thread_start);
    buffer.write(this->ki_user_apc_dispatcher);
    buffer.write(this->ki_user_exception_dispatcher);

    buffer.write(this->events);
    buffer.write(this->files);
    buffer.write(this->sections);
    buffer.write(this->devices);
    buffer.write(this->semaphores);
    buffer.write(this->ports);
    buffer.write(this->mutants);
    buffer.write(this->windows);
    buffer.write(this->timers);
    buffer.write(this->registry_keys);
    buffer.write_map(this->atoms);

    buffer.write_vector(this->default_register_set);
    buffer.write(this->spawned_thread_count);
    buffer.write(this->threads);

    buffer.write(this->threads.find_handle(this->active_thread).bits);
}

void process_context::deserialize(utils::buffer_deserializer& buffer)
{
    buffer.read(this->current_ip);
    buffer.read(this->previous_ip);
    buffer.read(this->shared_section_address);
    buffer.read(this->shared_section_size);
    buffer.read(this->dbwin_buffer);
    buffer.read(this->dbwin_buffer_size);
    buffer.read_optional(this->exception_rip);
    buffer.read_optional(this->exit_status);
    buffer.read(this->base_allocator);
    buffer.read(this->peb);
    buffer.read(this->process_params);
    buffer.read(this->kusd);

    buffer.read(this->ntdll_image_base);
    buffer.read(this->ldr_initialize_thunk);
    buffer.read(this->rtl_user_thread_start);
    buffer.read(this->ki_user_apc_dispatcher);
    buffer.read(this->ki_user_exception_dispatcher);

    buffer.read(this->events);
    buffer.read(this->files);
    buffer.read(this->sections);
    buffer.read(this->devices);
    buffer.read(this->semaphores);
    buffer.read(this->ports);
    buffer.read(this->mutants);
    buffer.read(this->windows);
    buffer.read(this->timers);
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

generic_handle_store* process_context::get_handle_store(const handle handle)
{
    switch (handle.value.type)
    {
    case handle_types::thread:
        return &threads;
    case handle_types::event:
        return &events;
    case handle_types::file:
        return &files;
    case handle_types::device:
        return &devices;
    case handle_types::semaphore:
        return &semaphores;
    case handle_types::registry:
        return &registry_keys;
    case handle_types::mutant:
        return &mutants;
    case handle_types::port:
        return &ports;
    case handle_types::section:
        return &sections;
    default:
        return nullptr;
    }
}

handle process_context::create_thread(memory_manager& memory, const uint64_t start_address, const uint64_t argument,
                                      const uint64_t stack_size, const bool suspended)
{
    emulator_thread t{memory, *this, start_address, argument, stack_size, suspended, ++this->spawned_thread_count};
    auto [h, thr] = this->threads.store_and_get(std::move(t));
    this->callbacks_->on_create_thread(h, *thr);
    return h;
}

std::optional<uint16_t> process_context::find_atom(const std::u16string_view name)
{
    for (auto& entry : this->atoms)
    {
        if (entry.second.name == name)
        {
            ++entry.second.ref_count;
            return entry.first;
        }
    }

    return {};
}

uint16_t process_context::add_or_find_atom(std::u16string name)
{
    uint16_t index = 1;
    if (!this->atoms.empty())
    {
        auto i = this->atoms.end();
        --i;
        index = i->first + 1;
    }

    std::optional<uint16_t> last_entry{};
    for (auto& entry : this->atoms)
    {
        if (entry.second.name == name)
        {
            ++entry.second.ref_count;
            return entry.first;
        }

        if (entry.first > 0)
        {
            if (!last_entry)
            {
                index = 1;
            }
            else
            {
                const auto diff = entry.first - *last_entry;
                if (diff > 1)
                {
                    index = *last_entry + 1;
                }
            }
        }

        last_entry = entry.first;
    }

    atoms[index] = {std::move(name), 1};

    return index;
}

bool process_context::delete_atom(const std::u16string& name)
{
    for (auto it = atoms.begin(); it != atoms.end(); ++it)
    {
        if (it->second.name == name)
        {
            if (--it->second.ref_count == 0)
            {
                atoms.erase(it);
            }
            return true;
        }
    }

    return false;
}

bool process_context::delete_atom(const uint16_t atom_id)
{
    const auto it = atoms.find(atom_id);
    if (it == atoms.end())
    {
        return false;
    }

    if (--it->second.ref_count == 0)
    {
        atoms.erase(it);
    }

    return true;
}

const std::u16string* process_context::get_atom_name(const uint16_t atom_id) const
{
    const auto it = atoms.find(atom_id);
    if (it == atoms.end())
    {
        return nullptr;
    }

    return &it->second.name;
}
