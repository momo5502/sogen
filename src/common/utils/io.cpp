#include "io.hpp"
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

namespace sogen
{

    namespace utils::io
    {
        bool remove_file(const std::filesystem::path& file)
        {
            std::error_code ec{};
            return std::filesystem::remove(file, ec) && !ec;
        }

        bool move_file(const std::filesystem::path& src, const std::filesystem::path& target)
        {
            copy_folder(src, target);
            return remove_file(src);
        }

        bool file_exists(const std::filesystem::path& file)
        {
            return std::ifstream(file).good();
        }

        bool write_file(const std::filesystem::path& file, const std::span<const std::byte> data, const bool append)
        {
            if (file.has_parent_path())
            {
                io::create_directory(file.parent_path());
            }

            std::ofstream stream(file, std::ios::binary | std::ofstream::out | (append ? std::ofstream::app : std::ofstream::out));

            if (stream.is_open())
            {
                stream.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
                stream.close();
                return true;
            }

            return false;
        }

        std::vector<std::byte> read_file(const std::filesystem::path& file)
        {
            std::vector<std::byte> data{};
            read_file(file, &data);
            return data;
        }

        bool read_file(const std::filesystem::path& file, std::vector<std::byte>* data)
        {
            if (!data)
            {
                return false;
            }

            data->clear();

            std::error_code ec{};
            const bool regular = std::filesystem::is_regular_file(file, ec);

            if (regular && !ec)
            {
                const auto size = std::filesystem::file_size(file, ec);
                if (!ec)
                {
                    if (size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max()))
                    {
                        return false;
                    }

                    data->resize(static_cast<std::size_t>(size));

                    std::ifstream file_stream(file, std::ios::binary);
                    if (!file_stream)
                    {
                        data->clear();
                        return false;
                    }

                    if (size == 0)
                    {
                        return true;
                    }

                    file_stream.read(reinterpret_cast<char*>(data->data()), static_cast<std::streamsize>(data->size()));
                    if (static_cast<std::size_t>(file_stream.gcount()) != data->size())
                    {
                        data->clear();
                        return false;
                    }

                    return true;
                }
            }

            std::ifstream file_stream(file, std::ios::binary);
            if (!file_stream)
            {
                return false;
            }

            constexpr std::size_t chunk_size = 0x1000;
            std::vector<char> buffer(chunk_size);

            while (file_stream)
            {
                file_stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                const auto bytes_read = file_stream.gcount();

                if (bytes_read > 0)
                {
                    const auto* begin = reinterpret_cast<const std::byte*>(buffer.data());
                    data->insert(data->end(), begin, begin + bytes_read);
                }
            }

            return !file_stream.bad();
        }

        std::size_t file_size(const std::filesystem::path& file)
        {
            std::ifstream stream(file, std::ios::binary);

            if (stream)
            {
                stream.seekg(0, std::ios::end);
                return static_cast<std::size_t>(stream.tellg());
            }

            return 0;
        }

        bool create_directory(const std::filesystem::path& directory)
        {
            std::error_code ec{};
            return std::filesystem::create_directories(directory, ec) && !ec;
        }

        bool directory_exists(const std::filesystem::path& directory)
        {
            std::error_code ec{};
            return std::filesystem::is_directory(directory, ec) && !ec;
        }

        bool directory_is_empty(const std::filesystem::path& directory)
        {
            std::error_code ec{};
            return std::filesystem::is_empty(directory, ec) && !ec;
        }

        void copy_folder(const std::filesystem::path& src, const std::filesystem::path& target)
        {
            std::error_code ec{};
            std::filesystem::copy(src, target, std::filesystem::copy_options::overwrite_existing | std::filesystem::copy_options::recursive,
                                  ec);
        }

        std::vector<std::filesystem::path> list_files(const std::filesystem::path& directory, const bool recursive)
        {
            std::error_code code{};
            std::vector<std::filesystem::path> files;

            if (recursive)
            {
                for (const auto& file : std::filesystem::recursive_directory_iterator(directory, code))
                {
                    files.push_back(file.path());
                }
            }
            else
            {
                for (const auto& file : std::filesystem::directory_iterator(directory, code))
                {
                    files.push_back(file.path());
                }
            }

            return files;
        }
    }
} // namespace sogen
