#pragma once

#include <arch_emulator.hpp>
#include <memory_interface.hpp>
#include <memory_permission.hpp>
#include <address_utils.hpp>
#include <serialization.hpp>
#include <platform/elf.hpp>

#include <utils/time.hpp>

// --------------------------------------------------------------------------
// emulator_pointer
// --------------------------------------------------------------------------

using emulator_pointer = uint64_t;

// --------------------------------------------------------------------------
// emulator_object<T> — typed accessor for emulated memory (duplicated from
// windows-emulator, identical logic, no Windows dependencies)
// --------------------------------------------------------------------------

template <typename T>
class emulator_object
{
  public:
    using value_type = T;

    emulator_object(memory_interface& memory, const uint64_t address = 0)
        : memory_(&memory),
          address_(address)
    {
    }

    emulator_object() = default;

    uint64_t value() const
    {
        return this->address_;
    }

    constexpr uint64_t size() const
    {
        return sizeof(T);
    }

    uint64_t end() const
    {
        return this->value() + this->size();
    }

    explicit operator bool() const
    {
        return this->address_ != 0;
    }

    std::optional<T> try_read(const size_t index = 0) const
    {
        T obj{};
        if (this->memory_->try_read_memory(this->address_ + index * this->size(), &obj, sizeof(obj)))
        {
            return obj;
        }
        return std::nullopt;
    }

    T read(const size_t index = 0) const
    {
        T obj{};
        this->memory_->read_memory(this->address_ + index * this->size(), &obj, sizeof(obj));
        return obj;
    }

    bool try_write(const T& value, const size_t index = 0) const
    {
        return this->memory_->try_write_memory(this->address_ + index * this->size(), &value, sizeof(value));
    }

    void write(const T& value, const size_t index = 0) const
    {
        this->memory_->write_memory(this->address_ + index * this->size(), &value, sizeof(value));
    }

    template <typename F>
    void access(const F& accessor, const size_t index = 0) const
    {
        T obj{};
        this->memory_->read_memory(this->address_ + index * this->size(), &obj, sizeof(obj));
        accessor(obj);
        this->write(obj, index);
    }

    void serialize(utils::buffer_serializer& buffer) const
    {
        buffer.write(this->address_);
    }

    void deserialize(utils::buffer_deserializer& buffer)
    {
        buffer.read(this->address_);
    }

    void set_address(const uint64_t address)
    {
        this->address_ = address;
    }

    emulator_object<T> shift(const int64_t offset) const
    {
        return emulator_object<T>(*this->memory_, this->address_ + offset);
    }

    memory_interface* get_memory_interface() const
    {
        return this->memory_;
    }

  private:
    memory_interface* memory_{};
    uint64_t address_{};
};

// --------------------------------------------------------------------------
// emulator_allocator — bump allocator over emulated memory (Linux version,
// no UNICODE_STRING methods)
// --------------------------------------------------------------------------

class emulator_allocator
{
  public:
    emulator_allocator() = default;

    emulator_allocator(memory_interface& memory)
        : memory_(&memory)
    {
    }

    emulator_allocator(memory_interface& memory, const uint64_t address, const uint64_t size)
        : memory_(&memory),
          address_(address),
          size_(size),
          active_address_(address)
    {
    }

    uint64_t reserve(const uint64_t count, const uint64_t alignment = 1)
    {
        const auto potential_start = align_up(this->active_address_, alignment);
        const auto potential_end = potential_start + count;
        const auto total_end = this->address_ + this->size_;

        if (potential_end > total_end)
        {
            throw std::runtime_error("Out of memory");
        }

        this->active_address_ = potential_end;

        return potential_start;
    }

    template <typename T>
    emulator_object<T> reserve(const size_t count = 1)
    {
        const auto potential_start = this->reserve(sizeof(T) * count, alignof(T));
        return emulator_object<T>(*this->memory_, potential_start);
    }

    uint64_t get_base() const
    {
        return this->address_;
    }

    uint64_t get_size() const
    {
        return this->size_;
    }

    uint64_t get_next_address() const
    {
        return this->active_address_;
    }

    memory_interface& get_memory() const
    {
        return *this->memory_;
    }

    void serialize(utils::buffer_serializer& buffer) const
    {
        buffer.write(this->address_);
        buffer.write(this->size_);
        buffer.write(this->active_address_);
    }

    void deserialize(utils::buffer_deserializer& buffer)
    {
        buffer.read(this->address_);
        buffer.read(this->size_);
        buffer.read(this->active_address_);
    }

    void skip(const uint64_t bytes)
    {
        this->active_address_ += bytes;
    }

  private:
    memory_interface* memory_{};
    uint64_t address_{};
    uint64_t size_{};
    uint64_t active_address_{0};
};

// --------------------------------------------------------------------------
// Linux syscall argument accessor (System V convention)
// --------------------------------------------------------------------------

inline uint64_t get_linux_syscall_argument(x86_64_emulator& emu, const size_t index)
{
    switch (index)
    {
    case 0:
        return emu.reg(x86_register::rdi);
    case 1:
        return emu.reg(x86_register::rsi);
    case 2:
        return emu.reg(x86_register::rdx);
    case 3:
        return emu.reg(x86_register::r10);
    case 4:
        return emu.reg(x86_register::r8);
    case 5:
        return emu.reg(x86_register::r9);
    default:
        throw std::runtime_error("Linux syscalls have at most 6 register arguments");
    }
}

// --------------------------------------------------------------------------
// ELF segment permission helper
// --------------------------------------------------------------------------

inline memory_permission elf_segment_to_permission(uint32_t p_flags)
{
    memory_permission perm = memory_permission::none;

    if (p_flags & elf::PF_R)
    {
        perm = perm | memory_permission::read;
    }
    if (p_flags & elf::PF_W)
    {
        perm = perm | memory_permission::write;
    }
    if (p_flags & elf::PF_X)
    {
        perm = perm | memory_permission::exec;
    }

    return perm;
}

// --------------------------------------------------------------------------
// String reader for emulated memory
// --------------------------------------------------------------------------

template <typename Element>
std::basic_string<Element> read_string(memory_interface& mem, const uint64_t address, const std::optional<size_t> size = {})
{
    std::basic_string<Element> result{};

    for (size_t i = 0;; ++i)
    {
        if (size && i >= *size)
        {
            break;
        }

        Element element{};
        mem.read_memory(address + (i * sizeof(element)), &element, sizeof(element));

        if (!size && !element)
        {
            break;
        }

        result.push_back(element);
    }

    return result;
}
