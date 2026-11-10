// Feasibility spike for running FEX-translated x64 code inside a Hypervisor.framework VM.
// FEX compiles on the host; its dispatcher and generated code run at EL0 in the VM. Calls from generated code into
// host C++ are redirected to unmapped sentinel addresses, so they arrive as instruction aborts and are performed on
// the host with the vCPU's argument registers.

#include <hvf_arm64_emulator.hpp>
#include <memory_manager.hpp>

#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/HostFeatures.h>
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/Utils/AllocatorHooks.h>
#include <FEXCore/Utils/LogManager.h>

#include <gtest/gtest.h>

#include <sys/mman.h>
#include <sys/sysctl.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>

namespace sogen::test
{
    namespace
    {
        constexpr uint64_t host_call_base = 0x7F0000000000ull;
        constexpr uint64_t host_block_size = 0x4000;

        enum class host_call : uint32_t
        {
            return_to_host,
            compile_block,
            compile_single_step,
            sleep_thread,
            syscall,
            exit_function_link,
            cpuid,
            xcr,
            remove_code_entry,
            mono_backpatcher_write,
            ludiv,
            ldiv,
            count,
        };

        uint64_t sentinel(const host_call call)
        {
            return host_call_base + 0x10 * static_cast<uint64_t>(call);
        }

        // Generated code never runs on the host, and MAP_JIT memory must never reach hv_vm_map (the page table
        // monitor panics the machine), so FEX's executable buffers are plain data; the VM's stage-1 tables grant
        // execute.
        void* data_only_mmap(void* address, const size_t length, const int protection, const int flags, const int fd, const off_t offset)
        {
            return ::mmap(address, length, protection & ~PROT_EXEC, flags & ~MAP_JIT, fd, offset);
        }

        bool sysctl_flag(const char* name)
        {
            int32_t value{};
            size_t size = sizeof(value);
            return ::sysctlbyname(name, &value, &size, nullptr, 0) == 0 && value != 0;
        }

        FEXCore::HostFeatures apple_host_features()
        {
            FEXCore::HostFeatures features{};
            features.DCacheLineSize = 64;
            features.ICacheLineSize = 64;
            features.SupportsCacheMaintenanceOps = true;
            features.SupportsAES = sysctl_flag("hw.optional.arm.FEAT_AES");
            features.SupportsCRC = sysctl_flag("hw.optional.arm.FEAT_CRC32");
            features.SupportsAtomics = sysctl_flag("hw.optional.arm.FEAT_LSE");
            features.SupportsRCPC = sysctl_flag("hw.optional.arm.FEAT_LRCPC");
            features.SupportsFlagM = sysctl_flag("hw.optional.arm.FEAT_FlagM");
            features.SupportsFlagM2 = sysctl_flag("hw.optional.arm.FEAT_FlagM2");
            features.SupportsFRINTTS = sysctl_flag("hw.optional.arm.FEAT_FRINTTS");
            features.CPUMIDRs.assign(1, 0u);
            return features;
        }

        class spike_syscall_handler final : public FEXCore::HLE::SyscallHandler
        {
          public:
            spike_syscall_handler(const uint64_t code, const uint64_t size)
                : code_(code),
                  size_(size)
            {
                this->OSABI = FEXCore::HLE::SyscallOSABI::OS_GENERIC;
            }

            uint64_t HandleSyscall(FEXCore::Core::CpuStateFrame*, FEXCore::HLE::SyscallArguments*) override
            {
                throw std::logic_error("Guest syscalls leave the VM before reaching FEX's handler");
            }

            FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(FEXCore::Core::InternalThreadState*,
                                                                        const uint64_t address) override
            {
                if (address >= code_ && address < code_ + size_)
                {
                    return {.Base = code_, .Size = size_, .Writable = false};
                }
                return {};
            }

            std::optional<FEXCore::ExecutableFileSectionInfo> LookupExecutableFileSection(FEXCore::Core::InternalThreadState*,
                                                                                          uint64_t) override
            {
                return std::nullopt;
            }

          private:
            uint64_t code_{};
            uint64_t size_{};
        };

        struct anonymous_memory
        {
            std::byte* memory{};
            size_t size{};

            explicit anonymous_memory(const size_t length)
                : size(length)
            {
                auto* result = ::mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
                if (result == MAP_FAILED)
                {
                    throw std::runtime_error("mmap failed");
                }
                memory = static_cast<std::byte*>(result);
            }

            ~anonymous_memory()
            {
                ::munmap(memory, size);
            }

            anonymous_memory(const anonymous_memory&) = delete;
            anonymous_memory& operator=(const anonymous_memory&) = delete;
            anonymous_memory(anonymous_memory&&) = delete;
            anonymous_memory& operator=(anonymous_memory&&) = delete;

