#define UNICORN_EMULATOR_IMPL
#include "unicorn_arm64_emulator.hpp"

#include <optional>
#include <ranges>
#include <string>
#include <unordered_map>

#include "unicorn_common.hpp"

namespace sogen::unicorn
{
    namespace
    {
        static_assert(static_cast<uint32_t>(arm64_register::end) == UC_ARM64_REG_ENDING);

        constexpr int arm64_excp_swi = 2;
        constexpr int arm64_excp_bkpt = 7;

        int map_hookable_instruction(const arm64_hookable_instructions instruction)
        {
            switch (instruction)
            {
            case arm64_hookable_instructions::svc:
                return arm64_excp_swi;
            case arm64_hookable_instructions::brk:
                return arm64_excp_bkpt;
            default:
                throw std::runtime_error("Bad instruction for mapping");
            }
        }

        constexpr uc_arm64_cp_reg scr_el3{.crn = 1, .crm = 1, .op0 = 3, .op1 = 6, .op2 = 0, .val = 0};
        constexpr uc_arm64_cp_reg hcr_el2{.crn = 1, .crm = 1, .op0 = 3, .op1 = 4, .op2 = 0, .val = 0};
        constexpr uc_arm64_cp_reg sctlr_el1{.crn = 1, .crm = 0, .op0 = 3, .op1 = 0, .op2 = 0, .val = 0};
        constexpr uc_arm64_cp_reg id_aa64isar1_el1{.crn = 0, .crm = 6, .op0 = 3, .op1 = 0, .op2 = 1, .val = 0};
        constexpr uc_arm64_cp_reg tcr_el1{.crn = 2, .crm = 0, .op0 = 3, .op1 = 0, .op2 = 2, .val = 0};

        constexpr uint64_t scr_ns = 1ULL << 0;
        constexpr uint64_t scr_rw = 1ULL << 10;
        constexpr uint64_t scr_apk = 1ULL << 16;
        constexpr uint64_t scr_api = 1ULL << 17;

        constexpr uint64_t hcr_rw = 1ULL << 31;
        constexpr uint64_t hcr_apk = 1ULL << 40;
        constexpr uint64_t hcr_api = 1ULL << 41;

        constexpr uint64_t sctlr_en_db = 1ULL << 13;
        constexpr uint64_t sctlr_en_da = 1ULL << 27;
        constexpr uint64_t sctlr_en_ib = 1ULL << 30;
        constexpr uint64_t sctlr_en_ia = 1ULL << 31;

        constexpr uint64_t id_aa64isar1_apa = 0xFULL << 4;

        // Top-byte-ignore is what confines the pointer authentication code to bits 54:47. With TBI clear
        // QEMU also spreads it over bits 63:56 (deps/unicorn/qemu/target/arm/pauth_helper.c:310-315),
        // which overwrites the tag byte that real arm64e software keeps live data in: libobjc stores a
        // class's realized flag in bit 63 of class_data_bits_t and branches on it before authenticating,
        // so a signature reaching that far forges the flag rather than merely looking wrong.
        constexpr uint64_t tcr_tbi0 = 1ULL << 37;
        constexpr uint64_t tcr_tbi1 = 1ULL << 38;

        std::string describe_system_register(const uc_arm64_cp_reg& reg)
        {
            return "S" + std::to_string(reg.op0) + "_" + std::to_string(reg.op1) + "_C" + std::to_string(reg.crn) + "_C" +
                   std::to_string(reg.crm) + "_" + std::to_string(reg.op2);
        }

        uint64_t read_cp_register(uc_engine* uc, const uc_arm64_cp_reg& reg)
        {
            auto query = reg;
            uce(uc_reg_read(uc, UC_ARM64_REG_CP_REG, &query));
            return query.val;
        }

        void set_system_register_bits(uc_engine* uc, const uc_arm64_cp_reg& reg, const uint64_t bits)
        {
            auto update = reg;
            update.val = read_cp_register(uc, reg) | bits;
            uce(uc_reg_write(uc, UC_ARM64_REG_CP_REG, &update));

            if ((read_cp_register(uc, reg) & bits) != bits)
            {
                throw std::runtime_error("QEMU masked away bits of " + describe_system_register(reg) +
                                         "; pointer authentication would silently decode as a no-op");
            }
        }

