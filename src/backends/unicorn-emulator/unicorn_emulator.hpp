#pragma once

#include <arch_emulator.hpp>

#include "unicorn.hpp"
#include "unicorn_hook.hpp"
#include "function_wrapper.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sogen::unicorn
{
    namespace detail
    {
        static_assert(static_cast<uint32_t>(memory_permission::none) == UC_PROT_NONE);
        static_assert(static_cast<uint32_t>(memory_permission::read) == UC_PROT_READ);
        static_assert(static_cast<uint32_t>(memory_permission::exec) == UC_PROT_EXEC);
        static_assert(static_cast<uint32_t>(memory_permission::all) == UC_PROT_ALL);

        inline memory_violation_type map_memory_violation_type(const uc_mem_type mem_type)
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

        inline memory_operation map_memory_operation(const uc_mem_type mem_type)
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

            virtual void release_hooks()
            {
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

            void release_hooks() override
            {
                for (auto& entry : this->hooks_)
                {
                    entry.hook.release();
                }
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

        inline void assert_64bit_limit(const size_t size)
        {
            if (size > sizeof(uint64_t))
            {
                throw std::runtime_error("Exceeded uint64_t size limit");
            }
        }

        inline uint64_t calc_end_address(const uint64_t address, uint64_t size)
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

        // Half-open, non-overlapping address ranges.
        class address_range_set
        {
          public:
            void add(const uint64_t begin, const uint64_t end)
            {
                if (begin >= end)
                {
                    return;
                }

                this->remove(begin, end);
                auto inserted = this->ranges_.emplace(begin, end).first;

                if (inserted != this->ranges_.begin())
                {
                    const auto previous = std::prev(inserted);
                    if (previous->second == begin)
                    {
                        previous->second = inserted->second;
                        this->ranges_.erase(inserted);
                        inserted = previous;
                    }
                }

                const auto next = std::next(inserted);
                if (next != this->ranges_.end() && next->first == inserted->second)
                {
                    inserted->second = next->second;
                    this->ranges_.erase(next);
                }
            }

            void remove(const uint64_t begin, const uint64_t end)
            {
                if (begin >= end)
                {
                    return;
                }

                auto entry = this->first_overlap(begin);
                while (entry != this->ranges_.end() && entry->first < end)
                {
                    const auto [range_begin, range_end] = *entry;
                    entry = this->ranges_.erase(entry);

                    if (range_begin < begin)
                    {
                        this->ranges_.emplace(range_begin, begin);
                    }
                    if (range_end > end)
                    {
                        this->ranges_.emplace(end, range_end);
                        break;
                    }
                }
            }

            template <typename F>
            void for_each_overlap(const uint64_t begin, const uint64_t end, F&& function) const
            {
                if (begin >= end)
                {
                    return;
                }

                for (auto entry = this->first_overlap(begin); entry != this->ranges_.end() && entry->first < end; ++entry)
                {
                    function(std::max(entry->first, begin), std::min(entry->second, end));
                }
            }

          private:
            std::map<uint64_t, uint64_t> ranges_{};

            std::map<uint64_t, uint64_t>::const_iterator first_overlap(const uint64_t address) const
            {
                auto entry = this->ranges_.upper_bound(address);
                if (entry != this->ranges_.begin() && std::prev(entry)->second > address)
                {
                    --entry;
                }
                return entry;
            }

            std::map<uint64_t, uint64_t>::iterator first_overlap(const uint64_t address)
            {
                auto entry = this->ranges_.upper_bound(address);
                if (entry != this->ranges_.begin() && std::prev(entry)->second > address)
                {
                    --entry;
                }
                return entry;
            }
        };
    }

    // Memory, hooks, context and lifecycle shared by every Unicorn guest architecture. Instruction
    // hooks, segmentation and system-register behavior belong to the architecture adapters.
    template <typename Base>
    class unicorn_emulator : public Base
    {
      public:
        using pointer_type = Base::pointer_type;

        unicorn_emulator(const uc_arch arch, const uc_mode mode, const std::optional<int> cpu_model = std::nullopt)
        {
            uce(uc_open(arch, mode, &this->uc_));

#ifndef OS_WINDOWS
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#endif
            if (cpu_model)
            {
                uce(uc_ctl_set_cpu_model(this->uc_, *cpu_model));
            }

            constexpr auto is_64_bit = sizeof(void*) >= 8;
            uce(uc_ctl_set_tcg_buffer_size(this->uc_, (is_64_bit ? 2 : 1) << 30 /* 2 gb */));

            uint32_t page_size{};
            uce(uc_ctl_get_page_size(this->uc_, &page_size));
            this->page_size_ = page_size;

#ifndef OS_WINDOWS
#pragma GCC diagnostic pop
#endif
        }

        ~unicorn_emulator() override
        {
            utils::reset_object_with_delayed_destruction(this->hooks_);
            uc_close(this->uc_);
        }

        unicorn_emulator(const unicorn_emulator&) = delete;
        unicorn_emulator& operator=(const unicorn_emulator&) = delete;
        unicorn_emulator(unicorn_emulator&&) = delete;
        unicorn_emulator& operator=(unicorn_emulator&&) = delete;

        void start(const size_t count) override
        {
            const auto start = this->violation_ip_.value_or(this->read_instruction_pointer());
            this->violation_ip_ = std::nullopt;
            this->hook_exception_ = nullptr;

            constexpr auto end = std::numeric_limits<uint64_t>::max();

            // uc_emu_start installs its instruction-count hook without discarding blocks translated
            // during uncounted runs, which would then run to their end; it only flushes on the way back.
            if (count > 0 && !this->counted_run_)
            {
                uce(uc_ctl_flush_tb(this->uc_));
            }
            this->counted_run_ = count > 0;

            this->in_emulation_ = true;
            const auto res = uc_emu_start(*this, start, end, 0, count);
            this->in_emulation_ = false;
            this->retired_hooks_.clear();

            if (this->hook_exception_)
            {
                std::rethrow_exception(std::exchange(this->hook_exception_, nullptr));
            }

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

        void map_mmio(const uint64_t address, const size_t size, mmio_read_callback read_cb, mmio_write_callback write_cb) override
        {
            auto read_wrapper = [c = std::move(read_cb)](uc_engine*, const uint64_t addr, const uint32_t s) {
                detail::assert_64bit_limit(s);
                uint64_t value{};
                c(addr, &value, s);
                return value;
            };

            auto write_wrapper = [c = std::move(write_cb)](uc_engine*, const uint64_t addr, const uint32_t s, const uint64_t value) {
                detail::assert_64bit_limit(s);
                c(addr, &value, s);
            };

            detail::mmio_callbacks cb{
                .read = detail::mmio_callbacks::read_wrapper(std::move(read_wrapper)),
                .write = detail::mmio_callbacks::write_wrapper(std::move(write_wrapper)),
            };

            uce(uc_mmio_map(*this, address, size, cb.read.get_c_function(), cb.read.get_user_data(), cb.write.get_c_function(),
                            cb.write.get_user_data()));

            this->mmio_[address] = std::move(cb);
        }

        void map_memory(const uint64_t address, const size_t size, memory_permission permissions) override
        {
            uce(uc_mem_map(*this, address, size, static_cast<uint32_t>(permissions)));
            this->track_executable(address, size, permissions);
        }

        bool supports_host_memory_mapping() const override
        {
            return true;
        }

        bool map_host_memory(const uint64_t address, const size_t size, void* host_pointer, memory_permission permissions) override
        {
            uce(uc_mem_map_ptr(*this, address, size, static_cast<uint32_t>(permissions), host_pointer));
            this->track_executable(address, size, permissions);
            return true;
        }

        void unmap_memory(const uint64_t address, const size_t size) override
        {
            uce(uc_mem_unmap(*this, address, size));
            this->executable_.remove(address, address + size);

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
            if (uc_mem_write(*this, address, data, size) != UC_ERR_OK)
            {
                return false;
            }

            this->invalidate_translations(address, size);
            return true;
        }

        void write_memory(const uint64_t address, const void* data, const size_t size) override
        {
            uce(uc_mem_write(*this, address, data, size));
            this->invalidate_translations(address, size);
        }

        void apply_memory_protection(const uint64_t address, const size_t size, memory_permission permissions) override
        {
            uce(uc_mem_protect(*this, address, size, static_cast<uint32_t>(permissions)));
            this->track_executable(address, size, permissions);
        }

        emulator_hook* hook_basic_block(basic_block_hook_callback callback) override
        {
            function_wrapper<void, uc_engine*, uint64_t, size_t> wrapper(
                [c = std::move(callback), this](uc_engine*, const uint64_t address, const size_t size) {
                    basic_block block{};
                    block.address = address;
                    block.size = size;

                    this->run_hook_guarded([&] { c(*this, block); });
                });

            unicorn_hook hook{*this};

            uce(uc_hook_add(*this, hook.make_reference(), UC_HOOK_BLOCK, wrapper.get_function(), wrapper.get_user_data(), 0,
                            std::numeric_limits<pointer_type>::max()));

            return this->add_hook(std::move(wrapper), std::move(hook));
        }

        emulator_hook* hook_memory_violation(memory_violation_hook_callback callback) override
        {
            function_wrapper<bool, uc_engine*, uc_mem_type, uint64_t, int, int64_t> wrapper(
                [c = std::move(callback), this](uc_engine*, const uc_mem_type type, const uint64_t address, const int size, const int64_t) {
                    const auto ip = this->read_instruction_pointer();

                    assert(size >= 0);
                    const auto operation = detail::map_memory_operation(type);
                    const auto violation = detail::map_memory_violation_type(type);

                    std::optional<memory_violation_continuation> continuation{};
                    this->run_hook_guarded([&] { continuation = c(*this, address, static_cast<uint64_t>(size), operation, violation); });
                    if (!continuation)
                    {
                        return false;
                    }

                    const auto result = *continuation;
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

            uce(uc_hook_add(*this, hook.make_reference(), UC_HOOK_MEM_INVALID, wrapper.get_function(), wrapper.get_user_data(), 0,
                            std::numeric_limits<uint64_t>::max()));

            return this->add_hook(std::move(wrapper), std::move(hook));
        }

        emulator_hook* hook_memory_range_execution(const uint64_t address, const uint64_t size,
                                                   memory_execution_hook_callback callback) override
        {
            auto exec_wrapper = [c = std::move(callback), this](uc_engine*, const uint64_t address, const uint32_t /*size*/) {
                const auto old_ip = this->read_instruction_pointer();
                this->run_hook_guarded([&] { c(*this, address); });
                if (this->hook_exception_)
                {
                    return;
                }

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
                            detail::calc_end_address(address, size)));

            auto* result = this->add_hook(std::move(wrapper), std::move(hook));
            this->code_hook_ranges_[result] = {address, size};
            return result;
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
                const auto operation = detail::map_memory_operation(type);
                if (operation == memory_operation::read && length > 0)
                {
                    this->run_hook_guarded([&] { c(*this, address, &value, std::min(static_cast<size_t>(length), sizeof(value))); });
                }
            };

            function_wrapper<void, uc_engine*, uc_mem_type, uint64_t, int, int64_t> wrapper(std::move(read_wrapper));

            unicorn_hook hook{*this};

            uce(uc_hook_add(*this, hook.make_reference(), UC_HOOK_MEM_READ_AFTER, wrapper.get_function(), wrapper.get_user_data(), address,
                            detail::calc_end_address(address, size)));

            return this->add_hook(std::move(wrapper), std::move(hook));
        }

        emulator_hook* hook_memory_write(const uint64_t address, const uint64_t size, memory_access_hook_callback callback) override
        {
            auto write_wrapper = [c = std::move(callback), this](uc_engine*, const uc_mem_type type, const uint64_t addr, const int length,
                                                                 const uint64_t value) {
                const auto operation = detail::map_memory_operation(type);
                if (operation == memory_operation::write && length > 0)
                {
                    this->run_hook_guarded([&] { c(*this, addr, &value, std::min(static_cast<size_t>(length), sizeof(value))); });
                }
            };

            function_wrapper<void, uc_engine*, uc_mem_type, uint64_t, int, int64_t> wrapper(std::move(write_wrapper));

            unicorn_hook hook{*this};

            uce(uc_hook_add(*this, hook.make_reference(), UC_HOOK_MEM_WRITE, wrapper.get_function(), wrapper.get_user_data(), address,
                            detail::calc_end_address(address, size)));

            return this->add_hook(std::move(wrapper), std::move(hook));
        }

        void delete_hook(emulator_hook* hook) override
        {
            const auto entry = std::ranges::find_if(
                this->hooks_, [&](const std::unique_ptr<detail::hook_object>& hook_ptr) { return hook_ptr->as_opaque_hook() == hook; });

            if (entry != this->hooks_.end())
            {
                auto obj = std::move(*entry);
                this->hooks_.erase(entry);
                obj->release_hooks();

                // Unicorn only discards blocks that start inside a code hook's range; a block that enters
                // the range part-way keeps an inlined call to the deleted callback.
                if (const auto range = this->code_hook_ranges_.find(hook); range != this->code_hook_ranges_.end())
                {
                    const auto [address, size] = range->second;
                    this->code_hook_ranges_.erase(range);
                    this->invalidate_translations(address, static_cast<size_t>(std::min<uint64_t>(size, UINT64_MAX - address)));
                }

                // A hook may delete itself from its own callback; keep its wrapper alive until Unicorn returns.
                if (this->in_emulation_)
                {
                    this->retired_hooks_.push_back(std::move(obj));
                }
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

            const detail::uc_context_serializer serializer(this->uc_, is_snapshot);
            serializer.serialize(buffer);
        }

        void deserialize_state(utils::buffer_deserializer& buffer, const bool is_snapshot) override
        {
            if (this->has_snapshots_ && !is_snapshot)
            {
                // TODO: Investigate if this is really necessary
                throw std::runtime_error("Unable to deserialize after snapshot was taken!");
            }

            const detail::uc_context_serializer serializer(this->uc_, is_snapshot);
            serializer.deserialize(buffer);
        }

        std::vector<std::byte> save_registers() const override
        {
            utils::buffer_serializer buffer{};
            const detail::uc_context_serializer serializer(this->uc_, false);
            serializer.serialize(buffer);
            return buffer.move_buffer();
        }

        void restore_registers(const std::vector<std::byte>& register_data) override
        {
            if (!this->saved_register_size_)
            {
                this->saved_register_size_ = this->save_registers().size();
            }

            if (register_data.size() != *this->saved_register_size_)
            {
                throw std::invalid_argument("Invalid Unicorn saved register state");
            }

            utils::buffer_deserializer buffer{register_data};
            const detail::uc_context_serializer serializer(this->uc_, false);
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

      protected:
        template <typename T>
        emulator_hook* add_hook(T data, unicorn_hook hook)
        {
            auto container = std::make_unique<detail::hook_container>();
            container->add(std::move(data), std::move(hook));

            auto* result = container->as_opaque_hook();
            this->hooks_.push_back(std::move(container));
            return result;
        }

        bool is_emulating() const
        {
            return this->in_emulation_;
        }

        // A C++ exception must not unwind through Unicorn's translation loop, which would leave the
        // engine mid-execution. Hooks that may fail record the exception, stop the CPU and let
        // start() rethrow it once Unicorn has returned.
        template <typename F>
        void run_hook_guarded(F&& function) noexcept
        {
            try
            {
                std::forward<F>(function)();
            }
            catch (...)
            {
                this->hook_exception_ = std::current_exception();
                (void)uc_emu_stop(*this);
            }
        }

      private:
        mutable bool has_snapshots_{false};
        uc_engine* uc_{};
        uint64_t page_size_{};
        std::optional<uint64_t> violation_ip_{};
        std::optional<size_t> saved_register_size_{};
        std::exception_ptr hook_exception_{};
        std::vector<std::unique_ptr<detail::hook_object>> hooks_{};
        std::vector<std::unique_ptr<detail::hook_object>> retired_hooks_{};
        std::unordered_map<emulator_hook*, std::pair<uint64_t, uint64_t>> code_hook_ranges_{};
        bool in_emulation_{};
        bool counted_run_{};
        std::unordered_map<uint64_t, detail::mmio_callbacks> mmio_{};
        detail::address_range_set executable_{};

        void track_executable(const uint64_t address, const size_t size, const memory_permission permissions)
        {
            if (is_executable(permissions))
            {
                this->executable_.add(address, address + size);
            }
            else
            {
                this->executable_.remove(address, address + size);
            }
        }

        // Unicorn only detects guest stores to translated code. Host writes must discard stale blocks
        // explicitly; the invalidation translates the address as a fetch, so it is limited to
        // executable pages and issued per page because adjacent mappings need not be contiguous.
        void invalidate_translations(const uint64_t address, const size_t size)
        {
            this->executable_.for_each_overlap(address, address + size, [this](const uint64_t begin, const uint64_t end) {
                for (auto page = begin & ~(this->page_size_ - 1); page < end; page += this->page_size_)
                {
                    uce(uc_ctl_remove_cache(this->uc_, std::max(page, begin), std::min(page + this->page_size_, end)));
                }
            });
        }
    };
} // namespace sogen::unicorn
