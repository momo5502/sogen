#pragma once

#include "../host_range_reader.hpp"
#include "../std_include.hpp"

#include <functional>
#include <map>
#include <memory>

#include <memory_permission.hpp>
#include <platform/macho.hpp>

namespace sogen
{
    struct dyld_cache_region
    {
        uint64_t address{};
        uint64_t size{};
        uint64_t file_offset{};
        uint64_t flags{};
        memory_permission initial_permissions{};
        memory_permission max_permissions{};
        bool has_slide_info{};
    };

    struct dyld_cache_file_info
    {
        std::filesystem::path path{};
        std::array<uint8_t, 16> uuid{};
        uint64_t vm_offset{};
        std::vector<dyld_cache_region> regions{};
    };

    struct dyld_cache_image_entry
    {
        uint64_t address{};
        uint64_t text_size{};
        std::string path{};
    };

    // A shared cache is 5.4 GiB across a dozen files while its metadata is a few hundred kilobytes of the
    // first one, so the reader pulls ranges instead of whole files. Implementations return the bytes present
    // in [offset, offset + size) - possibly fewer, empty past the end - and must never allocate beyond what
    // they hold: the counts driving these reads come from the untrusted header.
    class dyld_cache_data_source
    {
      public:
        virtual ~dyld_cache_data_source() = default;

        virtual std::vector<std::byte> read(uint64_t offset, uint64_t size) = 0;
    };

    using dyld_cache_source_opener = std::function<std::unique_ptr<dyld_cache_data_source>(const std::filesystem::path& path)>;

    std::unique_ptr<dyld_cache_data_source> open_dyld_cache_file(const std::filesystem::path& path);
    std::unique_ptr<dyld_cache_data_source> make_dyld_cache_memory_source(std::vector<std::byte> data);

    // Reads only what the parser asks for. The real cache is 5.4 GB across a dozen files, and a browser
    // has no way to hold that -- parsing has to be a matter of headers and tables, not of streaming.
    std::unique_ptr<dyld_cache_data_source> make_dyld_cache_range_source(host_range_reader& reader, const std::filesystem::path& path);
    dyld_cache_source_opener make_host_range_cache_opener(host_range_reader& reader);

    class dyld_shared_cache_reader
    {
      public:
        static dyld_shared_cache_reader open(const std::filesystem::path& main_cache_file);
        static dyld_shared_cache_reader parse(const std::filesystem::path& main_cache_file, const dyld_cache_source_opener& opener);

        std::string_view architecture() const
        {
            return this->architecture_;
        }

        uint32_t platform() const
        {
            return this->platform_;
        }

        uint64_t shared_region_start() const
        {
            return this->shared_region_start_;
        }

        uint64_t shared_region_size() const
        {
            return this->shared_region_size_;
        }

        uint64_t max_slide() const
        {
            return this->max_slide_;
        }

        bool dylibs_expected_on_disk() const
        {
            return this->dylibs_expected_on_disk_;
        }

        uint64_t dyld_in_cache_address() const
        {
            return this->dyld_in_cache_address_;
        }

        const std::vector<dyld_cache_file_info>& files() const
        {
            return this->files_;
        }

        const std::vector<dyld_cache_image_entry>& images() const
        {
            return this->images_;
        }

        const dyld_cache_image_entry* find_image_by_address(uint64_t unslid_address) const;

        // Matched on the install name as the cache records it, which is the path the image would have on
        // disk even though it has none.
        const dyld_cache_image_entry* find_image_by_path(std::string_view path) const;

        // Reads cache bytes by unslid VM address, after parsing. The metadata pass drops its sources, so
        // anything that needs an image's own load commands -- symbolication above all -- has no way to
        // reach them without this. Sources are opened on demand and kept, because the caller reads many
        // small ranges from the same few files.
        std::vector<std::byte> read_at_address(uint64_t unslid_address, uint64_t size) const;

        void set_source_opener(dyld_cache_source_opener opener)
        {
            this->opener_ = std::move(opener);
        }

      private:
        std::string architecture_{};
        uint32_t platform_{};
        uint64_t shared_region_start_{};
        uint64_t shared_region_size_{};
        uint64_t max_slide_{};
        uint64_t dyld_in_cache_address_{};
        bool dylibs_expected_on_disk_{};
        std::vector<dyld_cache_file_info> files_{};
        std::vector<dyld_cache_image_entry> images_{};

        dyld_cache_source_opener opener_{};
        mutable std::map<std::filesystem::path, std::unique_ptr<dyld_cache_data_source>> sources_{};
    };
}
