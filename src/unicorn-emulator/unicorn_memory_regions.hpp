#pragma once

#include <span>

#include "unicorn.hpp"

namespace unicorn
{
    class unicorn_memory_regions
    {
      public:
        unicorn_memory_regions(uc_engine* uc)
        {
            uce(uc_mem_regions(uc, &this->regions_, &this->count_));
        }

        ~unicorn_memory_regions()
        {
            this->release();
        }

        unicorn_memory_regions(const unicorn_memory_regions&) = delete;
        unicorn_memory_regions& operator=(const unicorn_memory_regions&) = delete;

        unicorn_memory_regions(unicorn_memory_regions&& obj) noexcept
        {
            this->operator=(std::move(obj));
        }

        unicorn_memory_regions& operator=(unicorn_memory_regions&& obj) noexcept
        {
            if (this != &obj)
            {
                this->release();

                this->count_ = obj.count_;
                this->regions_ = obj.regions_;

                obj.count_ = {};
                obj.regions_ = nullptr;
            }

            return *this;
        }

        std::span<uc_mem_region> get_span() const
        {
            return {this->regions_, this->count_};
        }

      private:
        uint32_t count_{};
        uc_mem_region* regions_{};

        void release()
        {
            if (this->regions_)
            {
                uc_free(regions_);
            }

            this->count_ = {};
            this->regions_ = nullptr;
        }
    };
}