            uint64_t address() const
            {
                return reinterpret_cast<uint64_t>(memory);
            }
        };

        arm64_register general(const int index)
        {
            return static_cast<arm64_register>(static_cast<int>(arm64_register::x0) + index);
        }
    }

    TEST(FexInHvfSpike, X64BlockCompiledByFexRunsInsideTheVmAndExitsToTheHost)
    {
        if (!hvf::is_available())
        {
            GTEST_SKIP() << "Hypervisor.framework is not available to this process";
        }

        LogMan::Msg::InstallHandler([](LogMan::DebugLevels level, const char* message) {
            std::fprintf(stderr, "[FEXCore] %s: %s\n", LogMan::DebugLevelStr(level), message);
        });
        FEXCore::Allocator::mmap = &data_only_mmap;

        // mov rax, 1; add rax, 2; syscall; ret
        constexpr std::array<uint8_t, 14> code{0x48, 0xC7, 0xC0, 0x01, 0x00, 0x00, 0x00, 0x48, 0x83, 0xC0, 0x02, 0x0F, 0x05, 0xC3};
        constexpr uint64_t syscall_offset = 11;
        anonymous_memory guest{0x10000};
        anonymous_memory callret_stack{FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE};
        anonymous_memory vm_stack{0x40000};
        std::memcpy(guest.memory, code.data(), code.size());
        const auto guest_stack_top = guest.address() + guest.size - 0x100;

        FEXCore::Config::Initialize();
        FEXCore::Config::Load();
        FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, "1");
        auto context = FEXCore::Context::Context::CreateNewContext(apple_host_features());
        spike_syscall_handler syscall_handler{guest.address(), guest.size};
        FEXCore::SignalDelegator signal_delegator{};
        context->SetSyscallHandler(&syscall_handler);
        context->SetSignalDelegator(&signal_delegator);
        context->InitCore();

        // Windows x64 user selectors: 0x2B flat data, 0x33 64-bit code. The decoder reads CS from this table to
        // choose 64-bit decoding.
        alignas(16) std::array<uint64_t, 8> gdt{};
        gdt[0x2B >> 3] = 0x00CFF3000000FFFFull;
        gdt[0x33 >> 3] = 0x00AFFB000000FFFFull;

        auto* thread = context->CreateThread(guest.address(), guest_stack_top);
        auto* frame = thread->CurrentFrame;
        frame->State.segment_arrays[0] = reinterpret_cast<FEXCore::Core::CPUState::gdt_segment*>(gdt.data());
        frame->State.cs_idx = 0x33;
        frame->State.ss_idx = 0x2B;
        frame->State.ds_idx = 0x2B;
        frame->State.es_idx = 0x2B;
        thread->CallRetStackBase = callret_stack.memory;
        frame->State.callret_sp = callret_stack.address() + callret_stack.size / 4;

        std::array<uint64_t, static_cast<size_t>(host_call::count)> host_functions{};
        const auto redirect = [&](uint64_t& slot, const host_call call) {
            host_functions[static_cast<size_t>(call)] = slot;
            slot = sentinel(call);
        };
        auto& pointers = frame->Pointers;
        redirect(pointers.SyscallHandlerFunc, host_call::syscall);
        redirect(pointers.ExitFunctionLink, host_call::exit_function_link);
        redirect(pointers.CPUIDFunction, host_call::cpuid);
        redirect(pointers.XCRFunction, host_call::xcr);
        redirect(pointers.ThreadRemoveCodeEntryFromJIT, host_call::remove_code_entry);
        redirect(pointers.MonoBackpatcherWrite, host_call::mono_backpatcher_write);
        redirect(pointers.LUDIV, host_call::ludiv);
        redirect(pointers.LDIV, host_call::ldiv);

        // The dispatcher ends with its literal pool: CTX, SleepThread, CompileBlock, CompileSingleStep.
        const auto& dispatcher = signal_delegator.GetConfig();
        auto* literals = reinterpret_cast<uint64_t*>(dispatcher.DispatcherEnd) - 4;
        ASSERT_EQ(literals[0], reinterpret_cast<uint64_t>(context.get())) << "The dispatcher literal pool was not found";
        redirect(literals[1], host_call::sleep_thread);
        redirect(literals[2], host_call::compile_block);
        redirect(literals[3], host_call::compile_single_step);

        // Declared after FEX's objects so the VM is destroyed before the memory it maps is released.
        auto vm = hvf::create_arm64_emulator(1);
        memory_manager memory{*vm};
        for (const auto* region : {&guest, &callret_stack, &vm_stack})
        {
            ASSERT_TRUE(memory.allocate_host_memory_at(region->address(), region->size, region->memory, memory_permission::all));
        }

