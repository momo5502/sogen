#pragma once

#include <capstone/capstone.h>
#include <optional>
#include <span>

#include "common/segment_utils.hpp"

class emulator;

class instructions
{
  public:
    instructions() = default;
    ~instructions()
    {
        this->release();
    }

    instructions(instructions&& obj) noexcept
        : instructions()
    {
        this->operator=(std::move(obj));
    }

    instructions& operator=(instructions&& obj) noexcept
    {
        if (this != &obj)
        {
            this->release();
            this->instructions_ = obj.instructions_;
            obj.instructions_ = {};
        }

        return *this;
    }

    instructions(const instructions&) = delete;
    instructions& operator=(const instructions&) = delete;

    operator std::span<cs_insn>() const
    {
        return this->instructions_;
    }

    bool empty() const noexcept
    {
        return this->instructions_.empty();
    }

    size_t size() const noexcept
    {
        return this->instructions_.size();
    }

    const cs_insn* data() const noexcept
    {
        return this->instructions_.data();
    }

    const cs_insn& operator[](const size_t index) const
    {
        return this->instructions_[index];
    }

    auto begin() const
    {
        return this->instructions_.begin();
    }
    auto end() const
    {
        return this->instructions_.end();
    }

  private:
    friend class disassembler;
    std::span<cs_insn> instructions_{};

    explicit instructions(const std::span<cs_insn> insts)
        : instructions_(insts)
    {
    }

    void release();
};

class disassembler
{
  public:
    disassembler();
    ~disassembler();

    disassembler(disassembler&& obj) noexcept;
    disassembler& operator=(disassembler&& obj) noexcept;

    disassembler(const disassembler& obj) = delete;
    disassembler& operator=(const disassembler& obj) = delete;

    using segment_bitness = segment_utils::segment_bitness;

    instructions disassemble(emulator& cpu, uint16_t cs_selector, std::span<const uint8_t> data, size_t count) const;
    static std::optional<segment_bitness> get_segment_bitness(emulator& cpu, uint16_t cs_selector);
    csh resolve_handle(emulator& cpu, uint16_t cs_selector) const;

    csh get_handle_64() const
    {
        return this->handle_64_;
    }

    csh get_handle_32() const
    {
        return this->handle_32_;
    }

    csh get_handle_16() const
    {
        return this->handle_16_;
    }

  private:
    csh handle_64_{};
    csh handle_32_{};
    csh handle_16_{};

    void release();
};
