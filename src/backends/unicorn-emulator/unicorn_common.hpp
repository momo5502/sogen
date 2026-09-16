#pragma once

#include <memory>
#include <vector>

#include <hook_interface.hpp>
#include <serialization.hpp>

#include "platform/compiler.hpp"

#include "unicorn.hpp"
#include "unicorn_hook.hpp"
#include "function_wrapper.hpp"

namespace sogen::unicorn
{
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

    inline void assert_64bit_limit(const size_t size)
    {
        if (size > sizeof(uint64_t))
        {
            throw std::runtime_error("Exceeded uint64_t size limit");
        }
    }
} // namespace sogen::unicorn
