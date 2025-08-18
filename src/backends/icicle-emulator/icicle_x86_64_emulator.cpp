#define ICICLE_EMULATOR_IMPL
#include "icicle_x86_64_emulator.hpp"

#include <unordered_set>
#include <utils/object.hpp>
#include <utils/finally.hpp>

using icicle_emulator = struct icicle_emulator_;

extern "C"
{
    using icicle_mmio_read_func = void(void* user, uint64_t address, void* data, size_t length);
    using icicle_mmio_write_func = void(void* user, uint64_t address, const void* data, size_t length);

    using raw_func = void(void*);
    using ptr_func = void(void*, uint64_t);
    using block_func = void(void*, uint64_t, uint64_t);
    using interrupt_func = void(void*, int32_t);
    using violation_func = int32_t(void*, uint64_t address, uint8_t operation, int32_t unmapped);
    using data_accessor_func = void(void* user, const void* data, size_t length);
    using memory_access_func = icicle_mmio_write_func;

    icicle_emulator* icicle_create_emulator();
    int32_t icicle_protect_memory(icicle_emulator*, uint64_t address, uint64_t length, uint8_t permissions);
    int32_t icicle_map_memory(icicle_emulator*, uint64_t address, uint64_t length, uint8_t permissions);
    int32_t icicle_map_mmio(icicle_emulator*, uint64_t address, uint64_t length, icicle_mmio_read_func* read_callback, void* read_data,
                            icicle_mmio_write_func* write_callback, void* write_data);
    int32_t icicle_unmap_memory(icicle_emulator*, uint64_t address, uint64_t length);
    int32_t icicle_read_memory(icicle_emulator*, uint64_t address, void* data, size_t length);
    int32_t icicle_write_memory(icicle_emulator*, uint64_t address, const void* data, size_t length);
    void icicle_save_registers(icicle_emulator*, data_accessor_func* accessor, void* accessor_data);
    void icicle_restore_registers(icicle_emulator*, const void* data, size_t length);
    uint32_t icicle_create_snapshot(icicle_emulator*);
    void icicle_restore_snapshot(icicle_emulator*, uint32_t id);
    uint32_t icicle_add_syscall_hook(icicle_emulator*, raw_func* callback, void* data);
    uint32_t icicle_add_interrupt_hook(icicle_emulator*, interrupt_func* callback, void* data);
    uint32_t icicle_add_block_hook(icicle_emulator*, block_func* callback, void* data);
    uint32_t icicle_add_execution_hook(icicle_emulator*, uint64_t address, ptr_func* callback, void* data);
    uint32_t icicle_add_generic_execution_hook(icicle_emulator*, ptr_func* callback, void* data);
    uint32_t icicle_add_violation_hook(icicle_emulator*, violation_func* callback, void* data);
    uint32_t icicle_add_read_hook(icicle_emulator*, uint64_t start, uint64_t end, memory_access_func* cb, void* data);
    uint32_t icicle_add_write_hook(icicle_emulator*, uint64_t start, uint64_t end, memory_access_func* cb, void* data);
    void icicle_remove_hook(icicle_emulator*, uint32_t id);
    size_t icicle_read_register(icicle_emulator*, int reg, void* data, size_t length);
    size_t icicle_write_register(icicle_emulator*, int reg, const void* data, size_t length);
    void icicle_start(icicle_emulator*, size_t count);
    void icicle_stop(icicle_emulator*);
    void icicle_destroy_emulator(icicle_emulator*);
    void icicle_run_on_next_instruction(icicle_emulator*, raw_func* callback, void* data);
}

namespace icicle
{
    namespace
    {
        void ice(const bool result, const std::string_view error)
        {
            if (!result)
            {
                throw std::runtime_error(std::string(error));
            }
        }

        template <typename T>
        struct function_object : utils::object
        {
            bool* hook_state{};
            std::function<T> func{};

            function_object(std::function<T> f = {}, bool* state = nullptr)
                : hook_state(state),
                  func(std::move(f))
            {
            }

            template <typename... Args>
            auto operator()(Args&&... args) const
            {
                bool old_state{};
                if (this->hook_state)
                {
                    old_state = *this->hook_state;
                    *this->hook_state = true;
                }

                const auto _ = utils::finally([&] {
                    if (this->hook_state)
                    {
                        *this->hook_state = old_state;
                    }
                });

                return this->func.operator()(std::forward<Args>(args)...);
            }

            ~function_object() override = default;
        };

        template <typename T>
        std::unique_ptr<function_object<T>> make_function_object(std::function<T> func, bool& hook_state)
        {
            return std::make_unique<function_object<T>>(std::move(func), &hook_state);
        }

        struct memory_access_hook
        {
            uint64_t address{};
            uint64_t size{};
            memory_access_hook_callback callback{};
            bool is_read{};
        };
    }

