#include <arch/windows_abi.hpp>
#include <backend_selection.hpp>
#include <cpu_context.hpp>
#include <emulator_thread.hpp>
#include <memory_manager.hpp>
#include <process_context.hpp>
#include <module/module_mapping.hpp>
#include <filesystem/mount_provider.hpp>
#include <version/windows_version_manager.hpp>
#include <logger.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>

namespace sogen::test
{
#if defined(_WIN64) && !defined(__MINGW64__)
    static_assert(sizeof(arm64_context) == sizeof(::ARM64_NT_CONTEXT));
    static_assert(alignof(arm64_context) == alignof(::ARM64_NT_CONTEXT));
    static_assert(offsetof(arm64_context, X) == offsetof(::ARM64_NT_CONTEXT, X));
    static_assert(offsetof(arm64_context, Sp) == offsetof(::ARM64_NT_CONTEXT, Sp));
    static_assert(offsetof(arm64_context, Pc) == offsetof(::ARM64_NT_CONTEXT, Pc));
    static_assert(offsetof(arm64_context, V) == offsetof(::ARM64_NT_CONTEXT, V));
    static_assert(offsetof(arm64_context, Bcr) == offsetof(::ARM64_NT_CONTEXT, Bcr));
    static_assert(offsetof(arm64_context, Wvr) == offsetof(::ARM64_NT_CONTEXT, Wvr));
#endif

    namespace
    {
        struct x86_64_guest
        {
            using machine = x86_64_emulator;
            static constexpr auto architecture = windows_guest_architecture::x86_64;
            static constexpr auto pe_machine = PEMachineType::AMD64;

            static std::unique_ptr<machine> create()
            {
                return create_x86_64_emulator(backend_type::unicorn);
            }
        };

        struct arm64_guest
        {
            using machine = arm64_emulator;
            static constexpr auto architecture = windows_guest_architecture::arm64;
            static constexpr auto pe_machine = PEMachineType::ARM64;

            static std::unique_ptr<machine> create()
            {
                return create_arm64_emulator();
            }
        };

        template <typename Guest>
        constexpr bool is_arm64 = Guest::architecture == windows_guest_architecture::arm64;

#if defined(_M_ARM64)
        using guest_architectures = testing::Types<arm64_guest>;
#else
        using guest_architectures = testing::Types<x86_64_guest, arm64_guest>;
#endif

#if defined(_WIN64) && !defined(__MINGW64__)
#if defined(_M_ARM64)
        constexpr auto host_architecture = windows_guest_architecture::arm64;
#else
        constexpr auto host_architecture = windows_guest_architecture::x86_64;
#endif
#endif

        struct guest_name
        {
            template <typename Guest>
            static std::string GetName(int)
            {
                return is_arm64<Guest> ? "arm64" : "x86_64";
            }
        };

        constexpr uint64_t code_base = 0x10000;
        constexpr uint32_t image_nt_offset = 0x80;
        constexpr uint64_t image_base = 0x1000000;

        template <typename T>
        std::vector<std::byte> make_test_image(const PEMachineType machine)
        {
            std::vector<std::byte> image(0x2000);
            PEDosHeader_t dos{};
            dos.e_magic = PEDosHeader_t::k_Magic;
            dos.e_lfanew = image_nt_offset;
            PENTHeaders_t<T> headers{};
            headers.Signature = PENTHeaders_t<T>::k_Signature;
            headers.FileHeader.Machine = machine;
            headers.FileHeader.NumberOfSections = 1;
            headers.FileHeader.SizeOfOptionalHeader = sizeof(PEOptionalHeader_t<T>);
            headers.FileHeader.Characteristics = IMAGE_FILE_DLL;
            auto& optional = headers.OptionalHeader;
            optional.Magic = PEOptionalHeader_t<T>::k_Magic;
            optional.ImageBase = static_cast<T>(image_base);
            optional.SizeOfImage = 0x2000;
            optional.SizeOfHeaders = 0x1000;
            optional.SectionAlignment = 0x1000;
            optional.FileAlignment = 0x1000;
            optional.AddressOfEntryPoint = 0x1000;
            optional.SizeOfStackReserve = 0x10000;
            optional.SizeOfStackCommit = 0x1000;
            optional.SizeOfHeapReserve = 0x20000;
            optional.SizeOfHeapCommit = 0x2000;
            optional.NumberOfRvaAndSizes = PEOptionalHeaderBasePart1_t<T>::k_NumberOfDataDirectors;
            optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC] = {0x1100, 12};
            IMAGE_SECTION_HEADER section{};
            std::memcpy(section.Name, ".text", 5);
            section.Misc.VirtualSize = 0x1000;
            section.VirtualAddress = 0x1000;
            section.SizeOfRawData = 0x1000;
            section.PointerToRawData = 0x1000;
            section.Characteristics = IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE;
            std::memcpy(image.data(), &dos, sizeof(dos));
            std::memcpy(image.data() + image_nt_offset, &headers, sizeof(headers));
            std::memcpy(image.data() + image_nt_offset + sizeof(headers), &section, sizeof(section));
            const T pointer = static_cast<T>(image_base + 0x1200);
            std::memcpy(image.data() + 0x1010, &pointer, sizeof(pointer));
            const std::array<uint32_t, 2> relocation_block{0x1000, 12};
            std::memcpy(image.data() + 0x1100, relocation_block.data(), sizeof(relocation_block));
            constexpr auto relocation_type = sizeof(T) == sizeof(uint64_t) ? IMAGE_REL_BASED_DIR64 : IMAGE_REL_BASED_HIGHLOW;
            const auto relocation = static_cast<uint16_t>((relocation_type << 12) | 0x10);
            std::memcpy(image.data() + 0x1108, &relocation, sizeof(relocation));
            return image;
        }

