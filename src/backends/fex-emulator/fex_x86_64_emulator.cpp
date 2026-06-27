// _GNU_SOURCE is required for mremap() (used to alias host memory into the guest address space).
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#define FEX_EMULATOR_IMPL
#include "fex_x86_64_emulator.hpp"
#include "fex_x86_64_common.hpp"

// ---------------------------------------------------------------------------------------------------
// FEX-Emu backend (basic support).
//
// FEX (https://fex-emu.com) is an in-process x86/x86-64 -> AArch64 binary translator. Unlike the
// Unicorn/Icicle/KVM backends, FEX does NOT manage a sandboxed guest address space: it executes the
// translated guest inside the *host* process and treats guest virtual addresses as host virtual
// addresses (a 1:1 mapping). Consequences that shape this backend:
//
//   * map_memory() is a real mmap(MAP_FIXED) at the guest address; read/write_memory() is a direct
//     host memcpy once the range is known to be mapped.
//   * The guest runs natively (JITed), so - exactly like the KVM backend - there is no per-access or
//     per-instruction instrumentation point. Memory/execution/basic-block hooks are accepted for API
//     compatibility but never fire.
//   * Guest `syscall` instructions are routed back to sogen through a FEXCore::HLE::SyscallHandler,
//     which invokes the registered syscall instruction-hook. That is what lets the Windows emulation
//     layer service NT syscalls.
//
// This file targets AArch64 Linux/Android hosts (FEX only JITs to ARM64). It is written against the
// FEXCore embedding API; spots that depend on FEX-version-specific internals are marked TODO(fex).
// ---------------------------------------------------------------------------------------------------

#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <utils/object.hpp>

// FEXCore embedding headers. These are only available when building against a FEX checkout/install;
// the CMake glue gates this whole target behind SOGEN_ENABLE_FEX so non-ARM builds never reach here.
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/HostFeatures.h>
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/HLE/SyscallHandler.h>

namespace sogen::fex
{
    namespace
    {
        constexpr size_t page_size = 0x1000;

        bool is_page_aligned(const uint64_t value)
        {
            return (value & (page_size - 1)) == 0;
        }

        int to_prot(const memory_permission permissions)
        {
            int prot = PROT_NONE;
            if ((permissions & memory_permission::read) != memory_permission::none)
            {
                prot |= PROT_READ;
            }
            if ((permissions & memory_permission::write) != memory_permission::none)
            {
                prot |= PROT_WRITE;
            }
            if ((permissions & memory_permission::exec) != memory_permission::none)
            {
                prot |= PROT_EXEC;
            }
            return prot;
        }

        struct mapped_region
        {
            size_t size = 0;
            memory_permission permissions = memory_permission::none;
            bool owned = true; // false for map_host_memory aliases we must not munmap
        };

        struct hook_entry
        {
            x86_hookable_instructions type = x86_hookable_instructions::invalid;
            instruction_hook_callback callback;
        };
    }

    class fex_x86_64_emulator;

    // -----------------------------------------------------------------------------------------------
    // The syscall handler bridges FEX's guest `syscall` exits to the registered instruction hook.
    // Method bodies are defined out of line (after fex_x86_64_emulator is complete) since they touch
    // the emulator's internals.
    // -----------------------------------------------------------------------------------------------
    class fex_syscall_handler final : public FEXCore::HLE::SyscallHandler
    {
      public:
        explicit fex_syscall_handler(fex_x86_64_emulator& emulator)
            : emulator_(emulator)
        {
        }

        uint64_t HandleSyscall(FEXCore::Core::CpuStateFrame* frame, FEXCore::HLE::SyscallArguments* args) override;
        FEXCore::HLE::SyscallABI GetSyscallABI(uint64_t syscall) override;
        FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(FEXCore::Core::InternalThreadState* thread, uint64_t address) override;

      private:
        fex_x86_64_emulator& emulator_;
    };

    // -----------------------------------------------------------------------------------------------
    // The emulator itself.
    // -----------------------------------------------------------------------------------------------
    class fex_x86_64_emulator final : public x86_64_emulator
    {
      public:
        fex_x86_64_emulator()
        {
            this->initialize_context();
        }

