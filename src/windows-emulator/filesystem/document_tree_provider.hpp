#pragma once

#include "mount_provider.hpp"

#include <unordered_map>

namespace sogen
{

    struct document_entry
    {
        // Relative to the tree, '/'-separated, in the case the provider reported.
        std::u16string path{};
        std::string id{};
        bool is_directory{};
        uint64_t size{};
        int64_t modified_ms{};

        void serialize(utils::buffer_serializer& buffer) const;
        void deserialize(utils::buffer_deserializer& buffer);
    };

    class document_source
    {
      public:
        virtual ~document_source() = default;

        virtual std::unique_ptr<i_file> open(const std::string& document_id) = 0;
    };

    // Presents a document tree that was listed once up front, e.g. an Android Storage Access Framework grant, as a
    // read-only mount. Changes the document provider makes later are not seen. Entries whose paths differ only in case
    // cannot be told apart by the guest, so construction rejects them instead of picking one.
    class document_tree_provider final : public mount_provider
    {
      public:
        document_tree_provider(std::vector<document_entry> entries, std::shared_ptr<document_source> source);

        std::optional<struct compat_stat> stat(const windows_path& path) const override;
        std::vector<directory_entry> list(const windows_path& path) const override;
        open_result open(const windows_path& path, const std::u16string& mode) override;
        NTSTATUS create_directory(const windows_path& path) override;
        NTSTATUS rename(const windows_path& from, const windows_path& to) override;
        NTSTATUS remove(const windows_path& path) override;

      private:
        std::optional<std::filesystem::path> get_host_path(const windows_path& path) const override;

        const document_entry* find(const windows_path& path) const;

        std::vector<document_entry> entries_{};
        std::unordered_map<windows_path, size_t> by_path_{};
        std::unordered_map<windows_path, std::vector<size_t>> children_{};
        std::shared_ptr<document_source> source_{};
    };

} // namespace sogen