        struct native_images
        {
            windows_path system_root{};
            windows_path executable{};
        };

        template <typename Guest>
        class WindowsArchitectureTest : public testing::Test
        {
          protected:
            static constexpr auto architecture = Guest::architecture;
            static constexpr auto other_architecture =
                is_arm64<Guest> ? windows_guest_architecture::x86_64 : windows_guest_architecture::arm64;

            std::unique_ptr<typename Guest::machine> machine_ = Guest::create();
            memory_manager memory_{machine_->memory()};
            utils::clock clock_{};
            process_context::callbacks callbacks_{};
            process_context process_{machine_->memory(), memory_, clock_, callbacks_, true, architecture};
            const windows_arch_abi& abi_ = windows_abi_for(architecture);
            file_system file_sys_{{}};
            module_manager::callbacks module_callbacks_{};
            module_manager modules_{memory_, file_sys_, module_callbacks_, architecture};
            logger log_{};

            void SetUp() override
            {
                log_.set_silent(true);
                ASSERT_TRUE(memory_.allocate_memory(code_base, 0x1000, memory_permission::all));
                process_.base_allocator =
                    emulator_allocator{memory_, memory_.allocate_memory(0x2000, memory_permission::read_write), 0x2000};
                process_.peb64 = process_.base_allocator.reserve<PEB64>();
                process_.rtl_user_thread_start = code_base + 0x100;
                process_.ldr_initialize_thunk = code_base + 0x200;
                process_.ntdll_image_base = code_base;
                process_.default_register_set = machine_->save_registers();
            }

            void set_register_argument(const size_t index, const uint64_t value)
            {
                if constexpr (is_arm64<Guest>)
                {
                    machine_->reg(static_cast<arm64_register>(static_cast<int>(arm64_register::x0) + static_cast<int>(index)), value);
                }
                else
                {
                    constexpr std::array registers{x86_register::rcx, x86_register::rdx, x86_register::r8, x86_register::r9};
                    machine_->reg(registers.at(index), value);
                    if (index == 0)
                    {
                        machine_->reg(x86_register::r10, value + 1);
                    }
                }
            }

            // Host-native guests use the installed system DLLs and a sample built by this project.
            // Foreign guests need an emulation root holding that architecture's Windows directory.
            std::optional<native_images> find_native_images()
            {
                if constexpr (uses_host_system_dlls)
                {
                    return this->map_host_images();
                }
                else if constexpr (is_arm64<Guest>)
                {
                    return this->map_emulation_root_images("EMULATOR_ARM64_ROOT", "hello_crt.exe");
                }
                else
                {
                    return std::nullopt;
                }
            }

          private:
#if defined(_WIN64) && !defined(__MINGW64__)
            static constexpr bool uses_host_system_dlls = architecture == host_architecture;

            native_images map_host_images()
            {
                std::array<wchar_t, MAX_PATH> directory{};
                const auto length = GetWindowsDirectoryW(directory.data(), static_cast<UINT>(directory.size()));
                if (length == 0 || length >= directory.size())
                {
                    throw std::runtime_error("GetWindowsDirectoryW failed");
                }

                file_sys_.map(windows_path{"C:\\sogen-native-test.exe"}, std::filesystem::path{SOGEN_NATIVE_SAMPLE_PATH});
                return {
                    .system_root = windows_path{std::filesystem::path {
                        directory.data()
                    }},
                    .executable = windows_path{"C:\\sogen-native-test.exe"},
                };
            }
#else
            static constexpr bool uses_host_system_dlls = false;

            // Never called; Clang still looks the name up in the discarded branch above.
            static native_images map_host_images()
            {
                throw std::logic_error("The host does not provide Windows system DLLs");
            }
#endif

            std::optional<native_images> map_emulation_root_images(const char* variable, const char* executable)
            {
                const auto* root = std::getenv(variable);
                if (!root || !*root)
                {
                    return std::nullopt;
                }

                // Samples are copied into the root's C:\ like test-sample.exe; the root itself holds only system DLLs.
                const auto drive = std::filesystem::path{root} / "filesys" / "c";
                if (!std::filesystem::exists(drive / executable))
                {
                    return std::nullopt;
                }

                const auto executable_path = windows_path{"C:\\"} / executable;
                file_sys_.map(windows_path{"C:\\Windows"}, drive / "windows");
                file_sys_.map(executable_path, drive / executable);
                return native_images{
                    .system_root = windows_path{"C:\\Windows"},
                    .executable = executable_path,
                };
            }
        };