        ~fex_x86_64_emulator() override
        {
            utils::reset_object_with_delayed_destruction(this->memory_read_hooks_);
            utils::reset_object_with_delayed_destruction(this->memory_write_hooks_);
            utils::reset_object_with_delayed_destruction(this->memory_execution_hooks_);
            utils::reset_object_with_delayed_destruction(this->memory_violation_hooks_);
            utils::reset_object_with_delayed_destruction(this->interrupt_hooks_);
            utils::reset_object_with_delayed_destruction(this->basic_block_hooks_);
            utils::reset_object_with_delayed_destruction(this->instruction_hooks_);

            if (this->thread_ != nullptr && this->context_)
            {
                this->context_->DestroyThread(this->thread_);
                this->thread_ = nullptr;
            }

            // Release everything we mmap'd into the (host == guest) address space.
            for (const auto& [address, region] : this->regions_)
            {
                if (region.owned)
                {
                    ::munmap(reinterpret_cast<void*>(address), region.size);
                }
            }
        }

        // --[ cpu_interface ]------------------------------------------------------------------------

        bool read_descriptor_table(int reg, descriptor_table_register& table) override
        {
            // FEX is a user-mode emulator: there is no real IDT, and the GDT is synthesized internally.
            // Only report the GDT base we were handed via load_gdt(); everything else is unsupported.
            if (reg == static_cast<int>(x86_register::gdtr))
            {
                table.base = this->gdt_base_;
                table.limit = this->gdt_limit_;
                return true;
            }
            return false;
        }

        void start(size_t count) override
        {
            if (count != 0)
            {
                // FEX has CompileRIPCount() for bounded execution, but wiring exact instruction counts
                // through the JIT exit path is non-trivial; match the KVM backend and refuse for now.
                throw std::runtime_error("FEX backend does not support exact instruction counts yet");
            }

            if (this->thread_ == nullptr)
            {
                this->create_thread();
            }

            this->stop_requested_ = false;

            // ExecuteThread runs the translated guest until the thread is asked to stop (which the
            // syscall bridge does when a hook calls stop()), or the guest faults/exits.
            this->context_->ExecuteThread(this->thread_);
        }

        void stop() override
        {
            this->stop_requested_ = true;
            this->request_thread_stop();
        }

        size_t read_raw_register(int reg, void* value, size_t size) override
        {
            const auto xreg = static_cast<x86_register>(reg);
            const auto mapping = detail::map_register(xreg);
            auto& state = this->cpu_state();

            switch (mapping.kind)
            {
            case detail::register_kind::gpr: {
                uint64_t raw = state.gregs[mapping.gpr.index] >> (mapping.gpr.byte_offset * 8);
                std::memcpy(value, &raw, (std::min)(size, mapping.gpr.width));
                return size;
            }
            case detail::register_kind::rip:
                std::memcpy(value, &state.rip, (std::min)(size, sizeof(state.rip)));
                return size;
            case detail::register_kind::flags: {
                const uint64_t rflags = this->read_rflags();
                std::memcpy(value, &rflags, (std::min)(size, sizeof(rflags)));
                return size;
            }
            case detail::register_kind::xmm:
                // Low 128 bits of the (possibly AVX) vector register.
                std::memcpy(value, &state.xmm.avx.data[mapping.index][0], (std::min)(size, size_t{16}));
                return size;
            case detail::register_kind::mm:
                std::memcpy(value, &state.mm[mapping.index][0], (std::min)(size, size_t{16}));
                return size;
            case detail::register_kind::mxcsr:
                std::memcpy(value, &state.mxcsr, (std::min)(size, sizeof(state.mxcsr)));
                return size;
            case detail::register_kind::fcw:
                std::memcpy(value, &state.FCW, (std::min)(size, sizeof(state.FCW)));
                return size;
            case detail::register_kind::fs_base:
                std::memcpy(value, &state.fs_cached, (std::min)(size, sizeof(state.fs_cached)));
                return size;
            case detail::register_kind::gs_base:
                std::memcpy(value, &state.gs_cached, (std::min)(size, sizeof(state.gs_cached)));
                return size;
            case detail::register_kind::segment: {
                const uint16_t selector = this->segment_selector(mapping.index);
                std::memcpy(value, &selector, (std::min)(size, sizeof(selector)));
                return size;
            }
            case detail::register_kind::fsw:
            case detail::register_kind::unsupported:
            default:
                // Unknown/unsupported register: report zeroed value rather than throwing, matching the
                // lenient behavior of the other backends for rarely-used registers.
                std::memset(value, 0, size);
                return size;
            }
        }