    class icicle_x86_64_emulator : public x86_64_emulator
    {
      public:
        icicle_x86_64_emulator()
            : emu_(icicle_create_emulator())
        {
            if (!this->emu_)
            {
                throw std::runtime_error("Failed to create icicle emulator instance");
            }
        }

        ~icicle_x86_64_emulator() override
        {
            reset_object_with_delayed_destruction(this->hooks_);
            reset_object_with_delayed_destruction(this->storage_);
            utils::reset_object_with_delayed_destruction(this->hooks_to_install_);

            if (this->emu_)
            {
                icicle_destroy_emulator(this->emu_);
                this->emu_ = nullptr;
            }
        }

        void start(const size_t count) override
        {
            icicle_start(this->emu_, count);
        }

        void stop() override
        {
            icicle_stop(this->emu_);
        }

        void load_gdt(const pointer_type address, const uint32_t limit) override
        {
            struct gdtr
            {
                uint32_t padding{};
                uint32_t limit{};
                uint64_t address{};
            };

            const gdtr entry{.limit = limit, .address = address};
            static_assert(sizeof(gdtr) - offsetof(gdtr, limit) == 12);

            this->write_register(x86_register::gdtr, &entry.limit, 12);
        }

        void set_segment_base(const x86_register base, const pointer_type value) override
        {
            switch (base)
            {
            case x86_register::fs:
            case x86_register::fs_base:
                this->reg(x86_register::fs_base, value);
                break;
            case x86_register::gs:
            case x86_register::gs_base:
                this->reg(x86_register::gs_base, value);
                break;
            default:
                break;
            }
        }

        size_t write_raw_register(const int reg, const void* value, const size_t size) override
        {
            return icicle_write_register(this->emu_, reg, value, size);
        }

        size_t read_raw_register(const int reg, void* value, const size_t size) override
        {
            return icicle_read_register(this->emu_, reg, value, size);
        }

        void map_mmio(const uint64_t address, const size_t size, mmio_read_callback read_cb, mmio_write_callback write_cb) override
        {
            struct mmio_wrapper : utils::object
            {
                uint64_t base{};
                mmio_read_callback read_cb{};
                mmio_write_callback write_cb{};
            };

            auto wrapper = std::make_unique<mmio_wrapper>();
            wrapper->base = address;
            wrapper->read_cb = std::move(read_cb);
            wrapper->write_cb = std::move(write_cb);

            auto* ptr = wrapper.get();
            this->storage_.push_back(std::move(wrapper));

            auto* read_wrapper = +[](void* user, const uint64_t addr, void* data, const size_t length) {
                const auto* w = static_cast<mmio_wrapper*>(user);
                w->read_cb(addr - w->base, data, length);
            };

            auto* write_wrapper = +[](void* user, const uint64_t addr, const void* data, const size_t length) {
                const auto* w = static_cast<mmio_wrapper*>(user);
                w->write_cb(addr + w->base, data, length);
            };

            icicle_map_mmio(this->emu_, address, size, read_wrapper, ptr, write_wrapper, ptr);
        }

        void map_memory(const uint64_t address, const size_t size, memory_permission permissions) override
        {
            const auto res = icicle_map_memory(this->emu_, address, size, static_cast<uint8_t>(permissions));
            ice(res, "Failed to map memory");
        }

        void unmap_memory(const uint64_t address, const size_t size) override
        {
            const auto res = icicle_unmap_memory(this->emu_, address, size);
            ice(res, "Failed to unmap memory");
        }

        bool try_read_memory(const uint64_t address, void* data, const size_t size) const override
        {
            return icicle_read_memory(this->emu_, address, data, size);
        }

        void read_memory(const uint64_t address, void* data, const size_t size) const override
        {
            const auto res = this->try_read_memory(address, data, size);
            ice(res, "Failed to read memory");
        }

        void write_memory(const uint64_t address, const void* data, const size_t size) override
        {
            const auto res = icicle_write_memory(this->emu_, address, data, size);
            ice(res, "Failed to write memory");
        }

        void apply_memory_protection(const uint64_t address, const size_t size, memory_permission permissions) override
        {
            const auto res = icicle_protect_memory(this->emu_, address, size, static_cast<uint8_t>(permissions));
            ice(res, "Failed to apply permissions");
        }

        emulator_hook* hook_instruction(int instruction_type, instruction_hook_callback callback) override
        {
            if (static_cast<x86_hookable_instructions>(instruction_type) != x86_hookable_instructions::syscall)
            {
                // TODO
                return nullptr;
            }

            auto obj = make_function_object(std::move(callback), this->is_in_hook_);
            auto* ptr = obj.get();

            const auto invoker = +[](void* cb) {
                const auto& func = *static_cast<decltype(ptr)>(cb);
                (void)func(0); //
            };

            const auto id = icicle_add_syscall_hook(this->emu_, invoker, ptr);
            this->hooks_[id] = std::move(obj);

            return wrap_hook(id);
        }

