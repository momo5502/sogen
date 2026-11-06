#include "../std_include.hpp"
#include "registry_state_file.hpp"
#include "registry_manager.hpp"

#include <atomic>
#include <bit>
#include <fstream>
#include <system_error>
#include <utils/finally.hpp>

#ifdef OS_WINDOWS
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace sogen
{
    namespace
    {
        constexpr std::array<char, 8> magic{'S', 'O', 'G', 'R', 'E', 'G', '0', '1'};
        constexpr uint32_t version = 1;
        constexpr size_t max_payload = 16 * 1024 * 1024;
        constexpr uint64_t max_entries = 65536;

        struct header
        {
            std::array<char, 8> signature = magic;
            uint32_t format = version;
            uint32_t reserved{};
            uint64_t size{};
        };

        static_assert(sizeof(header) == 24);
        static_assert(std::endian::native == std::endian::little);

        [[noreturn]] void fail(const char* message)
        {
#ifdef OS_WINDOWS
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), message);
#else
            throw std::system_error(errno, std::generic_category(), message);
#endif
        }

        class native_file
        {
          public:
#ifdef OS_WINDOWS
            HANDLE value = INVALID_HANDLE_VALUE;
#else
            int value = -1;
#endif
            native_file() = default;
            native_file(const native_file&) = delete;
            native_file& operator=(const native_file&) = delete;
            native_file(native_file&&) = delete;
            native_file& operator=(native_file&&) = delete;

            ~native_file()
            {
                this->close();
            }

            void close()
            {
#ifdef OS_WINDOWS
                if (this->value != INVALID_HANDLE_VALUE)
                {
                    CloseHandle(this->value);
                    this->value = INVALID_HANDLE_VALUE;
                }
#else
                if (this->value != -1)
                {
                    ::close(this->value);
                    this->value = -1;
                }
#endif
            }

            bool create(const std::filesystem::path& path)
            {
#ifdef OS_WINDOWS
                this->value = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
                return this->value != INVALID_HANDLE_VALUE;
#else
                this->value = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
                return this->value != -1;
#endif
            }

            void write(std::span<const std::byte> bytes) const
            {
                while (!bytes.empty())
                {
#ifdef OS_WINDOWS
                    DWORD written{};
                    const auto count = static_cast<DWORD>(std::min<size_t>(bytes.size(), 1024 * 1024));
                    if (!WriteFile(this->value, bytes.data(), count, &written, nullptr))
                    {
                        fail("Writing registry state failed");
                    }
#else
                    const auto written = ::write(this->value, bytes.data(), bytes.size());
                    if (written < 0 && errno == EINTR)
                    {
                        continue;
                    }
                    if (written < 0)
                    {
                        fail("Writing registry state failed");
                    }
#endif
                    if (written == 0)
                    {
                        throw std::runtime_error("No progress writing registry state");
                    }
                    bytes = bytes.subspan(static_cast<size_t>(written));
                }
            }

            void close_checked()
            {
#ifdef OS_WINDOWS
                auto* const file = std::exchange(this->value, INVALID_HANDLE_VALUE);
                if (!CloseHandle(file))
#else
                const auto file = std::exchange(this->value, -1);
                if (::close(file) != 0)
#endif
                {
                    fail("Closing registry state failed");
                }
            }

            void flush() const
            {
#ifdef OS_WINDOWS
                if (!FlushFileBuffers(this->value))
                {
                    fail("Flushing registry state failed");
                }
#else
                while (::fsync(this->value) != 0)
                {
                    if (errno != EINTR)
                    {
                        fail("Flushing registry state failed");
                    }
                }
#endif
            }
        };

        uint64_t count(utils::buffer_deserializer& buffer, uint64_t& budget)
        {
            const auto value = buffer.read<uint64_t>();
            if (value > budget)
            {
                throw std::runtime_error("Registry state has too many entries");
            }
            budget -= value;
            return value;
        }

        void text(utils::buffer_deserializer& buffer, const size_t width, const uint64_t limit)
        {
            const auto length = buffer.read<uint64_t>();
            if (length > limit || length > buffer.get_remaining_size() / width)
            {
                throw std::runtime_error("Invalid registry state string length");
            }
            buffer.read_data(static_cast<size_t>(length) * width);
        }

        void boolean(utils::buffer_deserializer& buffer)
        {
            if (buffer.read<uint8_t>() > 1)
            {
                throw std::runtime_error("Invalid registry state boolean");
            }
        }

        void validate(const std::span<const std::byte> payload)
        {
            utils::buffer_deserializer buffer{payload};
            uint64_t budget = max_entries;
            const auto buckets = count(buffer, budget);
            for (uint64_t i = 0; i < buckets; ++i)
            {
                text(buffer, sizeof(char16_t), 32768);
                const auto children = count(buffer, budget);
                for (uint64_t j = 0; j < children; ++j)
                {
                    text(buffer, sizeof(char), 131072);
                    boolean(buffer);
                }
                const auto values = count(buffer, budget);
                for (uint64_t j = 0; j < values; ++j)
                {
                    text(buffer, sizeof(char), 131072);
                    buffer.read<uint32_t>();
                    const auto size = buffer.read<uint64_t>();
                    if (size > max_payload || size > buffer.get_remaining_size())
                    {
                        throw std::runtime_error("Invalid registry state value length");
                    }
                    buffer.read_data(static_cast<size_t>(size));
                    boolean(buffer);
                }
            }
            const auto generations = count(buffer, budget);
            for (uint64_t i = 0; i < generations; ++i)
            {
                text(buffer, sizeof(char16_t), 32768);
                buffer.read<uint64_t>();
            }
            if (buffer.get_remaining_size() != 0)
            {
                throw std::runtime_error("Trailing data in registry state");
            }
        }

        void check_target(const std::filesystem::path& path)
        {
            std::error_code error{};
            const auto status = std::filesystem::symlink_status(path, error);
            if (status.type() == std::filesystem::file_type::not_found)
            {
                return;
            }
            if (error || !std::filesystem::is_regular_file(status) || std::filesystem::hard_link_count(path) != 1)
            {
                throw std::runtime_error("Registry state must be a regular file with one link");
            }
        }
    }

    struct registry_state_file::implementation
    {
        std::filesystem::path path;
        native_file lock{};

        explicit implementation(const std::filesystem::path& file)
            : path(std::filesystem::weakly_canonical(std::filesystem::absolute(file)))
        {
            check_target(this->path);
            auto name = this->path;
            name += ".lock";
            check_target(name);
#ifdef OS_WINDOWS
            this->lock.value = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (this->lock.value == INVALID_HANDLE_VALUE)
            {
                fail("Opening the registry state lock failed");
            }
            OVERLAPPED overlapped{};
            if (!LockFileEx(this->lock.value, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &overlapped))
            {
                fail("Registry state is already in use");
            }
#else
            this->lock.value = ::open(name.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
            if (this->lock.value < 0)
            {
                fail("Opening the registry state lock failed");
            }
            while (::flock(this->lock.value, LOCK_EX | LOCK_NB) != 0)
            {
                if (errno != EINTR)
                {
                    fail("Registry state is already in use");
                }
            }
#endif
        }
    };

    registry_state_file::registry_state_file(const std::filesystem::path& path)
        : implementation_(std::make_unique<implementation>(path))
    {
    }

    registry_state_file::~registry_state_file() = default;

    void registry_state_file::load(registry_manager& registry) const
    {
        const auto& path = this->implementation_->path;
        check_target(path);
        std::error_code error{};
        if (!std::filesystem::exists(path, error) && !error)
        {
            return;
        }
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        const auto size = input.tellg();
        if (!input || size < static_cast<std::streamoff>(sizeof(header)) ||
            size > static_cast<std::streamoff>(sizeof(header) + max_payload))
        {
            throw std::runtime_error("Invalid registry state file size");
        }
        input.seekg(0);
        header envelope{};
        input.read(reinterpret_cast<char*>(&envelope), sizeof(envelope));
        if (!input || envelope.signature != magic || envelope.format != version || envelope.reserved != 0 ||
            envelope.size != static_cast<uint64_t>(size) - sizeof(header))
        {
            throw std::runtime_error("Invalid registry state header");
        }
        std::vector<std::byte> payload(static_cast<size_t>(envelope.size));
        input.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
        if (!input || input.peek() != std::char_traits<char>::eof())
        {
            throw std::runtime_error("Reading registry state failed");
        }
        validate(payload);
        utils::buffer_deserializer buffer{payload};
        registry.deserialize_runtime_state(buffer);
    }

    void registry_state_file::save(const registry_manager& registry) const
    {
        std::vector<std::byte> payload{};
        utils::buffer_serializer buffer{[&](const std::span<const std::byte> bytes) {
            if (bytes.size() > max_payload - payload.size())
            {
                throw std::runtime_error("Registry state exceeds its size limit");
            }
            payload.insert(payload.end(), bytes.begin(), bytes.end());
        }};
        registry.serialize_runtime_state(buffer);
        buffer.flush();
        validate(payload);
        const auto& path = this->implementation_->path;
        check_target(path);
        static std::atomic<uint64_t> next{};
        auto temporary = path;
        native_file output{};
        bool created{};
        for (size_t attempt = 0; attempt < 64; ++attempt)
        {
            temporary = path;
#ifdef OS_WINDOWS
            const auto process = GetCurrentProcessId();
#else
            const auto process = ::getpid();
#endif
            temporary += ".tmp." + std::to_string(process) + "." + std::to_string(next.fetch_add(1));
            if (output.create(temporary))
            {
                created = true;
                break;
            }
#ifdef OS_WINDOWS
            if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS)
#else
            if (errno != EEXIST)
#endif
            {
                fail("Creating temporary registry state failed");
            }
        }
        if (!created)
        {
            throw std::runtime_error("No temporary registry state file is available");
        }
        const auto cleanup = utils::finally([&] {
            output.close();
            std::error_code ignored{};
            std::filesystem::remove(temporary, ignored);
        });
        header envelope{.size = payload.size()};
        output.write(std::as_bytes(std::span{&envelope, 1}));
        output.write(payload);
        output.flush();
        output.close_checked();
#ifdef OS_WINDOWS
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
#else
        if (::rename(temporary.c_str(), path.c_str()) != 0)
#endif
        {
            fail("Replacing registry state failed");
        }
    }
}
