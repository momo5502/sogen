#define ICICLE_EMULATOR_IMPL
#include "icicle_x64_emulator.hpp"

#include <utils/object.hpp>

using icicle_emulator = struct icicle_emulator_;

extern "C"
{
    using raw_func = void(void*);
    using ptr_func = void(void*, uint64_t);
    using data_accessor_func = void(void* user, const void* data, size_t length);

    using icicle_mmio_read_func = void(void* user, uint64_t address, size_t length, void* data);
    using icicle_mmio_write_func = void(void* user, uint64_t address, size_t length, const void* data);

    icicle_emulator* icicle_create_emulator();
    int32_t icicle_protect_memory(icicle_emulator*, uint64_t address, uint64_t length, uint8_t permissions);
    int32_t icicle_map_memory(icicle_emulator*, uint64_t address, uint64_t length, uint8_t permissions);
    int32_t icicle_map_mmio(icicle_emulator*, uint64_t address, uint64_t length, icicle_mmio_read_func* read_callback,
                            void* read_data, icicle_mmio_write_func* write_callback, void* write_data);
    int32_t icicle_unmap_memory(icicle_emulator*, uint64_t address, uint64_t length);
    int32_t icicle_read_memory(icicle_emulator*, uint64_t address, void* data, size_t length);
    int32_t icicle_write_memory(icicle_emulator*, uint64_t address, const void* data, size_t length);
    int32_t icicle_save_registers(icicle_emulator*, data_accessor_func* accessor, void* accessor_data);
    int32_t icicle_restore_registers(icicle_emulator*, const void* data, size_t length);
    uint32_t icicle_add_syscall_hook(icicle_emulator*, raw_func* callback, void* data);
    uint32_t icicle_add_execution_hook(icicle_emulator*, ptr_func* callback, void* data);
    void icicle_remove_syscall_hook(icicle_emulator*, uint32_t id);
    size_t icicle_read_register(icicle_emulator*, int reg, void* data, size_t length);
    size_t icicle_write_register(icicle_emulator*, int reg, const void* data, size_t length);
    void icicle_start(icicle_emulator*);
    void icicle_stop(icicle_emulator*);
    void icicle_destroy_emulator(icicle_emulator*);
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
        struct function_object : std::function<T>, utils::object
        {
            using std::function<T>::function;
            ~function_object() override = default;
        };

        template <typename T>
        std::unique_ptr<utils::object> wrap_shared(std::shared_ptr<T> shared_ptr)
        {
            struct shard_wrapper : utils::object
            {
                std::shared_ptr<T> ptr{};
                ~shard_wrapper() override = default;
            };

            auto wrapper = std::make_unique<shard_wrapper>();
            wrapper->ptr = std::move(shared_ptr);

            return wrapper;
        }
    }

    class icicle_x64_emulator : public x64_emulator
    {
      public:
        icicle_x64_emulator()
            : emu_(icicle_create_emulator())
        {
            if (!this->emu_)
            {
                throw std::runtime_error("Failed to create icicle emulator instance");
            }
        }

        ~icicle_x64_emulator() override
        {
            if (this->emu_)
            {
                icicle_destroy_emulator(this->emu_);
                this->emu_ = nullptr;
            }
        }

        void start(const uint64_t start, const uint64_t end, std::chrono::nanoseconds timeout,
                   const size_t count) override
        {
            if (timeout.count() < 0)
            {
                timeout = {};
            }

            (void)start;
            (void)end;
            (void)count;
            icicle_start(this->emu_);
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

            this->write_register(x64_register::gdtr, &entry.limit, 12);
        }

        void set_segment_base(const x64_register base, const pointer_type value) override
        {
            switch (base)
            {
            case x64_register::fs:
            case x64_register::fs_base:
                this->reg(x64_register::fs_base, value);
                break;
            case x64_register::gs:
            case x64_register::gs_base:
                this->reg(x64_register::gs_base, value);
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

        void map_mmio(const uint64_t address, const size_t size, mmio_read_callback read_cb,
                      mmio_write_callback write_cb) override
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

            auto* read_wrapper = +[](void* user, const uint64_t addr, const size_t length, void* data) {
                constexpr auto limit = sizeof(uint64_t);
                const auto* w = static_cast<mmio_wrapper*>(user);

                // TODO: Change interface to get rid of loop
                for (size_t offset = 0; offset < length; offset += limit)
                {
                    const auto max_read = std::min(limit, length - offset);
                    const auto value = w->read_cb(addr + offset - w->base, max_read);
                    memcpy(static_cast<uint8_t*>(data) + offset, &value, max_read);
                }
            };

            auto* write_wrapper = +[](void* user, const uint64_t addr, const size_t length, const void* data) {
                constexpr auto limit = sizeof(uint64_t);
                const auto* w = static_cast<mmio_wrapper*>(user);

                // TODO: Change interface to get rid of loop
                for (size_t offset = 0; offset < length; offset += limit)
                {
                    uint64_t value{};
                    const auto max_read = std::min(limit, length - offset);
                    memcpy(&value, static_cast<const uint8_t*>(data) + offset, max_read);
                    w->write_cb(addr + offset - w->base, max_read, value);
                }
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
            if (static_cast<x64_hookable_instructions>(instruction_type) != x64_hookable_instructions::syscall)
            {
                // TODO
                return nullptr;
            }

            auto callback_store = std::make_unique<function_object<void()>>([c = std::move(callback)] {
                (void)c(); //
            });

            const auto invoker = +[](void* cb) {
                (*static_cast<function_object<void()>*>(cb))(); //
            };

            const auto id = icicle_add_syscall_hook(this->emu_, invoker, callback_store.get());
            this->hooks_[id] = std::move(callback_store);

            return reinterpret_cast<emulator_hook*>(static_cast<size_t>(id));
        }

        emulator_hook* hook_basic_block(basic_block_hook_callback callback) override
        {
            // TODO
            (void)callback;
            throw std::runtime_error("Not implemented");
        }

        emulator_hook* hook_edge_generation(edge_generation_hook_callback callback) override
        {
            // TODO
            (void)callback;
            throw std::runtime_error("Not implemented");
        }

        emulator_hook* hook_interrupt(interrupt_hook_callback callback) override
        {
            // TODO
            (void)callback;
            return nullptr;
            // throw std::runtime_error("Not implemented");
        }

        emulator_hook* hook_memory_violation(uint64_t address, size_t size,
                                             memory_violation_hook_callback callback) override
        {
            // TODO
            (void)address;
            (void)size;
            (void)callback;
            return nullptr;
            // throw std::runtime_error("Not implemented");
        }

        emulator_hook* hook_memory_access(const uint64_t address, const size_t size, const memory_operation filter,
                                          complex_memory_hook_callback callback) override
        {
            if (filter == memory_permission::none)
            {
                return nullptr;
            }

            auto shared_callback = std::make_shared<complex_memory_hook_callback>(std::move(callback));

            if ((filter & memory_permission::exec) == memory_permission::exec)
            {
                if (address != 0 || size != std::numeric_limits<size_t>::max())
                {
                    throw std::runtime_error("Not supported!");
                }

                auto* ptr = shared_callback.get();
                auto wrapper = wrap_shared(shared_callback);
                auto* func = +[](void* user, const uint64_t ptr) {
                    (*static_cast<complex_memory_hook_callback*>(user))(ptr, 0, 0, memory_permission::exec);
                };

                const auto id = icicle_add_execution_hook(this->emu_, func, ptr);
                this->hooks_[id] = std::move(wrapper);

                return reinterpret_cast<emulator_hook*>(static_cast<size_t>(id));
            }

            return nullptr;
        }

        void delete_hook(emulator_hook* hook) override
        {
            const auto id = static_cast<uint32_t>(reinterpret_cast<size_t>(hook));
            const auto entry = this->hooks_.find(id);
            if (entry == this->hooks_.end())
            {
                return;
            }

            icicle_remove_syscall_hook(this->emu_, id);
            this->hooks_.erase(entry);
        }

        void serialize_state(utils::buffer_serializer& buffer, const bool is_snapshot) const override
        {
            if (is_snapshot)
            {
                throw std::runtime_error("Not implemented");
            }

            buffer.write_vector(this->save_registers());
        }

        void deserialize_state(utils::buffer_deserializer& buffer, const bool is_snapshot) override
        {
            if (is_snapshot)
            {
                throw std::runtime_error("Not implemented");
            }

            const auto data = buffer.read_vector<std::byte>();
            this->restore_registers(data);
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

      private:
        std::list<std::unique_ptr<utils::object>> storage_{};
        std::unordered_map<uint32_t, std::unique_ptr<utils::object>> hooks_{};
        icicle_emulator* emu_{};
    };

    std::unique_ptr<x64_emulator> create_x64_emulator()
    {
        return std::make_unique<icicle_x64_emulator>();
    }
}