        std::map<host_call, int> calls{};
        int demand_mapped_blocks{};
        bool finished{};
        std::optional<uint64_t> rax_at_syscall{};
        std::optional<uint64_t> rip_at_syscall{};
        std::string failure{};

        vm->hook_memory_violation([&](cpu_interface& raw, const uint64_t address, size_t, const memory_operation operation,
                                      const memory_violation_type type) {
            auto& cpu = dynamic_cast<arm64_cpu&>(raw);
            if (operation == memory_operation::exec && address >= host_call_base && address < sentinel(host_call::count))
            {
                const auto call = static_cast<host_call>((address - host_call_base) / 0x10);
                ++calls[call];
                const auto return_address = cpu.reg(arm64_register::x30);
                if (call == host_call::return_to_host)
                {
                    finished = true;
                    cpu.stop();
                    return memory_violation_continuation::resume;
                }
                if (call == host_call::syscall)
                {
                    rax_at_syscall = frame->State.gregs[FEXCore::X86State::REG_RAX];
                    rip_at_syscall = frame->State.rip;
                    finished = true;
                    cpu.reg(arm64_register::pc, return_address);
                    cpu.stop();
                    return memory_violation_continuation::resume;
                }
                using host_function = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
                const auto function = reinterpret_cast<host_function>(host_functions[static_cast<size_t>(call)]);
                const auto result = function(cpu.reg(general(0)), cpu.reg(general(1)), cpu.reg(general(2)), cpu.reg(general(3)),
                                             cpu.reg(general(4)), cpu.reg(general(5)), cpu.reg(general(6)), cpu.reg(general(7)));
                cpu.reg(arm64_register::x0, result);
                cpu.reg(arm64_register::pc, return_address);
                return memory_violation_continuation::resume;
            }

            // FEX's own memory (thread state, lookup caches, code buffer) is mapped into the VM where it lives on
            // the host, the first time generated code touches it.
            const auto block = address & ~(host_block_size - 1);
            if (type == memory_violation_type::unmapped)
            {
                bool mapped{};
                try
                {
                    mapped = memory.allocate_host_memory_at(block, host_block_size, reinterpret_cast<void*>(block), memory_permission::all);
                }
                catch (const std::exception& e)
                {
                    failure = e.what();
                }
                if (mapped)
                {
                    ++demand_mapped_blocks;
                    return memory_violation_continuation::resume;
                }
            }
            char message[160]{};
            std::snprintf(message, sizeof(message), "unexpected %s fault at 0x%llx (PC 0x%llx) %s",
                          operation == memory_operation::exec ? "exec" : "data", static_cast<unsigned long long>(address),
                          static_cast<unsigned long long>(cpu.read_instruction_pointer()), failure.c_str());
            failure = message;
            finished = true;
            cpu.stop();
            return memory_violation_continuation::resume;
        });
        vm->hook_interrupt([&](cpu_interface& raw, const int exception_class) {
            char message[96]{};
            std::snprintf(message, sizeof(message), "guest exception class 0x%x at PC 0x%llx", exception_class,
                          static_cast<unsigned long long>(dynamic_cast<arm64_cpu&>(raw).read_instruction_pointer()));
            failure = message;
            finished = true;
            raw.stop();
        });

        vm->reg(arm64_register::pc, dispatcher.DispatcherBegin);
        vm->reg(arm64_register::x0, reinterpret_cast<uint64_t>(frame));
        vm->reg(arm64_register::x1, 0);
        vm->reg(arm64_register::x30, sentinel(host_call::return_to_host));
        vm->reg(arm64_register::sp, vm_stack.address() + vm_stack.size - 0x10);

        for (int run = 0; run < 16 && !finished; ++run)
        {
            vm->start();
        }

        std::printf("host calls:");
        for (const auto& [call, count] : calls)
        {
            std::printf(" %u=%d", static_cast<unsigned>(call), count);
        }
        std::printf("; demand-mapped 16 KiB blocks: %d\n", demand_mapped_blocks);

        EXPECT_TRUE(failure.empty()) << failure;
        EXPECT_GE(calls[host_call::compile_block], 1) << "No code-cache miss reached the host";
        ASSERT_TRUE(rax_at_syscall.has_value()) << "The guest syscall never exited to the host";
        EXPECT_EQ(*rax_at_syscall, 3u);
        EXPECT_EQ(rip_at_syscall, guest.address() + syscall_offset);

        context->DestroyThread(thread);
    }
}
