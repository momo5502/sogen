#pragma once
#include "emulator.hpp"

class scoped_hook
{
  public:
    scoped_hook() = default;

    scoped_hook(emulator& emu, emulator_hook* hook)
        : emu_(&emu),
          hook_(hook)
    {
    }

    ~scoped_hook()
    {
        this->remove();
    }

    scoped_hook(const scoped_hook&) = delete;
    scoped_hook& operator=(const scoped_hook&) = delete;

    scoped_hook(scoped_hook&& obj) noexcept
    {
        this->operator=(std::move(obj));
    }

    scoped_hook& operator=(scoped_hook&& obj) noexcept
    {
        if (this != &obj)
        {
            this->remove();
            this->emu_ = obj.emu_;
            this->hook_ = obj.hook_;

            obj.hook_ = {};
        }

        return *this;
    }

    void remove()
    {
        if (this->hook_)
        {
            this->emu_->delete_hook(this->hook_);
            this->hook_ = {};
        }
    }

  private:
    emulator* emu_{};
    emulator_hook* hook_{};
};
