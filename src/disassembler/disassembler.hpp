#pragma once

#include <array>
#include <capstone/capstone.h>
#include <optional>
#include <span>

#include "arch_emulator.hpp"
#include "segment_utils.hpp"
#include "x86_register.hpp"

namespace sogen
{

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

        instructions disassemble(emulator& cpu, uint16_t cs_selector, std::span<const uint8_t> data, size_t count,
                                 uint64_t address = 0) const;
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

    inline bool read_x86_register_value(x86_64_emulator& cpu, x86_reg reg, uint64_t& value)
    {
        switch (reg)
        {
        case X86_REG_RAX:
            value = cpu.reg<uint64_t>(x86_register::rax);
            return true;
        case X86_REG_RCX:
            value = cpu.reg<uint64_t>(x86_register::rcx);
            return true;
        case X86_REG_RDX:
            value = cpu.reg<uint64_t>(x86_register::rdx);
            return true;
        case X86_REG_RBX:
            value = cpu.reg<uint64_t>(x86_register::rbx);
            return true;
        case X86_REG_RSP:
            value = cpu.reg<uint64_t>(x86_register::rsp);
            return true;
        case X86_REG_RBP:
            value = cpu.reg<uint64_t>(x86_register::rbp);
            return true;
        case X86_REG_RSI:
            value = cpu.reg<uint64_t>(x86_register::rsi);
            return true;
        case X86_REG_RDI:
            value = cpu.reg<uint64_t>(x86_register::rdi);
            return true;
        case X86_REG_R8:
            value = cpu.reg<uint64_t>(x86_register::r8);
            return true;
        case X86_REG_R9:
            value = cpu.reg<uint64_t>(x86_register::r9);
            return true;
        case X86_REG_R10:
            value = cpu.reg<uint64_t>(x86_register::r10);
            return true;
        case X86_REG_R11:
            value = cpu.reg<uint64_t>(x86_register::r11);
            return true;
        case X86_REG_R12:
            value = cpu.reg<uint64_t>(x86_register::r12);
            return true;
        case X86_REG_R13:
            value = cpu.reg<uint64_t>(x86_register::r13);
            return true;
        case X86_REG_R14:
            value = cpu.reg<uint64_t>(x86_register::r14);
            return true;
        case X86_REG_R15:
            value = cpu.reg<uint64_t>(x86_register::r15);
            return true;
        default:
            return false;
        }
    }

    inline bool resolve_jump_target(x86_64_emulator& cpu, uint64_t& target)
    {
        disassembler d{};
        const auto cs_selector = cpu.reg<uint16_t>(x86_register::cs);

        for (size_t depth = 0; depth < 8; ++depth)
        {
            std::array<uint8_t, 16> bytes{};
            if (!cpu.try_read_memory(target, bytes.data(), bytes.size()))
            {
                return false;
            }

            const auto insts = d.disassemble(cpu, cs_selector, std::span<const uint8_t>(bytes.data(), bytes.size()), 1, target);
            if (insts.empty())
            {
                return false;
            }

            const auto& inst = insts[0];
            if (inst.id != X86_INS_JMP)
            {
                return true;
            }

            const auto* detail = inst.detail;
            if (!detail || detail->x86.op_count == 0)
            {
                return true;
            }

            const auto& op = detail->x86.operands[0];
            switch (op.type)
            {
            case X86_OP_IMM:
                target = static_cast<uint64_t>(op.imm);
                continue;
            case X86_OP_MEM: {
                uint64_t address = 0;
                if (op.mem.base == X86_REG_RIP)
                {
                    address = target + inst.size + static_cast<uint64_t>(op.mem.disp);
                }
                else if (op.mem.base == X86_REG_INVALID && op.mem.index == X86_REG_INVALID)
                {
                    address = static_cast<uint64_t>(op.mem.disp);
                }
                else
                {
                    return true;
                }

                if (!cpu.try_read_memory(address, &target, sizeof(target)))
                {
                    return false;
                }
                continue;
            }
            case X86_OP_REG:
                if (!read_x86_register_value(cpu, op.reg, target))
                {
                    return true;
                }
                continue;
            default:
                return true;
            }
        }

        return true;
    }

} // namespace sogen
