#include "std_include.hpp"
#include "clipboard.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <iterator>
#include <limits>

namespace sogen
{
    namespace
    {
        constexpr std::array text_sources{clipboard_format::text, clipboard_format::oem_text, clipboard_format::unicode_text};
        constexpr std::array text_group{clipboard_format::locale, clipboard_format::text, clipboard_format::oem_text,
                                        clipboard_format::unicode_text};
        constexpr std::array bitmap_group{clipboard_format::bitmap, clipboard_format::dib, clipboard_format::dib_v5};
        constexpr std::array converted_by_user32{clipboard_format::text, clipboard_format::oem_text, clipboard_format::dib,
                                                 clipboard_format::unicode_text, clipboard_format::dib_v5};

        constexpr std::array text_from_unicode{clipboard_format::unicode_text, clipboard_format::oem_text};
        constexpr std::array oem_text_from_unicode{clipboard_format::unicode_text, clipboard_format::text};
        constexpr std::array unicode_text_from_legacy{clipboard_format::text, clipboard_format::oem_text};
        constexpr std::array bitmap_from_dib{clipboard_format::dib_v5, clipboard_format::dib};
        constexpr std::array dib_from_bitmap{clipboard_format::dib_v5, clipboard_format::bitmap};

        constexpr uint32_t bi_rgb = 0;
        constexpr uint32_t bi_bitfields = 3;
        constexpr uint32_t bi_rle8 = 1;
        constexpr uint32_t bi_rle4 = 2;
        constexpr uint32_t lcs_srgb = 0x73524742;
        constexpr uint32_t lcs_gm_images = 4;
        constexpr uint64_t max_size = std::numeric_limits<uint32_t>::max();
        constexpr uint64_t masks_size = 3 * sizeof(uint32_t);

        struct bitmap_v5_header
        {
            EMU_BITMAPINFOHEADER info;
            std::array<uint32_t, 4> masks;
            uint32_t color_space_type;
            std::array<int32_t, 9> endpoints;
            std::array<uint32_t, 3> gamma;
            uint32_t intent;
            uint32_t profile_data;
            uint32_t profile_size;
            uint32_t reserved;
        };

        static_assert(sizeof(bitmap_v5_header) == 124);

        bool contains(const std::span<const uint32_t> formats, const uint32_t format)
        {
            return std::ranges::find(formats, format) != formats.end();
        }

        std::span<const uint32_t> source_preference(const uint32_t target)
        {
            switch (target)
            {
            case clipboard_format::text:
                return text_from_unicode;
            case clipboard_format::oem_text:
                return oem_text_from_unicode;
            case clipboard_format::unicode_text:
                return unicode_text_from_legacy;
            case clipboard_format::bitmap:
                return bitmap_from_dib;
            case clipboard_format::dib:
                return dib_from_bitmap;
            default:
                return {};
            }
        }

        std::optional<uint64_t> dib_pixel_size(const EMU_BITMAPINFOHEADER& info)
        {
            if (info.biWidth <= 0 || info.biHeight == 0)
            {
                return std::nullopt;
            }

            const auto row_bits = static_cast<uint64_t>(info.biWidth) * info.biBitCount;
            const auto stride = (row_bits + 31) / 32 * 4;
            const auto height = static_cast<uint64_t>(info.biHeight < 0 ? -static_cast<int64_t>(info.biHeight) : info.biHeight);
            if (row_bits > max_size || stride > max_size || stride * height > max_size)
            {
                return std::nullopt;
            }

            return stride * height;
        }

        bool has_masks_block(const EMU_BITMAPINFOHEADER& info)
        {
            return info.biCompression == bi_bitfields && (info.biBitCount == 16 || info.biBitCount == 32);
        }

