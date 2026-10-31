#pragma once

#include "document_tree_provider.hpp"

#include <functional>
#include <list>
#include <mutex>
#include <thread>

namespace sogen
{

    class unique_fd
    {
      public:
        unique_fd() = default;
        explicit unique_fd(int fd);
        ~unique_fd();

        unique_fd(const unique_fd&) = delete;
        unique_fd& operator=(const unique_fd&) = delete;
        unique_fd(unique_fd&& other) noexcept;
        unique_fd& operator=(unique_fd&& other) noexcept;

        int get() const
        {
            return this->fd_;
        }

        explicit operator bool() const
        {
            return this->fd_ >= 0;
        }

      private:
        int fd_{-1};
    };

    // Reads by offset with a position of its own: the descriptor may be shared with other files and other processes,
    // which share its file offset too.
    class descriptor_file final : public i_file
    {
      public:
        descriptor_file(std::shared_ptr<const unique_fd> fd, int64_t size);

        size_t read(std::span<std::byte> buffer) override;
        size_t write(std::span<const std::byte> buffer) override;
        bool flush() override;

        bool seek(int64_t offset, int origin) override;
        int64_t tell() override;
        int64_t size() override;
        bool resize(uint64_t size) override;

        std::optional<struct compat_stat> stat() override;

      private:
        std::shared_ptr<const unique_fd> fd_{};
        int64_t size_{};
        int64_t position_{};
    };

    // Opening a document through Android's ContentResolver takes milliseconds, so the most recently used descriptors stay
    // open. Only regular files are accepted: some document providers hand out pipes, which cannot be read by offset.
    class descriptor_document_source final : public document_source
    {
      public:
        using opener = std::function<unique_fd(const std::string& document_id)>;

        descriptor_document_source(opener open_document, size_t capacity);

        std::unique_ptr<i_file> open(const std::string& document_id) override;
        std::shared_ptr<const unique_fd> open_descriptor(const std::string& document_id);

      private:
        struct cached_descriptor
        {
            std::string document_id{};
            std::shared_ptr<const unique_fd> fd{};
        };

        opener open_document_{};
        size_t capacity_{};
        std::mutex mutex_{};
        std::list<cached_descriptor> descriptors_{};
    };

    // Lets child processes, which cannot reach the document provider themselves, list a document tree and receive
    // descriptors of its files over a Unix socket. Clients must run as the same user and present the token.
    class document_broker
    {
      public:
        document_broker(std::vector<document_entry> entries, std::shared_ptr<descriptor_document_source> source);
        ~document_broker();

        document_broker(const document_broker&) = delete;
        document_broker& operator=(const document_broker&) = delete;
        document_broker(document_broker&&) = delete;
        document_broker& operator=(document_broker&&) = delete;

        const std::string& name() const
        {
            return this->name_;
        }

        const std::string& token() const
        {
            return this->token_;
        }

      private:
        std::vector<document_entry> entries_{};
        std::unordered_map<std::string, size_t> files_{};
        std::shared_ptr<descriptor_document_source> source_{};
        std::string name_{};
        std::string token_{};
        unique_fd listener_{};

        std::mutex mutex_{};
        bool stopping_{};
        std::vector<int> clients_{};
        std::vector<std::thread> threads_{};
        std::thread accept_thread_{};

        void accept_clients();
        void serve(unique_fd client);
    };

    struct brokered_document_tree
    {
        std::vector<document_entry> entries{};
        std::shared_ptr<document_source> source{};
    };

    brokered_document_tree connect_document_broker(const std::string& name, const std::string& token);

} // namespace sogen
