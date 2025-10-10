#pragma once

#include <arch_emulator.hpp>

#include "memory_manager.hpp"
#include "memory_utils.hpp"
#include "address_utils.hpp"
#include "x86_register.hpp"
#include "common/segment_utils.hpp"

#include <utils/time.hpp>

namespace network
{
    struct socket_factory;
}

// TODO: Replace with pointer handling structure for future 32 bit support
using emulator_pointer = uint64_t;

template <typename T>
class object_wrapper
{
    T* obj_;

  public:
    object_wrapper(T& obj)
        : obj_(&obj)
    {
    }

    T& get() const
    {
        return *this->obj_;
    }

    operator T&() const
    {
        return this->get();
    }

    void serialize(utils::buffer_serializer&) const
    {
    }

    void deserialize(utils::buffer_deserializer&)
    {
    }
};

class windows_emulator;
class module_manager;
struct process_context;

using clock_wrapper = object_wrapper<utils::clock>;
using x64_emulator_wrapper = object_wrapper<x86_64_emulator>;
using memory_manager_wrapper = object_wrapper<memory_manager>;
using module_manager_wrapper = object_wrapper<module_manager>;
using process_context_wrapper = object_wrapper<process_context>;
using windows_emulator_wrapper = object_wrapper<windows_emulator>;
using socket_factory_wrapper = object_wrapper<network::socket_factory>;

template <typename T>
class emulator_object
{
  public:
    using value_type = T;

    emulator_object(const x64_emulator_wrapper& wrapper, const uint64_t address = 0)
        : emulator_object(wrapper.get(), address)
    {
    }

    emulator_object(memory_interface& memory, const uint64_t address = 0)
        : memory_(&memory),
          address_(address)
    {
    }

    emulator_object(emulator& emu, const void* address)
        : emulator_object(emu, reinterpret_cast<uint64_t>(address))
    {
    }

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

    T read(const size_t index = 0) const
    {
        T obj{};
        this->memory_->read_memory(this->address_ + index * this->size(), &obj, sizeof(obj));
        return obj;
    }

    void write(const T& value, const size_t index = 0) const
    {
        this->memory_->write_memory(this->address_ + index * this->size(), &value, sizeof(value));
    }

    void write_if_valid(const T& value, const size_t index = 0) const
    {
        if (this->operator bool())
        {
            this->write(value, index);
        }
    }

    template <typename F>
    void access_safe(const F& accessor, const size_t index = 0) const
    {
        auto obj = std::make_unique<T>();
        this->access_object(accessor, *obj, index);
    }

    template <typename F>
    void access(const F& accessor, const size_t index = 0) const
    {
        if constexpr (sizeof(T) < 0x4000)
        {
            T obj{};
            this->access_object(accessor, obj, index);
        }
        else
        {
            this->access_safe(accessor, index);
        }
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

    template <typename F>
    void access_object(const F& accessor, T& obj, const size_t index = 0) const
    {
        this->memory_->read_memory(this->address_ + index * this->size(), &obj, sizeof(obj));

        accessor(obj);

        this->write(obj, index);
    }
};

// TODO: warning emulator_utils is hardcoded for 64bit unicode_string usage
class emulator_allocator
{
  public:
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

    template <typename T>
    emulator_object<T> reserve_page_aligned(const size_t count = 1)
    {
        constexpr auto page_aligned_size = page_align_up(sizeof(T));
        const auto potential_start = this->reserve(page_aligned_size * count, 0x1000);
        return emulator_object<T>(*this->memory_, potential_start);
    }

    uint64_t copy_string(const std::u16string_view str)
    {
        UNICODE_STRING<EmulatorTraits<Emu64>> uc_str{};
        this->make_unicode_string(uc_str, str);
        return uc_str.Buffer;
    }

    void make_unicode_string(UNICODE_STRING<EmulatorTraits<Emu64>>& result, const std::u16string_view str,
                             const std::optional<size_t> maximum_length = std::nullopt)
    {
        constexpr auto element_size = sizeof(str[0]);
        constexpr auto required_alignment = alignof(decltype(str[0]));
        const auto total_length = str.size() * element_size;
        const auto total_buffer_length = total_length + element_size;

        const auto max_length = std::max(maximum_length.value_or(total_buffer_length), total_buffer_length);

        const auto string_buffer = this->reserve(max_length, required_alignment);

        this->memory_->write_memory(string_buffer, str.data(), total_length);

        constexpr std::array<char, element_size> nullbyte{};
        this->memory_->write_memory(string_buffer + total_length, nullbyte.data(), nullbyte.size());

        result.Buffer = string_buffer;
        result.Length = static_cast<USHORT>(total_length);
        result.MaximumLength = static_cast<USHORT>(max_length);
    }

    emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> make_unicode_string(const std::u16string_view str,
                                                                               const std::optional<size_t> maximum_length = std::nullopt)
    {
        const auto unicode_string = this->reserve<UNICODE_STRING<EmulatorTraits<Emu64>>>();

        unicode_string.access([&](UNICODE_STRING<EmulatorTraits<Emu64>>& unicode_str) {
            this->make_unicode_string(unicode_str, str, maximum_length); //
        });

        return unicode_string;
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

    void release(memory_manager& manager)
    {
        if (this->address_ && this->size_)
        {
            // TODO: Make all sizes uint64_t
            manager.release_memory(this->address_, static_cast<size_t>(this->size_));
            this->address_ = 0;
            this->size_ = 0;
        }
    }

    void skip(const uint64_t bytes)
    {
        this->active_address_ += bytes;
    }

    void skip_until(const uint64_t offset)
    {
        this->active_address_ = this->address_ + offset;
    }

  private:
    memory_interface* memory_{};
    uint64_t address_{};
    uint64_t size_{};
    uint64_t active_address_{0};
};

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

inline std::u16string read_unicode_string(const emulator& emu, const UNICODE_STRING<EmulatorTraits<Emu64>> ucs)
{
    static_assert(offsetof(UNICODE_STRING<EmulatorTraits<Emu64>>, Length) == 0);
    static_assert(offsetof(UNICODE_STRING<EmulatorTraits<Emu64>>, MaximumLength) == 2);
    static_assert(offsetof(UNICODE_STRING<EmulatorTraits<Emu64>>, Buffer) == 8);
    static_assert(sizeof(UNICODE_STRING<EmulatorTraits<Emu64>>) == 16);

    std::u16string result{};
    result.resize(ucs.Length / 2);

    emu.read_memory(ucs.Buffer, result.data(), ucs.Length);

    return result;
}

inline std::u16string read_unicode_string(const emulator& emu, const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> uc_string)
{
    const auto ucs = uc_string.read();
    return read_unicode_string(emu, ucs);
}

inline std::u16string read_unicode_string(emulator& emu, const uint64_t uc_string)
{
    return read_unicode_string(emu, emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>>{emu, uc_string});
}

inline void copy_unicode_string_64_to_32(memory_interface& memory, UNICODE_STRING<EmulatorTraits<Emu32>>& dest32,
                                         const UNICODE_STRING<EmulatorTraits<Emu64>>& src64, const uint64_t dest_base_address,
                                         uint32_t& offset, const uint32_t max_size)
{
    dest32.Length = static_cast<uint16_t>(src64.Length);
    dest32.MaximumLength = static_cast<uint16_t>(src64.MaximumLength);

    if (!src64.Buffer || src64.Length == 0)
    {
        dest32.Buffer = 0;
        return;
    }

    offset = static_cast<uint32_t>(align_up(offset, 2));

    if (offset + src64.Length > max_size)
    {
        dest32.Buffer = 0;
        return;
    }

    dest32.Buffer = static_cast<uint32_t>(dest_base_address + offset);

    std::vector<std::byte> string_data(src64.Length);
    memory.read_memory(src64.Buffer, string_data.data(), src64.Length);
    memory.write_memory(dest_base_address + offset, string_data.data(), src64.Length);

    offset += src64.MaximumLength;
}

inline uint64_t get_function_argument(x86_64_emulator& emu, const size_t index, const bool is_syscall = false)
{
    bool use_32bit_stack = false;

    if (!is_syscall)
    {
        const auto cs_selector = emu.reg<uint16_t>(x86_register::cs);
        const auto bitness = segment_utils::get_segment_bitness(emu, cs_selector);
        use_32bit_stack = bitness && *bitness == segment_utils::segment_bitness::bit32;
    }

    if (use_32bit_stack)
    {
        const auto esp = emu.reg<uint32_t>(x86_register::esp);
        const auto address = static_cast<uint64_t>(esp) + static_cast<uint64_t>((index + 1) * sizeof(uint32_t));
        return static_cast<uint64_t>(emu.read_memory<uint32_t>(address));
    }

    switch (index)
    {
    case 0:
        return emu.reg(is_syscall ? x86_register::r10 : x86_register::rcx);
    case 1:
        return emu.reg(x86_register::rdx);
    case 2:
        return emu.reg(x86_register::r8);
    case 3:
        return emu.reg(x86_register::r9);
    default:
        return emu.read_stack(index + 1);
    }
}
