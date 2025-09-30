#define UNICORN_EMULATOR_IMPL
#include "unicorn_x86_64_emulator.hpp"

#include <array>
#include <ranges>
#include <optional>

#include "unicorn_memory_regions.hpp"
#include "unicorn_hook.hpp"

#include "function_wrapper.hpp"

namespace unicorn
{
    namespace
    {
        static_assert(static_cast<uint32_t>(memory_permission::none) == UC_PROT_NONE);
        static_assert(static_cast<uint32_t>(memory_permission::read) == UC_PROT_READ);
        static_assert(static_cast<uint32_t>(memory_permission::exec) == UC_PROT_EXEC);
        static_assert(static_cast<uint32_t>(memory_permission::all) == UC_PROT_ALL);

        static_assert(static_cast<uint32_t>(x86_register::end) == UC_X86_REG_ENDING);

        uc_x86_insn map_hookable_instruction(const x86_hookable_instructions instruction)
        {
            switch (instruction)
            {
            case x86_hookable_instructions::syscall:
                return UC_X86_INS_SYSCALL;
            case x86_hookable_instructions::cpuid:
                return UC_X86_INS_CPUID;
            case x86_hookable_instructions::rdtsc:
                return UC_X86_INS_RDTSC;
            case x86_hookable_instructions::rdtscp:
                return UC_X86_INS_RDTSCP;
            default:
                throw std::runtime_error("Bad instruction for mapping");
            }
        }

        memory_violation_type map_memory_violation_type(const uc_mem_type mem_type)
        {
            switch (mem_type)
            {
            case UC_MEM_READ_PROT:
            case UC_MEM_WRITE_PROT:
            case UC_MEM_FETCH_PROT:
                return memory_violation_type::protection;
            case UC_MEM_READ_UNMAPPED:
            case UC_MEM_WRITE_UNMAPPED:
            case UC_MEM_FETCH_UNMAPPED:
                return memory_violation_type::unmapped;
            default:
                throw std::runtime_error("Memory type does not constitute a violation");
            }
        }

        memory_operation map_memory_operation(const uc_mem_type mem_type)
        {
            switch (mem_type)
            {
            case UC_MEM_READ:
            case UC_MEM_READ_PROT:
            case UC_MEM_READ_AFTER:
            case UC_MEM_READ_UNMAPPED:
                return memory_operation::read;
            case UC_MEM_WRITE:
            case UC_MEM_WRITE_PROT:
            case UC_MEM_WRITE_UNMAPPED:
                return memory_operation::write;
            case UC_MEM_FETCH:
            case UC_MEM_FETCH_PROT:
            case UC_MEM_FETCH_UNMAPPED:
                return memory_operation::exec;
            default:
                return memory_operation::none;
            }
        }

        struct hook_object : utils::object
        {
            emulator_hook* as_opaque_hook()
            {
                return reinterpret_cast<emulator_hook*>(this);
            }
        };

        class hook_container : public hook_object
        {
          public:
            template <typename T>
                requires(std::is_base_of_v<utils::object, T> && std::is_move_constructible_v<T>)
            void add(T data, unicorn_hook hook)
            {
                hook_entry entry{};

                entry.data = std::make_unique<T>(std::move(data));
                entry.hook = std::move(hook);

                this->hooks_.emplace_back(std::move(entry));
            }

          private:
            struct hook_entry
            {
                std::unique_ptr<utils::object> data{};
                unicorn_hook hook{};
            };

            std::vector<hook_entry> hooks_;
        };

        struct mmio_callbacks
        {
            using read_wrapper = function_wrapper<uint64_t, uc_engine*, uint64_t, unsigned>;
            using write_wrapper = function_wrapper<void, uc_engine*, uint64_t, unsigned, uint64_t>;

            read_wrapper read{};
            write_wrapper write{};
        };