        size_t write_raw_register(int reg, const void* value, size_t size) override
        {
            const auto xreg = static_cast<x86_register>(reg);
            const auto mapping = detail::map_register(xreg);
            auto& state = this->cpu_state();

            switch (mapping.kind)
            {
            case detail::register_kind::gpr: {
                auto& slot = state.gregs[mapping.gpr.index];
                if (mapping.gpr.width == 8)
                {
                    std::memcpy(&slot, value, sizeof(slot));
                }
                else if (mapping.gpr.zero_extend_32)
                {
                    uint32_t v = 0;
                    std::memcpy(&v, value, sizeof(v));
                    slot = v; // 32-bit writes clear the high 32 bits
                }
                else
                {
                    uint64_t incoming = 0;
                    std::memcpy(&incoming, value, mapping.gpr.width);
                    const auto shift = mapping.gpr.byte_offset * 8;
                    const uint64_t mask = ((1ULL << (mapping.gpr.width * 8)) - 1) << shift;
                    slot = (slot & ~mask) | ((incoming << shift) & mask);
                }
                return size;
            }
            case detail::register_kind::rip:
                std::memcpy(&state.rip, value, (std::min)(size, sizeof(state.rip)));
                return size;
            case detail::register_kind::flags: {
                uint64_t rflags = 0;
                std::memcpy(&rflags, value, (std::min)(size, sizeof(rflags)));
                this->write_rflags(rflags);
                return size;
            }
            case detail::register_kind::xmm:
                std::memcpy(&state.xmm.avx.data[mapping.index][0], value, (std::min)(size, size_t{16}));
                return size;
            case detail::register_kind::mm:
                std::memcpy(&state.mm[mapping.index][0], value, (std::min)(size, size_t{16}));
                return size;
            case detail::register_kind::mxcsr:
                std::memcpy(&state.mxcsr, value, (std::min)(size, sizeof(state.mxcsr)));
                return size;
            case detail::register_kind::fcw:
                std::memcpy(&state.FCW, value, (std::min)(size, sizeof(state.FCW)));
                return size;
            case detail::register_kind::fs_base:
                std::memcpy(&state.fs_cached, value, (std::min)(size, sizeof(state.fs_cached)));
                return size;
            case detail::register_kind::gs_base:
                std::memcpy(&state.gs_cached, value, (std::min)(size, sizeof(state.gs_cached)));
                return size;
            case detail::register_kind::segment:
                this->set_segment_selector(mapping.index, value, size);
                return size;
            case detail::register_kind::fsw:
            case detail::register_kind::unsupported:
            default:
                return size;
            }
        }

        std::vector<std::byte> save_registers() const override
        {
            // The whole architectural state lives in a single CPUState struct; snapshot it verbatim.
            const auto& state = this->cpu_state();
            std::vector<std::byte> data(sizeof(FEXCore::Core::CPUState));
            std::memcpy(data.data(), &state, sizeof(state));
            return data;
        }

        void restore_registers(const std::vector<std::byte>& register_data) override
        {
            if (register_data.size() != sizeof(FEXCore::Core::CPUState))
            {
                throw std::runtime_error("FEX register snapshot has unexpected size");
            }
            std::memcpy(&this->cpu_state(), register_data.data(), sizeof(FEXCore::Core::CPUState));
        }

        bool has_violation() const override
        {
            return false;
        }

        bool supports_instruction_counting() const override
        {
            return false;
        }

        // --[ emulator ]-----------------------------------------------------------------------------

        std::string get_name() const override
        {
            return "FEX";
        }

        void serialize_state(utils::buffer_serializer& buffer, bool /*is_snapshot*/) const override
        {
            buffer.write_vector(this->save_registers());
            // TODO(fex): a full snapshot should also persist the mapped-memory layout and contents so a
            // restore can re-mmap and refill the (host == guest) address space. Registers-only for now.
        }

        void deserialize_state(utils::buffer_deserializer& buffer, bool /*is_snapshot*/) override
        {
            this->restore_registers(buffer.read_vector<std::byte>());
        }

        // --[ x86_emulator ]-------------------------------------------------------------------------

