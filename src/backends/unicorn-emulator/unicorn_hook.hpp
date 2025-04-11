#pragma once

#include "unicorn.hpp"

namespace unicorn
{
    class unicorn_hook
    {
      public:
        unicorn_hook() = default;

        unicorn_hook(uc_engine* uc)
            : unicorn_hook(uc, {})
        {
        }

        unicorn_hook(uc_engine* uc, const uc_hook hook)
            : uc_(uc),
              hook_(hook)
        {
        }

        ~unicorn_hook()
        {
            release();
        }

        unicorn_hook(const unicorn_hook&) = delete;
        unicorn_hook& operator=(const unicorn_hook&) = delete;

        unicorn_hook(unicorn_hook&& obj) noexcept
        {
            this->operator=(std::move(obj));
        }

        uc_hook* make_reference()
        {
            if (!this->uc_)
            {
                throw std::runtime_error("Cannot make reference on default constructed hook");
            }

            this->release();
            return &this->hook_;
        }

        unicorn_hook& operator=(unicorn_hook&& obj) noexcept
        {
            if (this != &obj)
            {
                this->release();

                this->uc_ = obj.uc_;
                this->hook_ = obj.hook_;

                obj.hook_ = {};
                obj.uc_ = {};
            }

            return *this;
        }

        void release()
        {
            if (this->hook_ && this->uc_)
            {
                uc_hook_del(this->uc_, this->hook_);
                this->hook_ = {};
            }
        }

      private:
        uc_engine* uc_{};
        uc_hook hook_{};
    };
}