        class uc_context_serializer
        {
          public:
            uc_context_serializer(uc_engine* uc, const bool in_place)
                : uc_(uc)
            {
                if (in_place)
                {
                    // Unicorn stores pointers in the struct. The serialization here is broken
                    throw std::runtime_error("Memory saving not supported atm");
                }

#ifndef OS_WINDOWS
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#endif

                uc_ctl_context_mode(uc, UC_CTL_CONTEXT_CPU | (in_place ? UC_CTL_CONTEXT_MEMORY : 0));

#ifndef OS_WINDOWS
#pragma GCC diagnostic pop
#endif

                this->size_ = uc_context_size(uc);
                uce(uc_context_alloc(uc, &this->context_));
            }

            ~uc_context_serializer()
            {
                if (this->context_)
                {
                    (void)uc_context_free(this->context_);
                }
            }

            void serialize(utils::buffer_serializer& buffer) const
            {
                uce(uc_context_save(this->uc_, this->context_));
                buffer.write(this->context_, this->size_);
            }

            void deserialize(utils::buffer_deserializer& buffer) const
            {
                buffer.read(this->context_, this->size_);
                uce(uc_context_restore(this->uc_, this->context_));
            }

            uc_context_serializer(uc_context_serializer&&) = delete;
            uc_context_serializer(const uc_context_serializer&) = delete;
            uc_context_serializer& operator=(uc_context_serializer&&) = delete;
            uc_context_serializer& operator=(const uc_context_serializer&) = delete;

          private:
            uc_engine* uc_{};
            uc_context* context_{};
            size_t size_{};
        };

        void assert_64bit_limit(const size_t size)
        {
            if (size > sizeof(uint64_t))
            {
                throw std::runtime_error("Exceeded uint64_t size limit");
            }
        }

        class unicorn_x86_64_emulator : public x86_64_emulator
        {
          public:
            unicorn_x86_64_emulator()
            {
                uce(uc_open(UC_ARCH_X86, UC_MODE_64, &this->uc_));
                // uce(uc_ctl_set_cpu_model(this->uc_, UC_CPU_X86_EPYC_ROME));

#ifndef OS_WINDOWS
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#endif
                constexpr auto is_64_bit = sizeof(void*) >= 8;
                uce(uc_ctl_set_tcg_buffer_size(this->uc_, (is_64_bit ? 2 : 1) << 30 /* 2 gb */));

#ifndef OS_WINDOWS
#pragma GCC diagnostic pop
#endif
            }

            ~unicorn_x86_64_emulator() override
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

            void load_gdt(const pointer_type address, const uint32_t limit) override
            {
                uc_x86_mmr gdt{};
                gdt.base = address;
                gdt.limit = limit;

                this->write_register(x86_register::gdtr, &gdt, sizeof(gdt));
            }