        uint64_t dib_color_table_size(const EMU_BITMAPINFOHEADER& info)
        {
            constexpr uint64_t entry_size = 4;

            if (has_masks_block(info))
            {
                return masks_size;
            }

            switch (info.biCompression)
            {
            case bi_rle8:
                return 256 * entry_size;
            case bi_rle4:
                return 16 * entry_size;
            case bi_rgb:
                break;
            default:
                return 0;
            }

            if (info.biBitCount <= 8)
            {
                return (info.biClrUsed != 0 ? uint64_t{info.biClrUsed} : uint64_t{1} << info.biBitCount) * entry_size;
            }

            return uint64_t{info.biClrUsed} * entry_size;
        }

        std::optional<std::vector<std::byte>> convert_dib_to_dib_v5(const std::span<const std::byte> dib)
        {
            EMU_BITMAPINFOHEADER info{};
            if (dib.size() < sizeof(info))
            {
                return std::nullopt;
            }

            std::memcpy(&info, dib.data(), sizeof(info));
            if (info.biSize != sizeof(info))
            {
                return std::nullopt;
            }

            const auto pixel_size = dib_pixel_size(info);
            const auto table_size = dib_color_table_size(info);
            if (!pixel_size || table_size + *pixel_size + sizeof(bitmap_v5_header) > max_size ||
                dib.size() < table_size + *pixel_size + sizeof(info))
            {
                return std::nullopt;
            }

            bitmap_v5_header header{};
            header.info = info;
            header.info.biSize = sizeof(header);
            header.color_space_type = lcs_srgb;
            header.intent = lcs_gm_images;

            if (has_masks_block(info))
            {
                std::memcpy(header.masks.data(), dib.data() + sizeof(info), masks_size);
            }

            const auto body_size = static_cast<size_t>(table_size + *pixel_size);
            std::vector<std::byte> converted(sizeof(header) + body_size);
            std::memcpy(converted.data(), &header, sizeof(header));
            std::memcpy(converted.data() + sizeof(header), dib.data() + sizeof(info), body_size);
            return converted;
        }
    }

    bool clipboard_state::open(const uint64_t window, const uint32_t thread_id)
    {
        if (this->opening && this->opening->window != window)
        {
            return false;
        }

        this->opening = clipboard_opening{.window = window, .thread_id = thread_id};
        return true;
    }

    bool clipboard_state::is_open_by(const uint32_t thread_id) const
    {
        return this->opening && this->opening->thread_id == thread_id;
    }

    void clipboard_state::close()
    {
        this->synthesize(text_sources, text_group);
        this->synthesize(bitmap_group, bitmap_group);
        this->opening.reset();
    }

    void clipboard_state::empty()
    {
        for (const auto& entry : this->entries)
        {
            this->release(entry);
        }

        this->entries.clear();
        this->owner = this->opening ? this->opening->window : 0;
        ++this->sequence_number;
    }

    void clipboard_state::set(const uint32_t format, const uint64_t handle, const bool memory_handle, const bool increment_sequence_number)
    {
        const clipboard_entry entry{.format = format, .handle = handle, .memory_handle = memory_handle, .synthesized = false};

        const auto existing = std::ranges::find(this->entries, format, &clipboard_entry::format);
        if (existing != this->entries.end())
        {
            this->release(*existing);
            *existing = entry;
        }
        else
        {
            this->entries.push_back(entry);
        }

        if (increment_sequence_number)
        {
            ++this->sequence_number;
        }
    }

    const clipboard_entry* clipboard_state::find(const uint32_t format) const
    {
        const auto entry = std::ranges::find(this->entries, format, &clipboard_entry::format);
        return entry == this->entries.end() ? nullptr : &*entry;
    }

    uint32_t clipboard_state::next_format(const uint32_t format) const
    {
        if (format == 0)
        {
            return this->entries.empty() ? 0 : this->entries.front().format;
        }

        const auto current = std::ranges::find(this->entries, format, &clipboard_entry::format);
        if (current == this->entries.end() || std::next(current) == this->entries.end())
        {
            return 0;
        }

        return std::next(current)->format;
    }