        void set_segment_base(x86_register base, pointer_type value) override
        {
            auto& state = this->cpu_state();
            if (base == x86_register::fs || base == x86_register::fs_base)
            {
                state.fs_cached = value;
            }
            else if (base == x86_register::gs || base == x86_register::gs_base)
            {
                state.gs_cached = value;
            }
        }

        pointer_type get_segment_base(x86_register base) override
        {
            const auto& state = this->cpu_state();
            if (base == x86_register::fs || base == x86_register::fs_base)
            {
                return state.fs_cached;
            }
            if (base == x86_register::gs || base == x86_register::gs_base)
            {
                return state.gs_cached;
            }
            return 0;
        }

        void load_gdt(pointer_type address, uint32_t limit) override
        {
            // FEX builds its own segment descriptor arrays; we only remember the base/limit so callers
            // querying gdtr (see read_descriptor_table) get a consistent answer.
            this->gdt_base_ = address;
            this->gdt_limit_ = limit;
        }

        // --[ memory_interface (public) ]------------------------------------------------------------

        void read_memory(uint64_t address, void* data, size_t size) const override
        {
            if (!this->try_read_memory(address, data, size))
            {
                throw std::runtime_error("Failed to read FEX guest memory");
            }
        }

        bool try_read_memory(uint64_t address, void* data, size_t size) const override
        {
            if (!this->is_range_mapped(address, size))
            {
                return false;
            }
            // Guest VA == host VA under FEX, so the guest pointer is directly dereferenceable.
            std::memcpy(data, reinterpret_cast<const void*>(address), size);
            return true;
        }

        void write_memory(uint64_t address, const void* data, size_t size) override
        {
            if (!this->try_write_memory(address, data, size))
            {
                throw std::runtime_error("Failed to write FEX guest memory");
            }
        }

        bool try_write_memory(uint64_t address, const void* data, size_t size) override
        {
            if (!this->is_range_mapped(address, size))
            {
                return false;
            }
            std::memcpy(reinterpret_cast<void*>(address), data, size);
            // Writing to a mapped region may overwrite already-translated code; drop FEX's cache for it.
            this->invalidate_code_range(address, size);
            return true;
        }

        // --[ hook_interface ]-----------------------------------------------------------------------
        //
        // Like the KVM backend, FEX runs the guest natively, so fine-grained memory/execution/basic-
        // block hooks cannot fire. They are accepted (and tracked, so delete_hook works) for API
        // compatibility. Only instruction hooks for `syscall` are actually wired (see the syscall
        // bridge). cpuid/rdtsc could later be wired through FEX's CPUID/TSC override hooks.

        emulator_hook* hook_memory_execution(memory_execution_hook_callback callback) override
        {
            auto* hook = this->make_hook();
            this->memory_execution_hooks_[hook] = std::move(callback);
            return hook;
        }

        emulator_hook* hook_memory_execution(uint64_t /*address*/, memory_execution_hook_callback callback) override
        {
            auto* hook = this->make_hook();
            this->memory_execution_hooks_[hook] = std::move(callback);
            return hook;
        }

        emulator_hook* hook_memory_range_execution(uint64_t /*address*/, uint64_t /*size*/,
                                                   memory_execution_hook_callback callback) override
        {
            auto* hook = this->make_hook();
            this->memory_execution_hooks_[hook] = std::move(callback);
            return hook;
        }

        emulator_hook* hook_memory_read(uint64_t /*address*/, uint64_t /*size*/, memory_access_hook_callback callback) override
        {
            auto* hook = this->make_hook();
            this->memory_read_hooks_[hook] = std::move(callback);
            return hook;
        }

        emulator_hook* hook_memory_write(uint64_t /*address*/, uint64_t /*size*/, memory_access_hook_callback callback) override
        {
            auto* hook = this->make_hook();
            this->memory_write_hooks_[hook] = std::move(callback);
            return hook;
        }

        emulator_hook* hook_instruction(int instruction_type, instruction_hook_callback callback) override
        {
            auto* hook = this->make_hook();
            auto& entry = this->instruction_hooks_[hook];
            entry.type = static_cast<x86_hookable_instructions>(instruction_type);
            entry.callback = std::move(callback);
            if (entry.type == x86_hookable_instructions::syscall)
            {
                this->syscall_hook_ = &entry;
            }
            return hook;
        }