            void set_segment_base(const x86_register base, const pointer_type value) override
            {
                constexpr auto IA32_FS_BASE_MSR = 0xC0000100;
                constexpr auto IA32_GS_BASE_MSR = 0xC0000101;

                struct msr_value
                {
                    uint64_t id{};
                    uint64_t value{};
                };

                msr_value msr_val{
                    .id = 0,
                    .value = value,
                };

                switch (base)
                {
                case x86_register::fs:
                case x86_register::fs_base:
                    msr_val.id = IA32_FS_BASE_MSR;
                    preserved_fs_base_ = static_cast<uint64_t>(value);
                    break;
                case x86_register::gs:
                case x86_register::gs_base:
                    msr_val.id = IA32_GS_BASE_MSR;
                    preserved_gs_base_ = static_cast<uint64_t>(value);
                    break;
                default:
                    return;
                }

                this->write_register(x86_register::msr, &msr_val, sizeof(msr_val));
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

            bool read_descriptor_table(const int reg, descriptor_table_register& table) override
            {
                if (reg != static_cast<int>(x86_register::gdtr) && reg != static_cast<int>(x86_register::idtr))
                {
                    return false;
                }

                uc_x86_mmr gdt{};

                this->read_register(x86_register::gdtr, &gdt, sizeof(gdt));

                table.base = gdt.base;
                table.limit = gdt.limit;
                return true;
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

            void write_memory(const uint64_t address, const void* data, const size_t size) override
            {
                uce(uc_mem_write(*this, address, data, size));
            }

            void apply_memory_protection(const uint64_t address, const size_t size, memory_permission permissions) override
            {
                uce(uc_mem_protect(*this, address, size, static_cast<uint32_t>(permissions)));
            }

            emulator_hook* hook_instruction(const int instruction_type, instruction_hook_callback callback) override
            {
                unicorn_hook hook{*this};
                auto container = std::make_unique<hook_container>();

                const auto inst_type = static_cast<x86_hookable_instructions>(instruction_type);

                if (inst_type == x86_hookable_instructions::invalid)
                {
                    function_wrapper<int, uc_engine*> wrapper([c = std::move(callback)](uc_engine*) {
                        return (c(0) == instruction_hook_continuation::skip_instruction) ? 1 : 0;
                    });

                    uce(uc_hook_add(*this, hook.make_reference(), UC_HOOK_INSN_INVALID, wrapper.get_function(), wrapper.get_user_data(), 0,
                                    std::numeric_limits<pointer_type>::max()));
                    container->add(std::move(wrapper), std::move(hook));
                }
                else if (inst_type == x86_hookable_instructions::syscall)
                {
                    function_wrapper<void, uc_engine*> wrapper([c = std::move(callback)](uc_engine*) { (void)c(0); });

                    const auto uc_instruction = map_hookable_instruction(inst_type);
                    uce(uc_hook_add(*this, hook.make_reference(), UC_HOOK_INSN, wrapper.get_function(), wrapper.get_user_data(), 0,
                                    std::numeric_limits<pointer_type>::max(), uc_instruction));

                    container->add(std::move(wrapper), std::move(hook));
                }
                else
                {
                    function_wrapper<int, uc_engine*> wrapper([c = std::move(callback)](uc_engine*) {
                        return (c(0) == instruction_hook_continuation::skip_instruction) ? 1 : 0;
                    });

                    const auto uc_instruction = map_hookable_instruction(inst_type);
                    uce(uc_hook_add(*this, hook.make_reference(), UC_HOOK_INSN, wrapper.get_function(), wrapper.get_user_data(), 0,
                                    std::numeric_limits<pointer_type>::max(), uc_instruction));

                    container->add(std::move(wrapper), std::move(hook));
                }

                auto* result = container->as_opaque_hook();

                this->hooks_.push_back(std::move(container));

                return result;
            }

            emulator_hook* hook_basic_block(basic_block_hook_callback callback) override
            {
                function_wrapper<void, uc_engine*, uint64_t, size_t> wrapper(
                    [c = std::move(callback)](uc_engine*, const uint64_t address, const size_t size) {
                        basic_block block{};
                        block.address = address;
                        block.size = size;

                        c(block);
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
                    [c = std::move(callback)](uc_engine*, const int interrupt_type) { c(interrupt_type); });

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

                        const auto resume =
                            c(address, static_cast<uint64_t>(size), operation, violation) == memory_violation_continuation::resume;

                        const auto new_ip = this->read_instruction_pointer();
                        const auto has_ip_changed = ip != new_ip;

                        if (!resume)
                        {
                            return false;
                        }

                        if (resume && has_ip_changed)
                        {
                            this->violation_ip_ = new_ip;
                        }
                        else
                        {
                            this->violation_ip_ = std::nullopt;
                        }

                        if (has_ip_changed)
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

            emulator_hook* hook_memory_execution(const uint64_t address, const uint64_t size, memory_execution_hook_callback callback)
            {
                auto exec_wrapper = [c = std::move(callback), this](uc_engine*, const uint64_t address, const uint32_t /*size*/) {
                    c(address); //

                    // Fix unicorn bug?
                    const auto cs_current = this->reg<uint16_t>(x86_register::cs);
                    if (this->current_reg_cs_ != cs_current)
                    {
                        this->set_segment_base(x86_register::gs, preserved_gs_base_);
                        this->set_segment_base(x86_register::fs, preserved_fs_base_);
                        this->current_reg_cs_ = cs_current;
                    }
                };

                function_wrapper<void, uc_engine*, uint64_t, uint32_t> wrapper(std::move(exec_wrapper));

                unicorn_hook hook{*this};

                uce(uc_hook_add(*this, hook.make_reference(), UC_HOOK_CODE, wrapper.get_function(), wrapper.get_user_data(), address,
                                address + size));

                auto* container = this->create_hook_container();
                container->add(std::move(wrapper), std::move(hook));
                return container->as_opaque_hook();
            }

            emulator_hook* hook_memory_execution(memory_execution_hook_callback callback) override
            {
                return this->hook_memory_execution(0, std::numeric_limits<uint64_t>::max(), std::move(callback));
            }

            emulator_hook* hook_memory_execution(const uint64_t address, memory_execution_hook_callback callback) override
            {
                return this->hook_memory_execution(address, 1, std::move(callback));
            }

            emulator_hook* hook_memory_read(const uint64_t address, const uint64_t size, memory_access_hook_callback callback) override
            {
                auto read_wrapper = [c = std::move(callback)](uc_engine*, const uc_mem_type type, const uint64_t address, const int length,
                                                              const uint64_t value) {
                    const auto operation = map_memory_operation(type);
                    if (operation == memory_operation::read && length > 0)
                    {
                        c(address, &value, std::min(static_cast<size_t>(length), sizeof(value)));
                    }
                };

                function_wrapper<void, uc_engine*, uc_mem_type, uint64_t, int, int64_t> wrapper(std::move(read_wrapper));

                unicorn_hook hook{*this};
                uce(uc_hook_add(*this, hook.make_reference(), UC_HOOK_MEM_READ_AFTER, wrapper.get_function(), wrapper.get_user_data(),
                                address, address + size));

                auto* container = this->create_hook_container();
                container->add(std::move(wrapper), std::move(hook));
                return container->as_opaque_hook();
            }

            emulator_hook* hook_memory_write(const uint64_t address, const uint64_t size, memory_access_hook_callback callback) override
            {
                auto write_wrapper = [c = std::move(callback)](uc_engine*, const uc_mem_type type, const uint64_t addr, const int length,
                                                               const uint64_t value) {
                    const auto operation = map_memory_operation(type);
                    if (operation == memory_operation::write && length > 0)
                    {
                        c(addr, &value, std::min(static_cast<size_t>(length), sizeof(value)));
                    }
                };

                function_wrapper<void, uc_engine*, uc_mem_type, uint64_t, int, int64_t> wrapper(std::move(write_wrapper));

                unicorn_hook hook{*this};

                uce(uc_hook_add(*this, hook.make_reference(), UC_HOOK_MEM_WRITE, wrapper.get_function(), wrapper.get_user_data(), address,
                                address + size));

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

                // Serialize unicorn bug workaround state
                buffer.write(this->preserved_gs_base_);
                buffer.write(this->preserved_fs_base_);
                buffer.write(this->current_reg_cs_);
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
                // Deserialize unicorn bug workaround state
                buffer.read(this->preserved_gs_base_);
                buffer.read(this->preserved_fs_base_);
                buffer.read(this->current_reg_cs_);
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

            std::string get_name() const override
            {
                return "Unicorn Engine";
            }

          private:
            mutable bool has_snapshots_{false};
            uc_engine* uc_{};
            std::optional<uint64_t> violation_ip_{};
            std::vector<std::unique_ptr<hook_object>> hooks_{};
            std::unordered_map<uint64_t, mmio_callbacks> mmio_{};

            // gs & fs base (Fix unicorn Bug?)
            mutable uint64_t preserved_gs_base_{0};
            mutable uint64_t preserved_fs_base_{0};
            mutable uint16_t current_reg_cs_{0x33};
        };
    }

    std::unique_ptr<x86_64_emulator> create_x86_64_emulator()
    {
        return std::make_unique<unicorn_x86_64_emulator>();
    }
}
