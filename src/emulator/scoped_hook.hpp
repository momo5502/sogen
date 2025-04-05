#pragma once
#include "emulator.hpp"

class scoped_hook
{
  public:
    scoped_hook() = default;

    scoped_hook(emulator& emu, emulator_hook* hook)
        : scoped_hook(emu, std::vector{hook})
    {
    }

    scoped_hook(emulator& emu, std::vector<emulator_hook*> hooks)
        : emu_(&emu),
          hooks_(std::move(hooks))
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
            this->hooks_ = std::move(obj.hooks_);

            obj.hooks_ = {};
        }

        return *this;
    }

    void remove()
    {
        auto hooks = std::move(this->hooks_);
        this->hooks_ = {};

        for (auto* hook : hooks_)
        {
            try
            {
                this->emu_->delete_hook(hook);
            }
            catch (...)
            {
            }
        }
    }

  private:
    emulator* emu_{};
    std::vector<emulator_hook*> hooks_{};
};