        emulator_hook* hook_interrupt(interrupt_hook_callback callback) override
        {
            auto* hook = this->make_hook();
            this->interrupt_hooks_[hook] = std::move(callback);
            return hook;
        }

        emulator_hook* hook_memory_violation(memory_violation_hook_callback callback) override
        {
            auto* hook = this->make_hook();
            this->memory_violation_hooks_[hook] = std::move(callback);
            return hook;
        }

        emulator_hook* hook_basic_block(basic_block_hook_callback callback) override
        {
            auto* hook = this->make_hook();
            this->basic_block_hooks_[hook] = std::move(callback);
            return hook;
        }

        void delete_hook(emulator_hook* hook) override
        {
            if (this->syscall_hook_ != nullptr)
            {
                const auto it = this->instruction_hooks_.find(hook);
                if (it != this->instruction_hooks_.end() && &it->second == this->syscall_hook_)
                {
                    this->syscall_hook_ = nullptr;
                }
            }

            this->instruction_hooks_.erase(hook);
            this->interrupt_hooks_.erase(hook);
            this->memory_read_hooks_.erase(hook);
            this->memory_write_hooks_.erase(hook);
            this->memory_execution_hooks_.erase(hook);
            this->memory_violation_hooks_.erase(hook);
            this->basic_block_hooks_.erase(hook);
        }

        bool supports_global_memory_execution_hooks() const override
        {
            // Native execution: global execution hooks would require single-stepping the JIT.
            return false;
        }

      private:
        friend class fex_syscall_handler;

        // --[ memory_interface (private) ]-----------------------------------------------------------

        void map_mmio(uint64_t /*address*/, size_t /*size*/, mmio_read_callback /*read_cb*/, mmio_write_callback /*write_cb*/) override
        {
            // FEX has no trap-and-emulate path for arbitrary MMIO since the guest runs natively. This
            // would need a no-access guard page plus a SIGSEGV handler that decodes the faulting access.
            throw std::runtime_error("MMIO mapping is not supported by the FEX backend");
        }