        emulator_hook* hook_basic_block(basic_block_hook_callback callback) override
        {
            auto object = make_function_object(std::move(callback), this->is_in_hook_);
            auto* ptr = object.get();
            auto* wrapper = +[](void* user, const uint64_t addr, const uint64_t instructions) {
                basic_block block{};
                block.address = addr;
                block.instruction_count = static_cast<size_t>(instructions);

                const auto& func = *static_cast<decltype(ptr)>(user);
                (func)(block);
            };

            const auto id = icicle_add_block_hook(this->emu_, wrapper, ptr);
            this->hooks_[id] = std::move(object);

            return wrap_hook(id);
        }

        emulator_hook* hook_interrupt(interrupt_hook_callback callback) override
        {
            auto obj = make_function_object(std::move(callback), this->is_in_hook_);
            auto* ptr = obj.get();
            auto* wrapper = +[](void* user, const int32_t code) {
                const auto& func = *static_cast<decltype(ptr)>(user);
                func(code);
            };

            const auto id = icicle_add_interrupt_hook(this->emu_, wrapper, ptr);
            this->hooks_[id] = std::move(obj);

            return wrap_hook(id);
        }

        emulator_hook* hook_memory_violation(memory_violation_hook_callback callback) override
        {
            auto obj = make_function_object(std::move(callback), this->is_in_hook_);
            auto* ptr = obj.get();
            auto* wrapper = +[](void* user, const uint64_t address, const uint8_t operation, const int32_t unmapped) -> int32_t {
                const auto violation_type = unmapped //
                                                ? memory_violation_type::unmapped
                                                : memory_violation_type::protection;

                const auto& func = *static_cast<decltype(ptr)>(user);
                const auto res = func(address, 1, static_cast<memory_operation>(operation), violation_type);
                return res == memory_violation_continuation::resume ? 1 : 0;
            };

            const auto id = icicle_add_violation_hook(this->emu_, wrapper, ptr);
            this->hooks_[id] = std::move(obj);

            return wrap_hook(id);
        }

        emulator_hook* hook_memory_execution(const uint64_t address, memory_execution_hook_callback callback) override
        {
            auto object = make_function_object(std::move(callback), this->is_in_hook_);
            auto* ptr = object.get();
            auto* wrapper = +[](void* user, const uint64_t addr) {
                const auto& func = *static_cast<decltype(ptr)>(user);
                (func)(addr);
            };

            const auto id = icicle_add_execution_hook(this->emu_, address, wrapper, ptr);
            this->hooks_[id] = std::move(object);

            return wrap_hook(id);
        }

        emulator_hook* hook_memory_execution(memory_execution_hook_callback callback) override
        {
            auto object = make_function_object(std::move(callback), this->is_in_hook_);
            auto* ptr = object.get();
            auto* wrapper = +[](void* user, const uint64_t addr) {
                const auto& func = *static_cast<decltype(ptr)>(user);
                (func)(addr);
            };

            const auto id = icicle_add_generic_execution_hook(this->emu_, wrapper, ptr);
            this->hooks_[id] = std::move(object);

            return wrap_hook(id);
        }

        emulator_hook* hook_memory_read(const uint64_t address, const uint64_t size, memory_access_hook_callback callback) override
        {
            return this->try_install_memory_access_hook(memory_access_hook{
                .address = address,
                .size = size,
                .callback = std::move(callback),
                .is_read = true,
            });
        }

        emulator_hook* hook_memory_write(const uint64_t address, const uint64_t size, memory_access_hook_callback callback) override
        {
            return this->try_install_memory_access_hook(memory_access_hook{
                .address = address,
                .size = size,
                .callback = std::move(callback),
                .is_read = false,
            });
        }

        void delete_hook(emulator_hook* hook) override
        {
            if (this->is_in_hook_)
            {
                this->hooks_to_delete_.insert(hook);
                this->schedule_action_execution();
            }
            else
            {
                this->delete_hook_internal(hook);
            }
        }

        void serialize_state(utils::buffer_serializer& buffer, const bool is_snapshot) const override
        {
            if (is_snapshot)
            {
                const auto snapshot = icicle_create_snapshot(this->emu_);
                buffer.write<uint32_t>(snapshot);
            }
            else
            {
                buffer.write_vector(this->save_registers());
            }
        }

        void deserialize_state(utils::buffer_deserializer& buffer, const bool is_snapshot) override
        {
            if (is_snapshot)
            {
                const auto snapshot = buffer.read<uint32_t>();
                icicle_restore_snapshot(this->emu_, snapshot);
            }
            else
            {
                const auto data = buffer.read_vector<std::byte>();
                this->restore_registers(data);
            }
        }