        void clear_system_register_bits(uc_engine* uc, const uc_arm64_cp_reg& reg, const uint64_t bits)
        {
            auto update = reg;
            update.val = read_cp_register(uc, reg) & ~bits;
            uce(uc_reg_write(uc, UC_ARM64_REG_CP_REG, &update));

            if ((read_cp_register(uc, reg) & bits) != 0)
            {
                throw std::runtime_error("QEMU kept bits of " + describe_system_register(reg) +
                                         " set; pointer authentication would stay enforced");
            }
        }

        class unicorn_arm64_emulator : public arm64_mappable_emulator
        {
          public:
            unicorn_arm64_emulator()
            {
                uce(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &this->uc_));

                // Only the "max" model advertises ID_AA64ISAR1.APA, which is what makes the pointer
                // authentication instructions do anything; the a53/a57/a72 models leave it clear
                // (deps/unicorn/qemu/target/arm/cpu64.c:239-240).
                uce(uc_ctl_set_cpu_model(this->uc_, UC_CPU_ARM64_MAX));

                this->enable_pointer_authentication();
            }

            ~unicorn_arm64_emulator() override
            {
                reset_object_with_delayed_destruction(this->hooks_);
                uc_close(this->uc_);
            }

            void start(const size_t count) override
            {
                const auto start = this->violation_ip_.value_or(this->read_instruction_pointer());
                this->violation_ip_ = std::nullopt;

                constexpr auto end = std::numeric_limits<uint64_t>::max();
                const auto res = uc_emu_start(*this, start, end, 0, count);
                if (res == UC_ERR_OK)
                {
                    return;
                }

                const auto is_violation =           //
                    res == UC_ERR_READ_UNMAPPED ||  //
                    res == UC_ERR_WRITE_UNMAPPED || //
                    res == UC_ERR_FETCH_UNMAPPED || //
                    res == UC_ERR_READ_PROT ||      //
                    res == UC_ERR_WRITE_PROT ||     //
                    res == UC_ERR_FETCH_PROT;

                if (!is_violation || !this->has_violation())
                {
                    uce(res);
                }
            }

            void stop() override
            {
                uce(uc_emu_stop(*this));
            }

            void set_thread_pointer(const pointer_type value) override
            {
                this->reg(arm64_register::tpidrro_el0, value);
            }

            pointer_type get_thread_pointer() override
            {
                return this->reg(arm64_register::tpidrro_el0);
            }

            uint64_t read_system_register(const uint32_t op0, const uint32_t op1, const uint32_t crn, const uint32_t crm,
                                          const uint32_t op2) override
            {
                const uc_arm64_cp_reg reg{.crn = crn, .crm = crm, .op0 = op0, .op1 = op1, .op2 = op2, .val = 0};
                return read_cp_register(this->uc_, reg);
            }

            bool sign_pointer(uint64_t& pointer, const arm64_pauth_key key, const uint64_t discriminator) override
            {
                uint64_t signed_pointer = 0;
                if (uc_ctl_pauth_sign(this->uc_, pointer, static_cast<int>(key), discriminator, &signed_pointer) != UC_ERR_OK)
                {
                    return false;
                }

                pointer = signed_pointer;
                return true;
            }

            size_t write_raw_register(const int reg, const void* value, const size_t size) override
            {
                auto result_size = size;
                uce(uc_reg_write2(*this, reg, value, &result_size));

                if (size < result_size)
                {
                    throw std::runtime_error("Register size mismatch: " + std::to_string(size) + " != " + std::to_string(result_size));
                }

                return result_size;
            }

            size_t read_raw_register(const int reg, void* value, const size_t size) override
            {
                size_t result_size = size;
                memset(value, 0, size);
                uce(uc_reg_read2(*this, reg, value, &result_size));

                if (size < result_size)
                {
                    throw std::runtime_error("Register size mismatch: " + std::to_string(size) + " != " + std::to_string(result_size));
                }

                return result_size;
            }

            bool read_descriptor_table(int, descriptor_table_register&) override
            {
                return false;
            }

