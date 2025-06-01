#pragma once

#include "handles.hpp"

#include <serialization_helper.hpp>
#include <utils/file_handle.hpp>
#include <platform/synchronisation.hpp>

struct timer : ref_counted_object
{
    std::u16string name{};

    void serialize_object(utils::buffer_serializer& buffer) const override
    {
        buffer.write(this->name);
    }

    void deserialize_object(utils::buffer_deserializer& buffer) override
    {
        buffer.read(this->name);
    }
};

struct event : ref_counted_object
{
    bool signaled{};
    EVENT_TYPE type{};
    std::u16string name{};

    bool is_signaled()
    {
        const auto res = this->signaled;

        if (this->type == SynchronizationEvent)
        {
            this->signaled = false;
        }

        return res;
    }

    void serialize_object(utils::buffer_serializer& buffer) const override
    {
        buffer.write(this->signaled);
        buffer.write(this->type);
        buffer.write(this->name);
    }

    void deserialize_object(utils::buffer_deserializer& buffer) override
    {
        buffer.read(this->signaled);
        buffer.read(this->type);
        buffer.read(this->name);
    }
};

struct window : ref_counted_object
{
    uint32_t thread_id{};
    std::u16string name{};
    std::u16string class_name{};
    int32_t width;
    int32_t height;
    int32_t x;
    int32_t y;
    std::unordered_map<std::u16string, uint64_t> props;

    void serialize_object(utils::buffer_serializer& buffer) const override
    {
        buffer.write(this->thread_id);
        buffer.write(this->name);
        buffer.write(this->class_name);
        buffer.write(this->width);
        buffer.write(this->height);
        buffer.write(this->x);
        buffer.write(this->y);
        buffer.write_map(this->props);
    }

    void deserialize_object(utils::buffer_deserializer& buffer) override
    {
        buffer.read(this->thread_id);
        buffer.read(this->name);
        buffer.read(this->class_name);
        buffer.read(this->width);
        buffer.read(this->height);
        buffer.read(this->x);
        buffer.read(this->y);
        buffer.read_map(this->props);
    }
};

struct mutant : ref_counted_object
{
    uint32_t locked_count{0};
    uint32_t owning_thread_id{};
    std::u16string name{};

    bool try_lock(const uint32_t thread_id)
    {
        if (this->locked_count == 0)
        {
            ++this->locked_count;
            this->owning_thread_id = thread_id;
            return true;
        }

        if (this->owning_thread_id != thread_id)
        {
            return false;
        }

        ++this->locked_count;
        return true;
    }

    std::pair<uint32_t, bool> release(const uint32_t thread_id)
    {
        const auto old_count = this->locked_count;

        if (this->locked_count <= 0 || this->owning_thread_id != thread_id)
        {
            return {old_count, false};
        }

        --this->locked_count;
        return {old_count, true};
    }

    void serialize_object(utils::buffer_serializer& buffer) const override
    {
        buffer.write(this->locked_count);
        buffer.write(this->owning_thread_id);
        buffer.write(this->name);
    }

    void deserialize_object(utils::buffer_deserializer& buffer) override
    {
        buffer.read(this->locked_count);
        buffer.read(this->owning_thread_id);
        buffer.read(this->name);
    }
};

struct file_entry
{
    std::filesystem::path file_path{};
    uint64_t file_size{};
    bool is_directory{};

    void serialize(utils::buffer_serializer& buffer) const
    {
        buffer.write(this->file_path);
        buffer.write(this->file_size);
        buffer.write(this->is_directory);
    }

    void deserialize(utils::buffer_deserializer& buffer)
    {
        buffer.read(this->file_path);
        buffer.read(this->file_size);
        buffer.read(this->is_directory);
    }
};

struct file_enumeration_state
{
    size_t current_index{0};
    std::vector<file_entry> files{};

    void serialize(utils::buffer_serializer& buffer) const
    {
        buffer.write(this->current_index);
        buffer.write_vector(this->files);
    }

    void deserialize(utils::buffer_deserializer& buffer)
    {
        buffer.read(this->current_index);
        buffer.read_vector(this->files);
    }
};

struct file : ref_counted_object
{
    utils::file_handle handle{};
    std::u16string name{};
    std::optional<file_enumeration_state> enumeration_state{};

    bool is_file() const
    {
        return this->handle;
    }

    bool is_directory() const
    {
        return !this->is_file();
    }

    void serialize_object(utils::buffer_serializer& buffer) const override
    {
        // TODO: Serialize handle
        buffer.write(this->name);
        buffer.write_optional(this->enumeration_state);
    }

    void deserialize_object(utils::buffer_deserializer& buffer) override
    {
        buffer.read(this->name);
        buffer.read_optional(this->enumeration_state);
        this->handle = {};
    }
};

struct section : ref_counted_object
{
    std::u16string name{};
    std::u16string file_name{};
    uint64_t maximum_size{};
    uint32_t section_page_protection{};
    uint32_t allocation_attributes{};

    bool is_image() const
    {
        return this->allocation_attributes & SEC_IMAGE;
    }

    void serialize_object(utils::buffer_serializer& buffer) const override
    {
        buffer.write(this->name);
        buffer.write(this->file_name);
        buffer.write(this->maximum_size);
        buffer.write(this->section_page_protection);
        buffer.write(this->allocation_attributes);
    }

    void deserialize_object(utils::buffer_deserializer& buffer) override
    {
        buffer.read(this->name);
        buffer.read(this->file_name);
        buffer.read(this->maximum_size);
        buffer.read(this->section_page_protection);
        buffer.read(this->allocation_attributes);
    }
};

struct semaphore : ref_counted_object
{
    std::u16string name{};
    uint32_t current_count{};
    uint32_t max_count{};

    bool try_lock()
    {
        if (this->current_count > 0)
        {
            --this->current_count;
            return true;
        }

        return false;
    }

    std::pair<uint32_t, bool> release(const uint32_t release_count)
    {
        const auto old_count = this->current_count;

        if (this->current_count + release_count > this->max_count)
        {
            return {old_count, false};
        }

        this->current_count += release_count;

        return {old_count, true};
    }

    void serialize_object(utils::buffer_serializer& buffer) const override
    {
        buffer.write(this->name);
        buffer.write(this->current_count);
        buffer.write(this->max_count);
    }

    void deserialize_object(utils::buffer_deserializer& buffer) override
    {
        buffer.read(this->name);
        buffer.read(this->current_count);
        buffer.read(this->max_count);
    }
};

struct port : ref_counted_object
{
    std::u16string name{};
    uint64_t view_base{};

    void serialize_object(utils::buffer_serializer& buffer) const override
    {
        buffer.write(this->name);
        buffer.write(this->view_base);
    }

    void deserialize_object(utils::buffer_deserializer& buffer) override
    {
        buffer.read(this->name);
        buffer.read(this->view_base);
    }
};
