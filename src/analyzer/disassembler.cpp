#include "std_include.hpp"

#include "disassembler.hpp"

disassembler::disassembler()
{
    const auto res = cs_open(CS_ARCH_X86, CS_MODE_64, &this->handle_);
    if (res != CS_ERR_OK)
    {
        throw std::runtime_error("Failed to initialize capstone");
    }
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
        this->handle_ = obj.handle_;
        obj.handle_ = 0;
    }

    return *this;
}

void disassembler::release()
{
    if (this->handle_)
    {
        cs_close(&this->handle_);
        this->handle_ = 0;
    }
}

instructions disassembler::disassemble(const std::span<const uint8_t> data, const size_t count) const
{
    cs_insn* insts{};
    const auto inst_count = cs_disasm(this->handle_, data.data(), data.size(), count, 0, &insts);
    return instructions{std::span(insts, inst_count)};
}

void instructions::release()
{
    if (!this->instructions_.empty())
    {
        cs_free(this->instructions_.data(), this->instructions_.size());
    }

    this->instructions_ = {};
}