            void map_mmio(const uint64_t address, const size_t size, mmio_read_callback read_cb, mmio_write_callback write_cb) override
            {
                auto read_wrapper = [c = std::move(read_cb)](uc_engine*, const uint64_t addr, const uint32_t s) {
                    assert_64bit_limit(s);
                    uint64_t value{};
                    c(addr, &value, s);
                    return value;
                };

                auto write_wrapper = [c = std::move(write_cb)](uc_engine*, const uint64_t addr, const uint32_t s, const uint64_t value) {
                    assert_64bit_limit(s);
                    c(addr, &value, s);
                };

                mmio_callbacks cb{
                    .read = mmio_callbacks::read_wrapper(std::move(read_wrapper)),
                    .write = mmio_callbacks::write_wrapper(std::move(write_wrapper)),
                };

                uce(uc_mmio_map(*this, address, size, cb.read.get_c_function(), cb.read.get_user_data(), cb.write.get_c_function(),
                                cb.write.get_user_data()));

                this->mmio_[address] = std::move(cb);
            }

            void map_memory(const uint64_t address, const size_t size, memory_permission permissions) override
            {
                uce(uc_mem_map(*this, address, size, static_cast<uint32_t>(permissions)));
            }

            void map_host_memory(const uint64_t address, const size_t size, void* host_pointer, memory_permission permissions) override
            {
                uce(uc_mem_map_ptr(*this, address, size, static_cast<uint32_t>(permissions), host_pointer));
            }

            void unmap_memory(const uint64_t address, const size_t size) override
            {
                uce(uc_mem_unmap(*this, address, size));

                const auto mmio_entry = this->mmio_.find(address);
                if (mmio_entry != this->mmio_.end())
                {
                    this->mmio_.erase(mmio_entry);
                }
            }

            bool try_read_memory(const uint64_t address, void* data, const size_t size) const override
            {
                return uc_mem_read(*this, address, data, size) == UC_ERR_OK;
            }

            void read_memory(const uint64_t address, void* data, const size_t size) const override
            {
                uce(uc_mem_read(*this, address, data, size));
            }

            bool try_write_memory(const uint64_t address, const void* data, const size_t size) override
            {
                return uc_mem_write(*this, address, data, size) == UC_ERR_OK;
            }

            void write_memory(const uint64_t address, const void* data, const size_t size) override
            {
                uce(uc_mem_write(*this, address, data, size));
            }

            void apply_memory_protection(const uint64_t address, const size_t size, memory_permission permissions) override
            {
                uce(uc_mem_protect(*this, address, size, static_cast<uint32_t>(permissions)));
            }

            // AArch64 has no UC_HOOK_INSN for svc or brk. They raise EXCP_SWI / EXCP_BKPT, which unicorn
            // dispatches through UC_HOOK_INTR instead (qemu/accel/tcg/cpu-exec.c). There is likewise no
            // UC_HOOK_INSN_INVALID equivalent, so arm64_hookable_instructions::invalid is unsupported.
            emulator_hook* hook_instruction(const int instruction_type, instruction_hook_callback callback) override
            {
                const auto expected_intno = map_hookable_instruction(static_cast<arm64_hookable_instructions>(instruction_type));

                function_wrapper<void, uc_engine*, int> wrapper(
                    [c = std::move(callback), expected_intno, this](uc_engine*, const int intno) {
                        if (intno != expected_intno)
                        {
                            return;
                        }

                        // Unlike the x86 syscall hook, unicorn does not re-add the instruction size after this
                        // callback: svc leaves PC past the instruction, while brk leaves it on the instruction
                        // (gen_exception_bkpt_insn uses pc_curr), so a brk handler must advance PC by 4 itself.
                        // Every instruction_hook_continuation is a no-op here; the trap already replaced the instruction.
                        c(*this, 0);
                    });

                unicorn_hook hook{*this};
                auto container = std::make_unique<hook_container>();

                uce(uc_hook_add(*this, hook.make_reference(), UC_HOOK_INTR, wrapper.get_function(), wrapper.get_user_data(), 0,
                                std::numeric_limits<pointer_type>::max()));

                container->add(std::move(wrapper), std::move(hook));

                auto* result = container->as_opaque_hook();
                this->hooks_.push_back(std::move(container));
                return result;
            }