        using Arm64WindowsArchitectureTest = WindowsArchitectureTest<arm64_guest>;
    }

    TYPED_TEST_SUITE(WindowsArchitectureTest, guest_architectures, guest_name);

#if !defined(_M_ARM64)
    TEST(UnicornMemoryTest, ZeroLengthWriteToExecutablePage)
    {
        auto machine = create_x86_64_emulator(backend_type::unicorn);
        memory_manager memory{machine->memory()};

        constexpr uint64_t address = 0x10000;
        ASSERT_TRUE(memory.allocate_memory(address, 0x1000, memory_permission::all));

        constexpr uint8_t value = 0x90;
        machine->write_memory(address + 0x10, &value, sizeof(value));

        EXPECT_NO_THROW(machine->write_memory(address + 0x10, &value, 0));
        EXPECT_TRUE(machine->try_write_memory(address + 0x10, &value, 0));
        EXPECT_EQ(machine->read_memory<uint8_t>(address + 0x10), value);
    }
#endif

    TYPED_TEST(WindowsArchitectureTest, Pe64MachineIsPreservedAndRelocationsUseTheSharedMapper)
    {
        constexpr auto machine = TypeParam::pe_machine;
        const auto image = make_test_image<uint64_t>(machine);
        const auto first = map_module_from_data<uint64_t>(this->memory_, image, "native.dll", windows_path{"C:\\native.dll"});
        EXPECT_EQ(first.machine, static_cast<uint16_t>(machine));
        EXPECT_EQ(first.entry_point, first.image_base + 0x1000);
        EXPECT_EQ(first.size_of_stack_reserve, 0x10000u);
        EXPECT_EQ(first.size_of_heap_commit, 0x2000u);
        const auto detection = pe_architecture_detector::detect_from_memory(this->memory_, first.image_base, first.size_of_image);
        ASSERT_TRUE(detection.is_valid());
        EXPECT_EQ(detection.machine, machine);
        EXPECT_EQ(detection.architecture, winpe::pe_arch::pe64);
        EXPECT_EQ(detection.suggested_mode, execution_mode::native_64bit);
        const auto* recorded = this->modules_.map_memory_module(first.image_base, first.size_of_image, first.module_path, this->log_);
        ASSERT_NE(recorded, nullptr);
        EXPECT_EQ(recorded->size_of_stack_reserve, first.size_of_stack_reserve);
        EXPECT_EQ(recorded->size_of_heap_commit, first.size_of_heap_commit);
        const auto relocated = map_module_from_data<uint64_t>(this->memory_, image, "copy.dll", windows_path{"C:\\copy.dll"});
        ASSERT_NE(relocated.image_base, first.image_base);
        EXPECT_EQ(this->memory_.template read_memory<uint64_t>(relocated.image_base + 0x1010), relocated.image_base + 0x1200);
        EXPECT_EQ(relocated.sections.front().region.permissions, memory_permission::read | memory_permission::exec);
    }

    TYPED_TEST(WindowsArchitectureTest, Pe32MemoryMappingKeepsItsNativeHeaderLayout)
    {
        const auto image = make_test_image<uint32_t>(PEMachineType::I386);
        const auto file_mapped = map_module_from_data<uint32_t>(this->memory_, image, "compat.dll", windows_path{"C:\\compat.dll"});
        const auto mapped =
            map_module_from_memory<uint32_t>(this->memory_, file_mapped.image_base, file_mapped.size_of_image, file_mapped.module_path);
        EXPECT_EQ(mapped.machine, static_cast<uint16_t>(PEMachineType::I386));
        EXPECT_EQ(mapped.size_of_stack_reserve, file_mapped.size_of_stack_reserve);
        EXPECT_EQ(mapped.size_of_heap_commit, file_mapped.size_of_heap_commit);
        EXPECT_EQ(mapped.entry_point, file_mapped.entry_point);
        EXPECT_EQ(mapped.sections.size(), file_mapped.sections.size());
    }

    TYPED_TEST(WindowsArchitectureTest, ForeignAndHybridMachinesAreRejectedBeforeModuleCallbacks)
    {
        size_t callbacks = 0;
        this->module_callbacks_.on_module_load.add([&](mapped_module&) { ++callbacks; });
        const std::array machines{PEMachineType::AMD64, PEMachineType::ARM64, PEMachineType::IA64, PEMachineType::ARM64EC,
                                  static_cast<PEMachineType>(0xA64E)};
        for (const auto machine : machines)
        {
            if (machine == TypeParam::pe_machine || (!is_arm64<TypeParam> && machine == PEMachineType::ARM64EC))
            {
                continue;
            }
            const auto image = make_test_image<uint64_t>(machine);
            ASSERT_TRUE(this->memory_.allocate_memory(image_base, image.size(), memory_permission::read_write));
            this->memory_.write_memory(image_base, image.data(), image.size());
            EXPECT_EQ(this->modules_.map_memory_module(image_base, image.size(), windows_path{"C:\\foreign.dll"}, this->log_), nullptr);
            EXPECT_TRUE(this->memory_.release_memory(image_base, 0));
        }
        EXPECT_EQ(callbacks, 0u);
        EXPECT_TRUE(this->modules_.modules().empty());
        const auto inconsistent = make_test_image<uint32_t>(PEMachineType::ARM64);
        EXPECT_THROW(map_module_from_data<uint32_t>(this->memory_, inconsistent, "bad.dll", windows_path{"C:\\bad.dll"}),
                     std::runtime_error);
        EXPECT_FALSE(this->memory_.get_region_info(image_base).is_reserved);
    }

    TYPED_TEST(WindowsArchitectureTest, Arm64ecImagesLoadOnlyOnTheX64Guest)
    {
        auto image = make_test_image<uint64_t>(PEMachineType::ARM64EC);
        const auto section_offset = image_nt_offset + sizeof(PENTHeaders_t<uint64_t>);
        IMAGE_SECTION_HEADER section{};
        std::memcpy(&section, image.data() + section_offset, sizeof(section));
        section.Characteristics = IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_READ;
        std::memcpy(image.data() + section_offset, &section, sizeof(section));
        ASSERT_TRUE(this->memory_.allocate_memory(image_base, image.size(), memory_permission::read_write));
        this->memory_.write_memory(image_base, image.data(), image.size());

        const auto* mod = this->modules_.map_memory_module(image_base, image.size(), windows_path{"C:\\hybrid.dll"}, this->log_);
        if constexpr (is_arm64<TypeParam>)
        {
            EXPECT_EQ(mod, nullptr);
        }
        else
        {
            ASSERT_NE(mod, nullptr);
            EXPECT_EQ(mod->sections.front().region.permissions, memory_permission::read | memory_permission::exec);
        }
    }

    TYPED_TEST(WindowsArchitectureTest, ModuleSnapshotsRejectArchitectureMismatchBeforeMutation)
    {
        utils::buffer_serializer serializer{};
        this->modules_.serialize(serializer);
        module_manager other_modules{this->memory_, this->file_sys_, this->module_callbacks_, this->other_architecture};
        utils::buffer_deserializer wrong{serializer};
        EXPECT_THROW(other_modules.deserialize(wrong), std::runtime_error);
        EXPECT_TRUE(other_modules.modules().empty());
        utils::buffer_deserializer same{serializer};
        EXPECT_NO_THROW(this->modules_.deserialize(same));
    }

    TYPED_TEST(WindowsArchitectureTest, NativeSystemDllsAndCompilerProducedExecutableUseTheSameModuleManager)
    {
        const auto images = this->find_native_images();
        if (!images)
        {
            GTEST_SKIP() << "Set EMULATOR_ARM64_ROOT to an ARM64 emulation root containing C:\\hello_crt.exe";
        }

        windows_version_manager version{};
        version.set_system_root(images->system_root);
        this->modules_.map_main_modules(*this->machine_, images->executable, version, this->process_, this->log_);
        ASSERT_NE(this->modules_.executable, nullptr);
        ASSERT_NE(this->modules_.ntdll, nullptr);
        ASSERT_NE(this->modules_.win32u, nullptr);
        for (const auto* module : {this->modules_.executable, this->modules_.ntdll, this->modules_.win32u})
        {
            EXPECT_EQ(module->machine, static_cast<uint16_t>(TypeParam::pe_machine));
            EXPECT_GT(module->sections.size(), 0u);
        }
        EXPECT_FALSE(this->modules_.executable->imports.empty());
        EXPECT_EQ(this->modules_.get_execution_mode(), execution_mode::native_64bit);
        EXPECT_FALSE(this->process_.is_wow64_process);
        this->process_.ldr_initialize_thunk = this->modules_.ntdll->find_export("LdrInitializeThunk");
        this->process_.rtl_user_thread_start = this->modules_.ntdll->find_export("RtlUserThreadStart");
        this->process_.ntdll_image_base = this->modules_.ntdll->image_base;
        const auto handle = this->process_.create_thread(this->memory_, this->modules_.executable->entry_point, 0,
                                                         this->modules_.executable->size_of_stack_reserve, 0);
        auto* thread = this->process_.threads.get(handle);
        ASSERT_NE(thread, nullptr);
        thread->setup_if_necessary(*this->machine_, this->process_);
        EXPECT_EQ(this->abi_.instruction_pointer(*this->machine_), this->process_.ldr_initialize_thunk);
        EXPECT_EQ(this->abi_.function_argument(*this->machine_, 1), this->modules_.ntdll->image_base);

        if constexpr (is_arm64<TypeParam>)
        {
            const auto symbol = std::ranges::find_if(this->modules_.ntdll->exports,
                                                     [](const exported_symbol& entry) { return entry.name == "NtYieldExecution"; });
            ASSERT_NE(symbol, this->modules_.ntdll->exports.end());
            const auto data =
                this->machine_->read_memory(this->modules_.ntdll->image_base, static_cast<size_t>(this->modules_.ntdll->size_of_image));
            const auto syscall_id = this->abi_.syscall_abi().scan_syscall_id(*symbol, data);
            ASSERT_TRUE(syscall_id.has_value());
            size_t syscalls = 0;
            this->machine_->hook_instruction(arm64_hookable_instructions::svc, [&](cpu_interface& cpu, const uint64_t service) {
                EXPECT_EQ(service, *syscall_id);
                EXPECT_EQ(this->abi_.raw_syscall_id(cpu), *syscall_id);
                this->abi_.return_value(cpu, STATUS_SUCCESS);
                ++syscalls;
                return instruction_hook_continuation::skip_instruction;
            });
            this->machine_->reg(arm64_register::lr, code_base);
            this->abi_.instruction_pointer(*this->machine_, symbol->address);
            for (size_t step = 0; step < 16 && this->abi_.instruction_pointer(*this->machine_) != code_base; ++step)
            {
                this->machine_->start(1);
            }
            EXPECT_EQ(this->abi_.instruction_pointer(*this->machine_), code_base);
            EXPECT_EQ(this->abi_.return_value(*this->machine_), STATUS_SUCCESS);
            EXPECT_EQ(syscalls, 1u);
        }
    }

    TYPED_TEST(WindowsArchitectureTest, NativeThreadAllocatesStackAndInstallsThreadEnvironment)
    {
        constexpr uint64_t start = code_base + 0x300;
        constexpr uint64_t argument = 0xAABBCCDD11223344ull;
        emulator_thread thread{this->memory_, this->process_, start, argument, 0x10000, 0, 8, true};
        ASSERT_NE(thread.stack_base, 0u);
        ASSERT_GE(thread.stack_size, 0x10000u);
        ASSERT_TRUE(thread.teb64.has_value());
        const auto teb = thread.teb64->read();
        EXPECT_EQ(teb.NtTib.Self, thread.teb64->value());
        EXPECT_EQ(teb.NtTib.StackBase, thread.stack_base + thread.stack_size);
        EXPECT_EQ(teb.NtTib.StackLimit, thread.stack_base);
        EXPECT_EQ(teb.ProcessEnvironmentBlock, this->process_.peb64.value());
        thread.setup_if_necessary(*this->machine_, this->process_);
        ASSERT_TRUE(thread.setup_done);
        EXPECT_EQ(this->abi_.instruction_pointer(*this->machine_), this->process_.ldr_initialize_thunk);
        const auto context_address = this->abi_.function_argument(*this->machine_, 0);
        EXPECT_EQ(this->abi_.function_argument(*this->machine_, 1), this->process_.ntdll_image_base);
        EXPECT_GE(this->abi_.stack_pointer(*this->machine_), thread.stack_base);
        EXPECT_LT(this->abi_.stack_pointer(*this->machine_), thread.stack_base + thread.stack_size);
        if constexpr (is_arm64<TypeParam>)
        {
            EXPECT_EQ(this->machine_->reg(arm64_register::x18), thread.teb64->value());
            EXPECT_EQ(this->abi_.stack_pointer(*this->machine_) % 16, 0u);
            const auto context = this->machine_->template read_memory<arm64_context>(context_address);
            EXPECT_EQ(context.ContextFlags, arm64_context_all);
            EXPECT_EQ(context.Pc, this->process_.rtl_user_thread_start);
            EXPECT_EQ(context.X[0], start);
            EXPECT_EQ(context.X[1], argument);
            EXPECT_EQ(context.X[18], thread.teb64->value());
            cpu_context::restore(*this->machine_, context);
        }
        else
        {
            EXPECT_EQ(this->machine_->get_segment_base(x86_register::gs), thread.teb64->value());
            EXPECT_EQ(this->abi_.stack_pointer(*this->machine_) % 16, 8u);
            const auto context = this->machine_->template read_memory<CONTEXT64>(context_address);
            EXPECT_EQ(context.Rip, this->process_.rtl_user_thread_start);
            EXPECT_EQ(context.Rcx, start);
            EXPECT_EQ(context.Rdx, argument);
            cpu_context::restore(*this->machine_, context);
        }
        EXPECT_EQ(this->abi_.instruction_pointer(*this->machine_), this->process_.rtl_user_thread_start);
        EXPECT_EQ(this->abi_.function_argument(*this->machine_, 0), start);
        EXPECT_EQ(this->abi_.function_argument(*this->machine_, 1), argument);
    }

    TYPED_TEST(WindowsArchitectureTest, ThreadSetupFailureCanBeRetriedAndPendingResultUsesNativeRegister)
    {
        emulator_thread thread{this->memory_, this->process_, code_base + 0x300, 11, 0x10000, 0, 8, true};
        const auto architecture = this->process_.native_architecture;
        this->process_.native_architecture = is_arm64<TypeParam> ? windows_guest_architecture::x86_64 : windows_guest_architecture::arm64;
        EXPECT_THROW(thread.setup_if_necessary(*this->machine_, this->process_), std::invalid_argument);
        EXPECT_FALSE(thread.setup_done);
        this->process_.native_architecture = architecture;
        thread.pending_status = STATUS_ACCESS_DENIED;
        thread.setup_if_necessary(*this->machine_, this->process_);
        EXPECT_TRUE(thread.setup_done);
        EXPECT_FALSE(thread.pending_status.has_value());
        EXPECT_EQ(this->abi_.return_value(*this->machine_), static_cast<uint64_t>(STATUS_ACCESS_DENIED));
    }

    TYPED_TEST(WindowsArchitectureTest, FunctionAndSyscallArgumentsCrossTheRegisterStackBoundary)
    {
        const auto stack = this->memory_.allocate_memory(0x1000, memory_permission::read_write);
        this->abi_.stack_pointer(*this->machine_, stack + 0x100);
        constexpr size_t register_count = is_arm64<TypeParam> ? 8 : 4;
        for (size_t i = 0; i < 12; ++i)
        {
            const auto value = 0xAABB000011220000ull + i;
            if (i < register_count)
            {
                this->set_register_argument(i, value);
            }
            else
            {
                const auto offset = (is_arm64<TypeParam> ? i - 8 : i + 1) * sizeof(uint64_t);
                this->machine_->write_memory(this->abi_.stack_pointer(*this->machine_) + offset, value);
            }
            EXPECT_EQ(this->abi_.function_argument(*this->machine_, i), value);
            EXPECT_EQ(this->abi_.syscall_argument(*this->machine_, i), !is_arm64<TypeParam> && i == 0 ? value + 1 : value);
        }
    }

    TYPED_TEST(WindowsArchitectureTest, SyscallIdentityAndPcAdjustmentAreArchitectureSpecific)
    {
        this->abi_.instruction_pointer(*this->machine_, code_base);
        if constexpr (is_arm64<TypeParam>)
        {
            constexpr uint32_t service = 0xF123;
            this->machine_->write_memory(code_base, uint32_t{0xD4000001 | (service << 5)});
            const auto raw_id = this->abi_.raw_syscall_id(*this->machine_);
            EXPECT_EQ(raw_id, service);
            EXPECT_EQ(this->abi_.syscall_id(raw_id), service);
            this->machine_->write_memory(code_base, uint32_t{0xD503201F});
            EXPECT_THROW(this->abi_.raw_syscall_id(*this->machine_), std::runtime_error);
        }
        else
        {
            constexpr uint32_t service = 0xC123;
            this->machine_->reg(x86_register::eax, service);
            const auto raw_id = this->abi_.raw_syscall_id(*this->machine_);
            EXPECT_EQ(raw_id, service);
            EXPECT_EQ(this->abi_.syscall_id(raw_id), 0x123u);
        }
        this->abi_.return_value(*this->machine_, 0x1122334455667788ull);
        EXPECT_EQ(this->abi_.return_value(*this->machine_), 0x1122334455667788ull);
        this->abi_.rewind_syscall(*this->machine_, code_base + 0x80);
        EXPECT_EQ(this->abi_.instruction_pointer(*this->machine_), code_base + 0x80 - this->abi_.syscall_abi().syscall_instruction_size());
        EXPECT_THROW(this->abi_.rewind_syscall(*this->machine_, 0), std::out_of_range);
    }

    TYPED_TEST(WindowsArchitectureTest, MessageWaitRestartPreservesNativeRegistersAndDeadline)
    {
        emulator_thread thread{this->memory_, this->process_, code_base + 0x300, 11, 0x10000, 0, 8, true};
        constexpr uint32_t service = 0x1234;
        this->abi_.instruction_pointer(*this->machine_, code_base);
        this->set_register_argument(0, 0x1122334455667788ull);
        if constexpr (is_arm64<TypeParam>)
        {
            this->machine_->write_memory(code_base, uint32_t{0xD4000001u | (service << 5)});
        }
        else
        {
            this->machine_->reg(x86_register::eax, service);
        }
        const auto argument = this->abi_.syscall_argument(*this->machine_, 0);
        const auto saved_register = this->abi_.return_value(*this->machine_);
        const auto deadline = this->clock_.steady_now() + std::chrono::milliseconds{100};
        thread.await_time = deadline;
        thread.await_msg_mask = QS_ALLINPUT;
        thread.remember_message_wait(*this->machine_, this->abi_);
        this->abi_.instruction_pointer(*this->machine_, code_base + this->abi_.syscall_abi().syscall_instruction_size());
        this->abi_.return_value(*this->machine_, STATUS_PENDING);

        thread.restart_message_wait(*this->machine_, this->abi_);

        EXPECT_EQ(this->abi_.instruction_pointer(*this->machine_), code_base);
        EXPECT_EQ(this->abi_.raw_syscall_id(*this->machine_), service);
        EXPECT_EQ(this->abi_.return_value(*this->machine_), saved_register);
        EXPECT_EQ(this->abi_.syscall_argument(*this->machine_, 0), argument);
        EXPECT_EQ(thread.message_wait_deadline, deadline);
        EXPECT_FALSE(thread.message_wait_return_register.has_value());
        EXPECT_FALSE(thread.await_time.has_value());
        EXPECT_FALSE(thread.pending_status.has_value());
    }

    namespace
    {
        class memory_file final : public i_file
        {
          public:
            explicit memory_file(std::vector<std::byte> data)
                : data_(std::move(data))
            {
            }

            size_t read(const std::span<std::byte> buffer) override
            {
                const auto count = std::min(buffer.size(), data_.size() - position_);
                std::memcpy(buffer.data(), data_.data() + position_, count);
                position_ += count;
                return count;
            }

            size_t write(std::span<const std::byte>) override
            {
                return 0;
            }

            bool flush() override
            {
                return true;
            }

            bool seek(const int64_t offset, const int origin) override
            {
                const auto base = origin == SEEK_SET ? 0 : (origin == SEEK_CUR ? static_cast<int64_t>(position_) : size());
                const auto target = base + offset;
                if (target < 0 || target > size())
                {
                    return false;
                }

                position_ = static_cast<size_t>(target);
                return true;
            }

            int64_t tell() override
            {
                return static_cast<int64_t>(position_);
            }

            int64_t size() override
            {
                return static_cast<int64_t>(data_.size());
            }

            bool resize(uint64_t) override
            {
                return false;
            }

            std::optional<struct compat_stat> stat() override
            {
                struct compat_stat result{};
                result.st_mode = S_IFREG;
                result.st_size = size();
                return result;
            }

          private:
            std::vector<std::byte> data_{};
            size_t position_{};
        };

        class memory_provider final : public mount_provider
        {
          public:
            explicit memory_provider(std::map<std::u16string, std::vector<std::byte>> files)
                : files_(std::move(files))
            {
            }

            std::optional<struct compat_stat> stat(const windows_path& path) const override
            {
                const auto file = files_.find(key(path));
                if (file == files_.end())
                {
                    return std::nullopt;
                }

                struct compat_stat result{};
                result.st_mode = S_IFREG;
                result.st_size = static_cast<int64_t>(file->second.size());
                return result;
            }

            std::vector<directory_entry> list(const windows_path&) const override
            {
                return {};
            }

            open_result open(const windows_path& path, const std::u16string&) override
            {
                const auto file = files_.find(key(path));
                if (file == files_.end())
                {
                    return {.status = STATUS_OBJECT_NAME_NOT_FOUND};
                }

                return {.file = std::make_unique<memory_file>(file->second), .status = STATUS_SUCCESS};
            }

            NTSTATUS create_directory(const windows_path&) override
            {
                return STATUS_ACCESS_DENIED;
            }

            NTSTATUS rename(const windows_path&, const windows_path&) override
            {
                return STATUS_ACCESS_DENIED;
            }

            NTSTATUS remove(const windows_path&) override
            {
                return STATUS_ACCESS_DENIED;
            }

          private:
            std::optional<std::filesystem::path> get_host_path(const windows_path&) const override
            {
                return std::nullopt;
            }

            static std::u16string key(const windows_path& path)
            {
                return utils::string::to_lower(path.u16string());
            }

            std::map<std::u16string, std::vector<std::byte>> files_{};
        };
    }

    TYPED_TEST(WindowsArchitectureTest, ModulesLoadFromProvidersWithoutHostPaths)
    {
        constexpr auto machine = TypeParam::pe_machine;
        this->file_sys_.mount(windows_path{"C:\\game"}, std::make_unique<memory_provider>(std::map<std::u16string, std::vector<std::byte>>{
                                                            {u"game.dll", make_test_image<uint64_t>(machine)},
                                                        }));

        const windows_path path{"C:\\game\\game.dll"};
        const auto detection = pe_architecture_detector::detect_from_file(this->file_sys_, path);
        ASSERT_TRUE(detection.is_valid());
        EXPECT_EQ(detection.machine, machine);

        const auto* mapped = this->modules_.map_module(path, this->log_);
        ASSERT_NE(mapped, nullptr);
        EXPECT_EQ(mapped->module_path, path);
        EXPECT_EQ(mapped->machine, static_cast<uint16_t>(machine));

        const windows_path differently_cased{"C:\\GAME\\Game.DLL"};
        EXPECT_EQ(this->modules_.map_module(differently_cased, this->log_), mapped);
        EXPECT_EQ(this->modules_.get_module_load_count_by_path(differently_cased), 1u);

        EXPECT_EQ(this->modules_.map_module(windows_path{"C:\\game\\missing.dll"}, this->log_), nullptr);
    }

    TYPED_TEST(WindowsArchitectureTest, WrongCpuArchitectureAndInvalidBootstrapAreRejected)
    {
        EXPECT_EQ(guest_architecture_of(*this->machine_), this->architecture);
        EXPECT_THROW(windows_abi_for(this->other_architecture).instruction_pointer(*this->machine_), std::invalid_argument);
        EXPECT_THROW(this->abi_.initialize_native_thread(*this->machine_, {}), std::invalid_argument);
        EXPECT_THROW(windows_abi_for(static_cast<windows_guest_architecture>(255)), std::invalid_argument);
    }

    TYPED_TEST(WindowsArchitectureTest, InstrumentationRedirectUsesOnlyTheSupportedNativeContract)
    {
        this->abi_.instruction_pointer(*this->machine_, code_base + 0x20);
        if constexpr (is_arm64<TypeParam>)
        {
            this->abi_.redirect_instrumentation_callback(*this->machine_, code_base + 0x60);
            EXPECT_EQ(this->abi_.instruction_pointer(*this->machine_), code_base + 0x60 - 4);
            EXPECT_EQ(this->machine_->reg(arm64_register::x16), code_base + 0x24);
        }
        else
        {
            this->abi_.redirect_instrumentation_callback(*this->machine_, code_base + 0x60);
            EXPECT_EQ(this->abi_.instruction_pointer(*this->machine_), code_base + 0x60 - 2);
            EXPECT_EQ(this->machine_->reg(x86_register::r10), code_base + 0x20);
        }
    }

    TYPED_TEST(WindowsArchitectureTest, ProcessThreadsHaveSeparateStacksAndSavedCpuState)
    {
        const auto first_handle = this->process_.create_thread(this->memory_, code_base + 0x300, 1, 0x10000, 0);
        const auto second_handle = this->process_.create_thread(this->memory_, code_base + 0x400, 2, 0x10000, 0);
        auto* first = this->process_.threads.get(first_handle);
        auto* second = this->process_.threads.get(second_handle);
        ASSERT_NE(first, nullptr);
        ASSERT_NE(second, nullptr);
        EXPECT_NE(first->id, second->id);
        EXPECT_NE(first->stack_base, second->stack_base);
        EXPECT_NE(first->teb64->value(), second->teb64->value());
        first->setup_if_necessary(*this->machine_, this->process_);
        const auto first_sp = this->abi_.stack_pointer(*this->machine_);
        this->abi_.return_value(*this->machine_, 0x1122334455667788ull);
        first->save(*this->machine_);
        second->setup_if_necessary(*this->machine_, this->process_);
        const auto second_sp = this->abi_.stack_pointer(*this->machine_);
        this->abi_.return_value(*this->machine_, 0x8877665544332211ull);
        second->save(*this->machine_);
        first->restore(*this->machine_);
        EXPECT_EQ(this->abi_.stack_pointer(*this->machine_), first_sp);
        EXPECT_EQ(this->abi_.return_value(*this->machine_), 0x1122334455667788ull);
        second->restore(*this->machine_);
        EXPECT_EQ(this->abi_.stack_pointer(*this->machine_), second_sp);
        EXPECT_EQ(this->abi_.return_value(*this->machine_), 0x8877665544332211ull);
        EXPECT_EQ(this->process_.find_thread_by_id(first->id), first);
        EXPECT_EQ(this->process_.find_thread_by_id(second->id), second);
    }

    TEST_F(Arm64WindowsArchitectureTest, ContextHonorsRequestedGroupsAndPreservesVectorAndDebugState)
    {
        const auto stack = memory_.allocate_memory(0x1000, memory_permission::read_write);
        abi_.stack_pointer(*machine_, stack + 0x800);
        abi_.instruction_pointer(*machine_, code_base);
        machine_->reg(arm64_register::x0, 0x1111111122222222ull);
        machine_->reg(arm64_register::x18, 0x3333333344444444ull);
        machine_->reg(arm64_register::fp, 0x5555555566666666ull);
        machine_->reg(arm64_register::lr, 0x7777777788888888ull);
        const arm64_context::vector_register vector{.Low = 0x123456789ABCDEF0ull, .High = 0x1234567812345678ll};
        machine_->write_register(arm64_register::q31, &vector, sizeof(vector));
        machine_->reg<uint32_t>(arm64_register::bcr0, 0);
        machine_->reg(arm64_register::bvr0, code_base + 0x80);
        arm64_context context{};
        context.ContextFlags = arm64_context_all;
        cpu_context::save(*machine_, context);
        EXPECT_EQ(context.X[0], 0x1111111122222222ull);
        EXPECT_EQ(context.X[18], 0x3333333344444444ull);
        EXPECT_EQ(context.X[29], 0x5555555566666666ull);
        EXPECT_EQ(context.X[30], 0x7777777788888888ull);
        EXPECT_EQ(context.V[31].Low, vector.Low);
        EXPECT_EQ(context.V[31].High, vector.High);
        EXPECT_EQ(context.Bvr[0], code_base + 0x80);
        machine_->reg(arm64_register::x0, 0);
        machine_->reg(arm64_register::fp, 0);
        machine_->reg(arm64_register::bvr0, 0);
        cpu_context::restore(*machine_, context);
        EXPECT_EQ(machine_->reg(arm64_register::x0), context.X[0]);
        EXPECT_EQ(machine_->reg(arm64_register::fp), context.X[29]);
        EXPECT_EQ(machine_->reg(arm64_register::bvr0), context.Bvr[0]);
        context.ContextFlags = arm64_context_integer;
        context.X[0] = 42;
        context.Sp = 3;
        context.Pc = 5;
        context.X[29] = 0;
        cpu_context::restore(*machine_, context);
        EXPECT_EQ(machine_->reg(arm64_register::x0), 42u);
        EXPECT_EQ(machine_->reg(arm64_register::fp), 0x5555555566666666ull);
        EXPECT_EQ(abi_.stack_pointer(*machine_), stack + 0x800);
        context.ContextFlags = arm64_context_control;
        EXPECT_THROW(cpu_context::restore(*machine_, context), std::invalid_argument);
        context.ContextFlags = CONTEXT64_ALL;
        EXPECT_THROW(cpu_context::save(*machine_, context), std::invalid_argument);
    }
}
