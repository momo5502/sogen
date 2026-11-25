#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>
#include <serialization.hpp>

namespace sogen
{
    namespace clipboard_format
    {
        inline constexpr uint32_t text = 1;
        inline constexpr uint32_t bitmap = 2;
        inline constexpr uint32_t oem_text = 7;
        inline constexpr uint32_t dib = 8;
        inline constexpr uint32_t unicode_text = 13;
        inline constexpr uint32_t locale = 16;
        inline constexpr uint32_t dib_v5 = 17;
        inline constexpr uint32_t first_registered = 0xC000;
    }

    struct clipboard_entry
    {
        uint32_t format{};
        uint64_t handle{};
        bool memory_handle{};
        bool synthesized{};

        void serialize(utils::buffer_serializer& buffer) const
        {
            buffer.write(this->format);
            buffer.write(this->handle);
            buffer.write(this->memory_handle);
            buffer.write(this->synthesized);
        }

        void deserialize(utils::buffer_deserializer& buffer)
        {
            buffer.read(this->format);
            buffer.read(this->handle);
            buffer.read(this->memory_handle);
            buffer.read(this->synthesized);
        }
    };

    struct clipboard_opening
    {
        uint64_t window{};
        uint32_t thread_id{};

        void serialize(utils::buffer_serializer& buffer) const
        {
            buffer.write(this->window);
            buffer.write(this->thread_id);
        }

        void deserialize(utils::buffer_deserializer& buffer)
        {
            buffer.read(this->window);
            buffer.read(this->thread_id);
        }
    };

    struct SETCLIPBDATA
    {
        int32_t global_handle;
        int32_t increment_sequence_number;
    };

    struct GETCLIPBDATA
    {
        uint32_t format;
        int32_t global_handle;
        uint64_t locale;
    };

    struct clipboard_data
    {
        uint32_t format{};
        uint64_t handle{};
        bool memory_handle{};
    };

    struct clipboard_state
    {
        uint32_t sequence_number{};
        uint64_t owner{};
        std::optional<clipboard_opening> opening{};
        std::vector<clipboard_entry> entries{};
        std::vector<uint64_t> listeners{};

        bool open(uint64_t window, uint32_t thread_id);
        bool is_open_by(uint32_t thread_id) const;
        void close();
        void empty();
        void set(uint32_t format, uint64_t handle, bool memory_handle, bool increment_sequence_number);

        const clipboard_entry* find(uint32_t format) const;
        uint32_t next_format(uint32_t format) const;
        std::optional<clipboard_data> data(uint32_t format);

        uint64_t store_memory(std::vector<std::byte> contents);
        const std::vector<std::byte>* memory(uint64_t handle) const;

        bool add_listener(uint64_t window);
        bool remove_listener(uint64_t window);

        void serialize(utils::buffer_serializer& buffer) const;
        void deserialize(utils::buffer_deserializer& buffer);

      private:
        std::map<uint64_t, std::vector<std::byte>> memory_handles_{};
        uint64_t next_memory_handle_{1};

        void release(const clipboard_entry& entry);
        std::optional<clipboard_data> read_dib_v5(clipboard_entry& dib_v5);
        void synthesize(std::span<const uint32_t> sources, std::span<const uint32_t> group);
        const clipboard_entry* synthesis_source(const clipboard_entry& synthesized) const;
    };
}