            emulator_hook* hook_basic_block(basic_block_hook_callback callback) override
            {
                // uc_cb_hookcode_t takes the size as uint32_t. Declaring it size_t here happens to work on
                // 64-bit native ABIs but is a hard function-signature trap under wasm64, where the wrapper
                // is emitted as (i64,i64,i64,i64) and unicorn calls it through (i64,i64,i32,i64).
                function_wrapper<void, uc_engine*, uint64_t, uint32_t> wrapper(
                    [c = std::move(callback), this](uc_engine*, const uint64_t address, const uint32_t size) {
                        basic_block block{};
                        block.address = address;
                        block.size = size;

                        // Unicorn reports only the block's byte length. A64 instructions are all four
                        // bytes wide, so the count is exact here in a way it could never be for x86.
                        block.instruction_count = size / 4;

                        c(*this, block);
                    });

                unicorn_hook hook{*this};
                auto container = std::make_unique<hook_container>();

                uce(uc_hook_add(*this, hook.make_reference(), UC_HOOK_BLOCK, wrapper.get_function(), wrapper.get_user_data(), 0,
                                std::numeric_limits<pointer_type>::max()));

                container->add(std::move(wrapper), std::move(hook));

                auto* result = container->as_opaque_hook();
                this->hooks_.push_back(std::move(container));
                return result;
            }

            emulator_hook* hook_interrupt(interrupt_hook_callback callback) override
            {
                function_wrapper<void, uc_engine*, int> wrapper(
                    [c = std::move(callback), this](uc_engine*, const int interrupt_type) { c(*this, interrupt_type); });

                unicorn_hook hook{*this};
                auto container = std::make_unique<hook_container>();

                uce(uc_hook_add(*this, hook.make_reference(), UC_HOOK_INTR, wrapper.get_function(), wrapper.get_user_data(), 0,
                                std::numeric_limits<pointer_type>::max()));

                container->add(std::move(wrapper), std::move(hook));

                auto* result = container->as_opaque_hook();
                this->hooks_.push_back(std::move(container));
                return result;
            }

            emulator_hook* hook_memory_violation(memory_violation_hook_callback callback) override
            {
                function_wrapper<bool, uc_engine*, uc_mem_type, uint64_t, int, int64_t> wrapper(
                    [c = std::move(callback), this](uc_engine*, const uc_mem_type type, const uint64_t address, const int size,
                                                    const int64_t) {
                        const auto ip = this->read_instruction_pointer();

                        assert(size >= 0);
                        const auto operation = map_memory_operation(type);
                        const auto violation = map_memory_violation_type(type);

                        const auto result = c(*this, address, static_cast<uint64_t>(size), operation, violation);
                        const auto restart = result == memory_violation_continuation::restart;
                        const auto resume = result == memory_violation_continuation::resume || restart;

                        const auto new_ip = this->read_instruction_pointer();
                        const auto set_ip = ip != new_ip || restart;

                        if (!resume)
                        {
                            return false;
                        }

                        if (resume && set_ip)
                        {
                            this->violation_ip_ = new_ip;
                        }
                        else
                        {
                            this->violation_ip_ = std::nullopt;
                        }

                        if (set_ip)
                        {
                            return false;
                        }

                        return true;
                    });

                unicorn_hook hook{*this};
                auto container = std::make_unique<hook_container>();

                uce(uc_hook_add(*this, hook.make_reference(), UC_HOOK_MEM_INVALID, wrapper.get_function(), wrapper.get_user_data(), 0,
                                std::numeric_limits<uint64_t>::max()));

                container->add(std::move(wrapper), std::move(hook));

                auto* result = container->as_opaque_hook();
                this->hooks_.push_back(std::move(container));
                return result;
            }

            emulator_hook* hook_memory_range_execution(const uint64_t address, const uint64_t size,
                                                       memory_execution_hook_callback callback) override
            {
                auto exec_wrapper = [c = std::move(callback), this](uc_engine*, const uint64_t address, const uint32_t /*size*/) {
                    const auto old_ip = this->read_instruction_pointer();
                    c(*this, address);

                    const auto new_ip = this->read_instruction_pointer();
                    if (new_ip != old_ip)
                    {
                        this->violation_ip_ = new_ip;
                        uce(uc_emu_stop(*this));
                    }
                };

                function_wrapper<void, uc_engine*, uint64_t, uint32_t> wrapper(std::move(exec_wrapper));

                unicorn_hook hook{*this};

                uce(uc_hook_add(*this, hook.make_reference(), UC_HOOK_CODE, wrapper.get_function(), wrapper.get_user_data(), address,
                                calc_end_address(address, size)));

                auto* container = this->create_hook_container();
                container->add(std::move(wrapper), std::move(hook));
                return container->as_opaque_hook();
            }

