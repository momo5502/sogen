#include "std_include.hpp"
#include "disassembler.hpp"
#include <utils/finally.hpp>

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

    cse(cs_open(CS_ARCH_X86, CS_MODE_64, &this->handle_));
    cse(cs_option(this->handle_, CS_OPT_DETAIL, CS_OPT_ON));

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
