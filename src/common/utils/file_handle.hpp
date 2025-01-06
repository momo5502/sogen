#pragma once

#include <cstdio>
#include <type_traits>

namespace utils
{
    class file_handle
    {
      public:
        file_handle() = default;

        file_handle(FILE* file)
            : file_(file)
        {
        }

        ~file_handle()
        {
            this->release();
        }

        file_handle(const file_handle&) = delete;
        file_handle& operator=(const file_handle&) = delete;

        file_handle(file_handle&& obj) noexcept
            : file_handle()
        {
            this->operator=(std::move(obj));
        }

        file_handle& operator=(file_handle&& obj) noexcept
        {
            if (this != &obj)
            {
                this->release();
                this->file_ = obj.file_;
                obj.file_ = {};
            }

            return *this;
        }

        file_handle& operator=(FILE* file) noexcept
        {
            this->release();
            this->file_ = file;

            return *this;
        }

        [[nodiscard]] operator bool() const
        {
            return this->file_;
        }

        [[nodiscard]] operator FILE*() const
        {
            return this->file_;
        }

        [[nodiscard]] int64_t size() const
        {
            const auto current_position = this->tell();

            this->seek_to(0, SEEK_END);
            const auto size = this->tell();
            this->seek_to(current_position);

            return size;
        }

        bool seek_to(const int64_t position, const int origin = SEEK_SET) const
        {
            return _fseeki64(this->file_, position, origin) == 0;
        }

        [[nodiscard]] int64_t tell() const
        {
            return _ftelli64(this->file_);
        }

      private:
        FILE* file_{};

        void release()
        {
            if (this->file_)
            {
                (void)fclose(this->file_);
                this->file_ = {};
            }
        }
    };
}
