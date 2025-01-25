#pragma once
#include "std_include.hpp"
#include "windows_path.hpp"

class file_system
{
  public:
    file_system(std::filesystem::path root, windows_path working_dir)
        : root_(std::move(root)),
          working_dir_(std::move(working_dir))
    {
    }

    std::filesystem::path translate(const windows_path& win_path) const
    {
        if (win_path.is_absolute())
        {
            return this->root_ / win_path.to_portable_path();
        }

        return this->root_ / (this->working_dir_ / win_path).to_portable_path();
    }

    void set_working_directory(windows_path working_dir)
    {
        this->working_dir_ = std::move(working_dir);
    }

    const windows_path& get_working_directory() const
    {
        return this->working_dir_;
    }

    windows_path local_to_windows_path(const std::filesystem::path& local_path) const
    {
        const auto absolute_local_path = absolute(local_path);
        const auto relative_path = relative(absolute_local_path, this->root_);

        if (relative_path.empty() || *relative_path.begin() == "..")
        {
            throw std::runtime_error("Path '" + local_path.string() + "' is not within the root filesystem!");
        }

        char drive{};
        std::list<std::u16string> folders{};

        for (auto i = relative_path.begin(); i != relative_path.end(); ++i)
        {
            if (i == relative_path.begin())
            {
                const auto str = i->string();
                assert(str.size() == 1);
                drive = str[0];
            }
            else
            {
                folders.push_back(i->u16string());
            }
        }

        return windows_path{drive, std::move(folders)};
    }

    void serialize(utils::buffer_serializer& buffer) const
    {
        buffer.write(this->working_dir_);
    }

    void deserialize(utils::buffer_deserializer& buffer)
    {
        buffer.read(this->working_dir_);
    }

  private:
    std::filesystem::path root_{};
    windows_path working_dir_{};
};