            emulator_hook* hook_memory_execution(memory_execution_hook_callback callback) override
            {
                return this->hook_memory_range_execution(0, std::numeric_limits<uint64_t>::max(), std::move(callback));
            }

            emulator_hook* hook_memory_execution(const uint64_t address, memory_execution_hook_callback callback) override
            {
                return this->hook_memory_range_execution(address, 1, std::move(callback));
            }

            emulator_hook* hook_memory_read(const uint64_t address, const uint64_t size, memory_access_hook_callback callback) override
            {
                auto read_wrapper = [c = std::move(callback), this](uc_engine*, const uc_mem_type type, const uint64_t address,
                                                                    const int length, const uint64_t value) {
                    const auto operation = map_memory_operation(type);
                    if (operation == memory_operation::read && length > 0)
                    {
                        c(*this, address, &value, std::min(static_cast<size_t>(length), sizeof(value)));
                    }
                };

                function_wrapper<void, uc_engine*, uc_mem_type, uint64_t, int, int64_t> wrapper(std::move(read_wrapper));

                unicorn_hook hook{*this};

                uce(uc_hook_add(*this, hook.make_reference(), UC_HOOK_MEM_READ_AFTER, wrapper.get_function(), wrapper.get_user_data(),
                                address, calc_end_address(address, size)));

                auto* container = this->create_hook_container();
                container->add(std::move(wrapper), std::move(hook));
                return container->as_opaque_hook();
            }

            emulator_hook* hook_memory_write(const uint64_t address, const uint64_t size, memory_access_hook_callback callback) override
            {
                auto write_wrapper = [c = std::move(callback), this](uc_engine*, const uc_mem_type type, const uint64_t addr,
                                                                     const int length, const uint64_t value) {
                    const auto operation = map_memory_operation(type);
                    if (operation == memory_operation::write && length > 0)
                    {
                        c(*this, addr, &value, std::min(static_cast<size_t>(length), sizeof(value)));
                    }
                };

                function_wrapper<void, uc_engine*, uc_mem_type, uint64_t, int, int64_t> wrapper(std::move(write_wrapper));

                unicorn_hook hook{*this};

                uce(uc_hook_add(*this, hook.make_reference(), UC_HOOK_MEM_WRITE, wrapper.get_function(), wrapper.get_user_data(), address,
                                calc_end_address(address, size)));

                auto* container = this->create_hook_container();
                container->add(std::move(wrapper), std::move(hook));
                return container->as_opaque_hook();
            }

            hook_container* create_hook_container()
            {
                auto container = std::make_unique<hook_container>();
                auto* ptr = container.get();
                this->hooks_.push_back(std::move(container));
                return ptr;
            }

            void delete_hook(emulator_hook* hook) override
            {
                const auto entry = std::ranges::find_if(
                    this->hooks_, [&](const std::unique_ptr<hook_object>& hook_ptr) { return hook_ptr->as_opaque_hook() == hook; });

                if (entry != this->hooks_.end())
                {
                    const auto obj = std::move(*entry);
                    this->hooks_.erase(entry);
                    (void)obj;
                }
            }

            operator uc_engine*() const
            {
                return this->uc_;
            }

            void serialize_state(utils::buffer_serializer& buffer, const bool is_snapshot) const override
            {
                if (this->has_snapshots_ && !is_snapshot)
                {
                    // TODO: Investigate if this is really necessary
                    throw std::runtime_error("Unable to serialize after snapshot was taken!");
                }

                this->has_snapshots_ |= is_snapshot;

                const uc_context_serializer serializer(this->uc_, is_snapshot);
                serializer.serialize(buffer);
            }

            void deserialize_state(utils::buffer_deserializer& buffer, const bool is_snapshot) override
            {
                if (this->has_snapshots_ && !is_snapshot)
                {
                    // TODO: Investigate if this is really necessary
                    throw std::runtime_error("Unable to deserialize after snapshot was taken!");
                }

                const uc_context_serializer serializer(this->uc_, is_snapshot);
                serializer.deserialize(buffer);
            }

            std::vector<std::byte> save_registers() const override
            {
                utils::buffer_serializer buffer{};
                const uc_context_serializer serializer(this->uc_, false);
                serializer.serialize(buffer);
                return buffer.move_buffer();
            }