        void map_memory(uint64_t address, size_t size, memory_permission permissions) override
        {
            if (!is_page_aligned(address) || !is_page_aligned(size))
            {
                throw std::runtime_error("FEX memory mappings must be page aligned");
            }

            // Place the guest pages at their guest address in the host address space (guest VA == host VA).
            void* result = ::mmap(reinterpret_cast<void*>(address), size, to_prot(permissions),
                                  MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
            if (result == MAP_FAILED || reinterpret_cast<uint64_t>(result) != address)
            {
                throw std::runtime_error("FEX backend failed to map guest memory at requested address");
            }

            this->regions_[address] = mapped_region{.size = size, .permissions = permissions, .owned = true};
            this->mark_executable_range(address, size, permissions);
        }

        void map_host_memory(uint64_t address, size_t size, void* host_pointer, memory_permission permissions) override
        {
            if (!is_page_aligned(address) || !is_page_aligned(size))
            {
                throw std::runtime_error("FEX host memory mappings must be page aligned");
            }

            // Move the existing host mapping so the guest sees it at `address` without a staging copy.
            // mremap with MREMAP_FIXED relocates the VMA; the caller must treat host_pointer as moved.
            void* result = ::mremap(host_pointer, size, size, MREMAP_MAYMOVE | MREMAP_FIXED, reinterpret_cast<void*>(address));
            if (result == MAP_FAILED || reinterpret_cast<uint64_t>(result) != address)
            {
                throw std::runtime_error("FEX backend failed to alias host memory into the guest");
            }

            ::mprotect(reinterpret_cast<void*>(address), size, to_prot(permissions));
            // owned=false: the memory belongs to the caller; we must not munmap it on teardown.
            this->regions_[address] = mapped_region{.size = size, .permissions = permissions, .owned = false};
            this->mark_executable_range(address, size, permissions);
        }

        void unmap_memory(uint64_t address, size_t size) override
        {
            ::munmap(reinterpret_cast<void*>(address), size);
            this->invalidate_code_range(address, size);
            this->regions_.erase(address);
        }

        void apply_memory_protection(uint64_t address, size_t size, memory_permission permissions) override
        {
            if (::mprotect(reinterpret_cast<void*>(address), size, to_prot(permissions)) != 0)
            {
                throw std::runtime_error("FEX backend failed to change memory protection");
            }

            const auto it = this->regions_.find(address);
            if (it != this->regions_.end())
            {
                it->second.permissions = permissions;
            }

            // Permission changes can expose/retract executable code; keep FEX's translation cache honest.
            this->invalidate_code_range(address, size);
            this->mark_executable_range(address, size, permissions);
        }

        // --[ region bookkeeping ]-------------------------------------------------------------------

        bool is_range_mapped(uint64_t address, size_t size) const
        {
            if (size == 0)
            {
                return true;
            }

            uint64_t cursor = address;
            const uint64_t end = address + size;

            // Walk the (sorted) region map covering [address, end). Regions are page-granular and
            // non-overlapping, so a simple forward walk suffices.
            while (cursor < end)
            {
                auto it = this->regions_.upper_bound(cursor);
                if (it == this->regions_.begin())
                {
                    return false;
                }
                --it;

                const uint64_t region_end = it->first + it->second.size;
                if (cursor < it->first || cursor >= region_end)
                {
                    return false;
                }
                cursor = region_end;
            }

            return true;
        }

        // --[ FEX context plumbing ]-----------------------------------------------------------------

        void initialize_context()
        {
            // TODO(fex): real embedding also requires FEXCore::Config initialization (Config::Initialize,
            // setting Is64BitMode, etc.) and possibly FEXCore::Context::InitializeStaticTables(). Those
            // calls are version-specific; perform them here before CreateNewContext().
            const FEXCore::HostFeatures features{}; // TODO(fex): FEXCore::FetchHostFeatures() on real HW.
            this->context_ = FEXCore::Context::Context::CreateNewContext(features);

            this->syscall_handler_ = std::make_unique<fex_syscall_handler>(*this);
            this->context_->SetSyscallHandler(this->syscall_handler_.get());

            // TODO(fex): FEX requires a SignalDelegator for guest signal/exception delivery. A minimal
            // delegator (or the LinuxEmulation one) must be installed via SetSignalDelegator() for the
            // guest to handle faults; left unset in this basic scaffold.

            this->context_->InitCore();
        }

        void create_thread()
        {
            // Seed the FEX thread from the staged CPUState the loader populated before the first start().
            this->thread_ =
                this->context_->CreateThread(this->staged_state_.rip, this->staged_state_.gregs[detail::greg_rsp], &this->staged_state_);
        }

        // CPUState is owned by the thread frame once a thread exists. Before the thread is created we
        // stage register accesses in a local CPUState so the Windows loader can set up the initial
        // context; create_thread() seeds the real thread from it.
        FEXCore::Core::CPUState& cpu_state()
        {
            if (this->thread_ != nullptr)
            {
                return this->thread_->CurrentFrame->State; // TODO(fex): confirm field path for the FEX version.
            }
            return this->staged_state_;
        }

        const FEXCore::Core::CPUState& cpu_state() const
        {
            if (this->thread_ != nullptr)
            {
                return this->thread_->CurrentFrame->State;
            }
            return this->staged_state_;
        }

        uint64_t read_rflags() const
        {
            // TODO(fex): FEX stores flags decomposed in CPUState; reconstruct via the context helper.
            // At rest (not in JIT) WasInJIT=false and the host GPR/PSTATE inputs are unused.
            return this->context_->ReconstructCompactedEFLAGS(this->thread_, /*WasInJIT=*/false, nullptr, 0);
        }

        void write_rflags(uint64_t rflags)
        {
            this->context_->SetFlagsFromCompactedEFLAGS(this->thread_, static_cast<uint32_t>(rflags));
        }

        uint16_t segment_selector(int index) const
        {
            const auto& state = this->cpu_state();
            switch (index)
            {
            case 0:
                return state.es_idx;
            case 1:
                return state.cs_idx;
            case 2:
                return state.ss_idx;
            case 3:
                return state.ds_idx;
            case 4:
                return state.fs_idx;
            case 5:
                return state.gs_idx;
            default:
                return 0;
            }
        }

        void set_segment_selector(int index, const void* value, size_t size)
        {
            uint16_t selector = 0;
            std::memcpy(&selector, value, (std::min)(size, sizeof(selector)));
            auto& state = this->cpu_state();
            switch (index)
            {
            case 0:
                state.es_idx = selector;
                break;
            case 1:
                state.cs_idx = selector;
                break;
            case 2:
                state.ss_idx = selector;
                break;
            case 3:
                state.ds_idx = selector;
                break;
            case 4:
                state.fs_idx = selector;
                break;
            case 5:
                state.gs_idx = selector;
                break;
            default:
                break;
            }
        }

        void mark_executable_range(uint64_t address, size_t size, memory_permission permissions)
        {
            if (this->thread_ != nullptr && (permissions & memory_permission::exec) != memory_permission::none)
            {
                this->syscall_handler_->MarkGuestExecutableRange(this->thread_, address, size);
            }
        }

        void invalidate_code_range(uint64_t address, size_t size) const
        {
            if (this->context_)
            {
                this->context_->InvalidateCodeBuffersCodeRange(address, size);
            }
        }

        void request_thread_stop()
        {
            // TODO(fex): make the in-flight ExecuteThread return. Depending on FEX version this is a
            // per-thread stop request / signal. The syscall bridge also checks stop_requested_ after
            // each guest syscall and unwinds out of the run loop.
        }

        emulator_hook* make_hook()
        {
            return reinterpret_cast<emulator_hook*>(this->next_hook_id_++);
        }

        // --[ state ]--------------------------------------------------------------------------------

        fextl::unique_ptr<FEXCore::Context::Context> context_{};
        FEXCore::Core::InternalThreadState* thread_ = nullptr;
        std::unique_ptr<fex_syscall_handler> syscall_handler_{};
        FEXCore::Core::CPUState staged_state_{};

        uint64_t gdt_base_ = 0;
        uint32_t gdt_limit_ = 0;

        std::atomic<bool> stop_requested_{false};
        uintptr_t next_hook_id_ = 1;

        std::map<uint64_t, mapped_region> regions_;

        hook_entry* syscall_hook_ = nullptr;
        std::unordered_map<emulator_hook*, hook_entry> instruction_hooks_;
        std::unordered_map<emulator_hook*, interrupt_hook_callback> interrupt_hooks_;
        std::unordered_map<emulator_hook*, memory_access_hook_callback> memory_read_hooks_;
        std::unordered_map<emulator_hook*, memory_access_hook_callback> memory_write_hooks_;
        std::unordered_map<emulator_hook*, memory_execution_hook_callback> memory_execution_hooks_;
        std::unordered_map<emulator_hook*, memory_violation_hook_callback> memory_violation_hooks_;
        std::unordered_map<emulator_hook*, basic_block_hook_callback> basic_block_hooks_;
    };