    std::optional<clipboard_data> clipboard_state::data(const uint32_t format)
    {
        const auto found = std::ranges::find(this->entries, format, &clipboard_entry::format);
        if (found == this->entries.end())
        {
            return std::nullopt;
        }

        auto& requested = *found;
        if (requested.synthesized && format == clipboard_format::dib_v5)
        {
            return this->read_dib_v5(requested);
        }

        const auto* entry = &requested;
        if (requested.synthesized && requested.handle == 0)
        {
            entry = this->synthesis_source(requested);
            if (!entry || !contains(converted_by_user32, entry->format))
            {
                return std::nullopt;
            }
        }

        if (entry->handle == 0)
        {
            return std::nullopt;
        }

        return clipboard_data{.format = entry->format, .handle = entry->handle, .memory_handle = entry->memory_handle};
    }

    uint64_t clipboard_state::store_memory(std::vector<std::byte> contents)
    {
        const auto handle = this->next_memory_handle_++;
        this->memory_handles_.emplace(handle, std::move(contents));
        return handle;
    }

    const std::vector<std::byte>* clipboard_state::memory(const uint64_t handle) const
    {
        const auto entry = this->memory_handles_.find(handle);
        return entry == this->memory_handles_.end() ? nullptr : &entry->second;
    }

    bool clipboard_state::add_listener(const uint64_t window)
    {
        if (std::ranges::find(this->listeners, window) != this->listeners.end())
        {
            return false;
        }

        this->listeners.push_back(window);
        return true;
    }

    bool clipboard_state::remove_listener(const uint64_t window)
    {
        return std::erase(this->listeners, window) != 0;
    }

    void clipboard_state::serialize(utils::buffer_serializer& buffer) const
    {
        buffer.write(this->sequence_number);
        buffer.write(this->owner);
        buffer.write_optional(this->opening);
        buffer.write_vector(this->entries);
        buffer.write_vector(this->listeners);
        buffer.write_map(this->memory_handles_);
        buffer.write(this->next_memory_handle_);
    }

    void clipboard_state::deserialize(utils::buffer_deserializer& buffer)
    {
        buffer.read(this->sequence_number);
        buffer.read(this->owner);
        buffer.read_optional(this->opening);
        buffer.read_vector(this->entries);
        buffer.read_vector(this->listeners);
        buffer.read_map(this->memory_handles_);
        buffer.read(this->next_memory_handle_);
    }

    void clipboard_state::release(const clipboard_entry& entry)
    {
        if (entry.memory_handle)
        {
            this->memory_handles_.erase(entry.handle);
        }
    }

    void clipboard_state::synthesize(const std::span<const uint32_t> sources, const std::span<const uint32_t> group)
    {
        const auto has_source = std::ranges::any_of(sources, [this](const uint32_t format) {
            const auto* entry = this->find(format);
            return entry && !entry->synthesized;
        });

        if (!has_source)
        {
            return;
        }

        for (const auto format : group)
        {
            if (!this->find(format))
            {
                this->entries.push_back({.format = format, .handle = 0, .memory_handle = false, .synthesized = true});
                ++this->sequence_number;
            }
        }
    }

    std::optional<clipboard_data> clipboard_state::read_dib_v5(clipboard_entry& dib_v5)
    {
        if (dib_v5.handle != 0)
        {
            return clipboard_data{.format = dib_v5.format, .handle = dib_v5.handle, .memory_handle = true};
        }

        const auto* dib = this->find(clipboard_format::dib);
        const auto* contents = dib && !dib->synthesized && dib->memory_handle ? this->memory(dib->handle) : nullptr;
        auto converted = contents ? convert_dib_to_dib_v5(*contents) : std::nullopt;
        if (!converted)
        {
            return std::nullopt;
        }

        dib_v5.handle = this->store_memory(std::move(*converted));
        dib_v5.memory_handle = true;
        return clipboard_data{.format = dib_v5.format, .handle = dib_v5.handle, .memory_handle = true};
    }

    const clipboard_entry* clipboard_state::synthesis_source(const clipboard_entry& synthesized) const
    {
        for (const auto format : source_preference(synthesized.format))
        {
            const auto* entry = this->find(format);
            if (entry && (!entry->synthesized || entry->handle != 0))
            {
                return entry;
            }
        }

        return nullptr;
    }
}
