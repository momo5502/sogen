#include "disassembler.hpp"

#include <utils/finally.hpp>

namespace sogen
{
    namespace
    {
        void cse(const cs_err error)
        {
            if (error != CS_ERR_OK)
            {
                throw std::runtime_error(cs_strerror(error));
            }
        }
    }

    disassembler::disassembler()
    {
        auto deleter = utils::finally([&] { this->release(); });

        cse(cs_open(CS_ARCH_X86, CS_MODE_64, &this->handle_64_));
        cse(cs_option(this->handle_64_, CS_OPT_DETAIL, CS_OPT_ON));

        cse(cs_open(CS_ARCH_X86, CS_MODE_32, &this->handle_32_));
        cse(cs_option(this->handle_32_, CS_OPT_DETAIL, CS_OPT_ON));

        cse(cs_open(CS_ARCH_X86, CS_MODE_16, &this->handle_16_));
        cse(cs_option(this->handle_16_, CS_OPT_DETAIL, CS_OPT_ON));

        cse(cs_open(CS_ARCH_AARCH64, CS_MODE_LITTLE_ENDIAN, &this->handle_arm64_));
        cse(cs_option(this->handle_arm64_, CS_OPT_DETAIL, CS_OPT_ON));

        deleter.cancel();
    }

    disassembler::~disassembler()
    {
        this->release();
    }

    disassembler::disassembler(disassembler&& obj) noexcept
    {
        this->operator=(std::move(obj));
    }

    disassembler& disassembler::operator=(disassembler&& obj) noexcept
    {
        if (this != &obj)
        {
            this->release();
            this->handle_64_ = obj.handle_64_;
            this->handle_32_ = obj.handle_32_;
            this->handle_16_ = obj.handle_16_;
            this->handle_arm64_ = obj.handle_arm64_;
            obj.handle_64_ = 0;
            obj.handle_32_ = 0;
            obj.handle_16_ = 0;
            obj.handle_arm64_ = 0;
        }

        return *this;
    }

    void disassembler::release()
    {
        if (this->handle_64_)
        {
            cs_close(&this->handle_64_);
            this->handle_64_ = 0;
        }

        if (this->handle_32_)
        {
            cs_close(&this->handle_32_);
            this->handle_32_ = 0;
        }

        if (this->handle_16_)
        {
            cs_close(&this->handle_16_);
            this->handle_16_ = 0;
        }

        if (this->handle_arm64_)
        {
            cs_close(&this->handle_arm64_);
            this->handle_arm64_ = 0;
        }
    }

    instructions disassembler::disassemble(x86_64_cpu& cpu, const uint16_t cs_selector, const std::span<const uint8_t> data,
                                           const size_t count, const uint64_t address) const
    {
        const csh handle_to_use = this->resolve_handle(cpu, cs_selector);

        cs_insn* insts{};
        const auto inst_count = cs_disasm(handle_to_use, data.data(), data.size(), address, count, &insts);
        return instructions{std::span(insts, inst_count)};
    }

    std::optional<disassembler::segment_bitness> disassembler::get_segment_bitness(x86_64_cpu& cpu, const uint16_t cs_selector)
    {
        return segment_utils::get_segment_bitness(cpu, cs_selector);
    }

    csh disassembler::resolve_handle(x86_64_cpu& cpu, const uint16_t cs_selector) const
    {
        const auto mode = disassembler::get_segment_bitness(cpu, cs_selector);
        if (!mode)
        {
            return this->handle_64_;
        }

        switch (*mode)
        {
        case segment_bitness::bit16:
            return this->handle_16_;
        case segment_bitness::bit32:
            return this->handle_32_;
        case segment_bitness::bit64:
        default:
            return this->handle_64_;
        }
    }

    instructions disassembler::disassemble(guest_cpu& cpu, const std::span<const uint8_t> data, const size_t count,
                                           const uint64_t address) const
    {
        cs_insn* insts{};
        const auto inst_count = cs_disasm(this->resolve_handle(cpu), data.data(), data.size(), address, count, &insts);
        return instructions{std::span(insts, inst_count)};
    }

    csh disassembler::resolve_handle(guest_cpu& cpu) const
    {
        if (auto* x64 = dynamic_cast<x86_64_cpu*>(&cpu))
        {
            return this->resolve_handle(*x64, x64->reg<uint16_t>(x86_register::cs));
        }

        if (dynamic_cast<arm64_cpu*>(&cpu))
        {
            return this->handle_arm64_;
        }

        throw std::runtime_error("No disassembler for this guest CPU");
    }

    void instructions::release()
    {
        if (!this->instructions_.empty())
        {
            cs_free(this->instructions_.data(), this->instructions_.size());
        }

        this->instructions_ = {};
    }
} // namespace sogen