            void restore_registers(const std::vector<std::byte>& register_data) override
            {
                utils::buffer_deserializer buffer{register_data};
                const uc_context_serializer serializer(this->uc_, false);
                serializer.deserialize(buffer);
            }

            bool has_violation() const override
            {
                return this->violation_ip_.has_value();
            }

            bool supports_instruction_counting() const override
            {
                return true;
            }

            bool is_stop_thread_safe() const override
            {
                return false;
            }

            bool supports_multiple_vcpus() const override
            {
                return false;
            }

            std::string get_name() const override
            {
                return "Unicorn Engine";
            }

          private:
            // QEMU resets all three of these to zero, so PAC instructions would otherwise decode to
            // nothing (translate-a64.c:5209-5216, reached with pauth_active clear because
            // helper.c:11684-11685 only sets TBFLAG_A64.PAUTH_ACTIVE when one of SCTLR_EL1.En{IA,IB,DA,DB}
            // is set) or trap out of EL1 (pauth_helper.c:370, which demands HCR_EL2.API and SCR_EL3.API
            // because aarch64_max carries both ARM_FEATURE_EL2 and ARM_FEATURE_EL3).
            //
            // SCR_EL3.RW is load-bearing rather than cosmetic: cpu.h:2047 makes arm_el_is_aa64(env, 2)
            // false without it, and arm_hcr_el2_eff() then narrows HCR_EL2 to the AArch32-valid bits
            // (helper.c:4936-4947), a whitelist that discards everything above bit 31 - HCR_EL2.API sits
            // at bit 41. Setting HCR_EL2.API alone is therefore silently ineffective. SCR_EL3.NS is
            // needed for the same reason one step earlier: arm_hcr_el2_eff() returns 0 outright while
            // the guest is secure below EL3.
            //
            // Writes go through UC_ARM64_REG_CP_REG rather than guest MSR because Unicorn starts the
            // guest at EL1, where SCR_EL3 and HCR_EL2 are not writable; that path rebuilds the cached
            // hflags for us (unicorn_aarch64.c:406), which is what makes the SCTLR_EL1 change visible to
            // translation. On real hardware the boot chain and XNU leave all of this configured before
            // any user code runs.
            void enable_pointer_authentication() const
            {
                if ((read_cp_register(this->uc_, id_aa64isar1_el1) & id_aa64isar1_apa) == 0)
                {
                    throw std::runtime_error("CPU model does not advertise ID_AA64ISAR1.APA (cpu64.c:239); "
                                             "pointer authentication is unavailable");
                }

                set_system_register_bits(this->uc_, scr_el3, scr_ns | scr_rw | scr_apk | scr_api);
                set_system_register_bits(this->uc_, hcr_el2, hcr_rw | hcr_apk | hcr_api);
                set_system_register_bits(this->uc_, sctlr_el1, sctlr_en_ia | sctlr_en_ib | sctlr_en_da | sctlr_en_db);
                set_system_register_bits(this->uc_, tcr_el1, tcr_tbi0 | tcr_tbi1);
            }

          public:
            void set_pointer_authentication(const bool enabled) override
            {
                constexpr auto keys = sctlr_en_ia | sctlr_en_ib | sctlr_en_da | sctlr_en_db;

                if (enabled)
                {
                    set_system_register_bits(this->uc_, sctlr_el1, keys);
                }
                else
                {
                    clear_system_register_bits(this->uc_, sctlr_el1, keys);
                }
            }

          private:
            mutable bool has_snapshots_{false};
            uc_engine* uc_{};
            std::optional<uint64_t> violation_ip_{};
            std::vector<std::unique_ptr<hook_object>> hooks_{};
            std::unordered_map<uint64_t, mmio_callbacks> mmio_{};

            static uint64_t calc_end_address(const uint64_t address, uint64_t size)
            {
                if (size == 0)
                {
                    size = 1;
                }
                else if (size == std::numeric_limits<uint64_t>::max())
                {
                    size = 0;
                }

                auto end_address = address + size - 1;

                if (end_address < address)
                {
                    end_address = std::numeric_limits<uint64_t>::max();
                }

                return end_address;
            }
        };
    }

    std::unique_ptr<arm64_mappable_emulator> create_arm64_emulator()
    {
        return std::make_unique<unicorn_arm64_emulator>();
    }
} // namespace sogen::unicorn