        std::vector<std::byte> save_registers() const override
        {
            std::vector<std::byte> data{};
            auto* accessor = +[](void* user, const void* data, const size_t length) {
                auto& vec = *static_cast<std::vector<std::byte>*>(user);
                vec.resize(length);
                memcpy(vec.data(), data, length);
            };

            icicle_save_registers(this->emu_, accessor, &data);

            return data;
        }

        void restore_registers(const std::vector<std::byte>& register_data) override
        {
            icicle_restore_registers(this->emu_, register_data.data(), register_data.size());
        }

        bool has_violation() const override
        {
            return false;
        }

        std::string get_name() const override
        {
            return "icicle-emu";
        }

      private:
        bool is_in_hook_{false};
        std::list<std::unique_ptr<utils::object>> storage_{};
        std::unordered_map<uint32_t, std::unique_ptr<utils::object>> hooks_{};
        std::unordered_map<emulator_hook*, std::optional<uint32_t>> id_mapping_{};
        icicle_emulator* emu_{};
        uint32_t index_{0};

        std::unordered_set<emulator_hook*> hooks_to_delete_{};
        std::unordered_map<emulator_hook*, memory_access_hook> hooks_to_install_{};

        emulator_hook* wrap_hook(const std::optional<uint32_t> icicle_id)
        {
            const auto id = ++this->index_;
            auto* hook = reinterpret_cast<emulator_hook*>(static_cast<size_t>(id));

            this->id_mapping_[hook] = icicle_id;

            return hook;
        }

        emulator_hook* hook_memory_access(memory_access_hook hook, emulator_hook* hook_id)
        {
            auto obj = make_function_object(std::move(hook.callback), this->is_in_hook_);
            auto* ptr = obj.get();
            auto* wrapper = +[](void* user, const uint64_t address, const void* data, size_t length) {
                const auto& func = *static_cast<decltype(ptr)>(user);
                func(address, data, length);
            };

            auto* installer = hook.is_read ? &icicle_add_read_hook : &icicle_add_write_hook;
            const auto id = installer(this->emu_, hook.address, hook.address + hook.size, wrapper, ptr);
            this->hooks_[id] = std::move(obj);

            if (hook_id)
            {
                this->id_mapping_[hook_id] = id;
                return hook_id;
            }

            return wrap_hook(id);
        }

        void delete_hook_internal(emulator_hook* hook)
        {
            auto hook_id = this->id_mapping_.find(hook);
            if (hook_id == this->id_mapping_.end())
            {
                return;
            }

            if (!hook_id->second.has_value())
            {
                this->hooks_to_delete_.insert(hook);
                return;
            }

            const auto id = *hook_id->second;
            this->id_mapping_.erase(hook_id);

            const auto entry = this->hooks_.find(id);
            if (entry == this->hooks_.end())
            {
                return;
            }

            icicle_remove_hook(this->emu_, id);
            const auto obj = std::move(entry->second);
            this->hooks_.erase(entry);
            (void)obj;
        }

        void perform_pending_actions()
        {
            auto hooks_to_install = std::move(this->hooks_to_install_);
            const auto hooks_to_delete = std::move(this->hooks_to_delete_);

            this->hooks_to_delete_ = {};
            this->hooks_to_install_ = {};

            for (auto& hook : hooks_to_install)
            {
                this->hook_memory_access(std::move(hook.second), hook.first);
            }

            for (auto* hook : hooks_to_delete)
            {
                this->delete_hook_internal(hook);
            }
        }

        emulator_hook* try_install_memory_access_hook(memory_access_hook hook)
        {
            if (!this->is_in_hook_)
            {
                return this->hook_memory_access(std::move(hook), nullptr);
            }

            auto* hook_id = wrap_hook(std::nullopt);
            this->hooks_to_install_[hook_id] = std::move(hook);

            this->schedule_action_execution();

            return hook_id;
        }

        void schedule_action_execution()
        {
            this->run_on_next_instruction([this] {
                this->perform_pending_actions(); //
            });
        }

        void run_on_next_instruction(std::function<void()> func) const
        {
            auto* heap_func = new std::function(std::move(func));
            auto* callback = +[](void* data) {
                auto* cb = static_cast<std::function<void()>*>(data);

                try
                {
                    (*cb)();
                }
                catch (...)
                {
                    // Ignore
                }

                delete cb;
            };

            icicle_run_on_next_instruction(this->emu_, callback, heap_func);
        }
    };

    std::unique_ptr<x86_64_emulator> create_x86_64_emulator()
    {
        return std::make_unique<icicle_x86_64_emulator>();
    }
}