    // -----------------------------------------------------------------------------------------------
    // fex_syscall_handler method bodies (fex_x86_64_emulator is now complete).
    // -----------------------------------------------------------------------------------------------

    uint64_t fex_syscall_handler::HandleSyscall(FEXCore::Core::CpuStateFrame* /*frame*/, FEXCore::HLE::SyscallArguments* /*args*/)
    {
        auto* hook = this->emulator_.syscall_hook_;
        if (hook != nullptr && hook->callback)
        {
            // The Windows syscall layer reads/writes guest registers itself through the emulator, so the
            // hook needs no arguments here. It places the NT status in RAX before returning.
            hook->callback(0);
        }

        if (this->emulator_.stop_requested_)
        {
            this->emulator_.request_thread_stop();
        }

        // FEX writes our return value into guest RAX; hand back whatever the hook already set so the
        // value is preserved.
        return this->emulator_.cpu_state().gregs[detail::greg_rax];
    }

    FEXCore::HLE::SyscallABI fex_syscall_handler::GetSyscallABI(uint64_t /*syscall*/)
    {
        // Everything is handled inside HandleSyscall; -1 marks "no host syscall passthrough".
        return FEXCore::HLE::SyscallABI{.NumArgs = 6, .HasReturn = true, .HostSyscallNumber = -1};
    }

    FEXCore::HLE::ExecutableRangeInfo fex_syscall_handler::QueryGuestExecutableRange(FEXCore::Core::InternalThreadState* /*thread*/,
                                                                                     uint64_t /*address*/)
    {
        return {}; // TODO(fex): report the containing mapped range for better cache invalidation.
    }

    std::unique_ptr<x86_64_emulator> create_x86_64_emulator()
    {
        return std::make_unique<fex_x86_64_emulator>();
    }
} // namespace sogen::fex
