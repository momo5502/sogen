#pragma once

#include "handles.hpp"

#include <serialization_helper.hpp>
#include <utils/file_handle.hpp>
#include <platform/synchronisation.hpp>
#include <platform/win_pefile.hpp>

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

template <typename GuestType>
struct user_object : ref_counted_object
{
    using guest_type = GuestType;
    emulator_object<GuestType> guest;

    user_object(memory_interface& memory)
        : guest(memory)
    {
    }

    void serialize_object(utils::buffer_serializer& buffer) const override
    {
        buffer.write(this->guest);
    }

    void deserialize_object(utils::buffer_deserializer& buffer) override
    {
        buffer.read(this->guest);
    }
};

struct window : user_object<USER_WINDOW>
{
    uint32_t thread_id{};
    hwnd handle{};
    std::u16string name{};
    std::u16string class_name{};
    int32_t width{};
    int32_t height{};
    int32_t x{};
    int32_t y{};
    uint32_t ex_style{};
    uint32_t style{};
    std::map<std::u16string, uint64_t> props{};
    emulator_pointer wnd_proc{};

    window(memory_interface& memory)
        : user_object(memory)
    {
    }

    void serialize_object(utils::buffer_serializer& buffer) const override
    {
        user_object::serialize_object(buffer);
        buffer.write(this->thread_id);
        buffer.write(this->handle);
        buffer.write(this->name);
        buffer.write(this->class_name);
        buffer.write(this->width);
        buffer.write(this->height);
        buffer.write(this->x);
        buffer.write(this->y);
        buffer.write(this->ex_style);
        buffer.write(this->style);
        buffer.write_map(this->props);
        buffer.write(this->wnd_proc);
    }

    void deserialize_object(utils::buffer_deserializer& buffer) override
    {
        user_object::deserialize_object(buffer);
        buffer.read(this->thread_id);
        buffer.read(this->handle);
        buffer.read(this->name);
        buffer.read(this->class_name);
        buffer.read(this->width);
        buffer.read(this->height);
        buffer.read(this->x);
        buffer.read(this->y);
        buffer.read(this->ex_style);
        buffer.read(this->style);
        buffer.read_map(this->props);
        buffer.read(this->wnd_proc);
    }
};

struct desktop : ref_counted_object
{
    std::u16string name{};
    uint64_t mapped_object{};

    void serialize_object(utils::buffer_serializer& buffer) const override
    {
        buffer.write(this->name);
        buffer.write(this->mapped_object);
    }

    void deserialize_object(utils::buffer_deserializer& buffer) override
    {
        buffer.read(this->name);
        buffer.read(this->mapped_object);
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
    std::u16string open_mode{};
    std::filesystem::path host_path{};
    std::optional<file_enumeration_state> enumeration_state{};
    ACCESS_MASK access_mask{};
    uint32_t file_mode{}; // FileModeInformation

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
        buffer.write(this->name);
        buffer.write(this->open_mode);
        buffer.write(this->host_path.u16string());
        buffer.write_optional(this->enumeration_state);
        buffer.write(this->access_mask);
        buffer.write(this->file_mode);

        const auto has_handle = static_cast<bool>(this->handle);
        buffer.write(has_handle);

        if (has_handle)
        {
            buffer.write(this->handle);
        }
    }

    void deserialize_object(utils::buffer_deserializer& buffer) override
    {
        buffer.read(this->name);
        buffer.read(this->open_mode);
        this->host_path = buffer.read<std::u16string>();
        buffer.read_optional(this->enumeration_state);
        buffer.read(this->access_mask);
        buffer.read(this->file_mode);

        const auto has_handle = buffer.read<bool>();

        this->handle = {};

        if (has_handle)
        {
#if defined(OS_WINDOWS)
            FILE* native_file = _wfopen(this->host_path.c_str(), reinterpret_cast<const wchar_t*>(this->open_mode.c_str()));
#else
            FILE* native_file = fopen(u16_to_u8(this->host_path.u16string()).c_str(), u16_to_u8(this->open_mode).c_str());
#endif

            if (native_file)
            {
                this->handle = native_file;
                buffer.read(this->handle);
            }
            else
            {
                throw std::runtime_error("Failed to reobtain file handle");
            }
        }
    }
};

struct section : ref_counted_object
{
    std::u16string name{};
    std::u16string file_name{};
    uint64_t maximum_size{};
    uint32_t section_page_protection{};
    uint32_t allocation_attributes{};
    std::optional<winpe::pe_image_basic_info> cached_image_info{};

    bool is_image() const
    {
        return this->allocation_attributes & SEC_IMAGE;
    }

    void cache_image_info_from_filedata(const std::vector<std::byte>& file_data)
    {
        winpe::pe_image_basic_info info{};

        // Read the PE magic to determine if it's 32-bit or 64-bit
        bool parsed = false;
        if (file_data.size() >= sizeof(PEDosHeader_t))
        {
            const auto* dos_header = reinterpret_cast<const PEDosHeader_t*>(file_data.data());
            if (dos_header->e_magic == PEDosHeader_t::k_Magic &&
                file_data.size() >= dos_header->e_lfanew + sizeof(uint32_t) + sizeof(PEFileHeader_t) + sizeof(uint16_t))
            {
                const auto* magic_ptr =
                    reinterpret_cast<const uint16_t*>(file_data.data() + dos_header->e_lfanew + sizeof(uint32_t) + sizeof(PEFileHeader_t));
                const uint16_t magic = *magic_ptr;

                // Parse based on the actual PE type
                if (magic == PEOptionalHeader_t<std::uint32_t>::k_Magic)
                {
                    parsed = winpe::parse_pe_headers<uint32_t>(file_data, info);
                }
                else if (magic == PEOptionalHeader_t<std::uint64_t>::k_Magic)
                {
                    parsed = winpe::parse_pe_headers<uint64_t>(file_data, info);
                }
            }
        }

        if (parsed)
        {
            this->cached_image_info = info;
        }
    }

    void serialize_object(utils::buffer_serializer& buffer) const override
    {
        buffer.write(this->name);
        buffer.write(this->file_name);
        buffer.write(this->maximum_size);
        buffer.write(this->section_page_protection);
        buffer.write(this->allocation_attributes);
        buffer.write_optional<winpe::pe_image_basic_info>(this->cached_image_info);
    }

    void deserialize_object(utils::buffer_deserializer& buffer) override
    {
        buffer.read(this->name);
        buffer.read(this->file_name);
        buffer.read(this->maximum_size);
        buffer.read(this->section_page_protection);
        buffer.read(this->allocation_attributes);
        buffer.read_optional(this->cached_image_info);
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

struct io_completion_message
{
    uint64_t key_context{};
    uint64_t apc_context{};
    IO_STATUS_BLOCK<EmulatorTraits<Emu64>> io_status_block{};
    handle wait_packet_handle{};

    void serialize(utils::buffer_serializer& buffer) const
    {
        buffer.write(this->key_context);
        buffer.write(this->apc_context);
        buffer.write(this->io_status_block);
        buffer.write(this->wait_packet_handle);
    }

    void deserialize(utils::buffer_deserializer& buffer)
    {
        buffer.read(this->key_context);
        buffer.read(this->apc_context);
        buffer.read(this->io_status_block);
        buffer.read(this->wait_packet_handle);
    }
};

struct io_completion : ref_counted_object
{
    std::u16string name{};
    uint32_t number_of_concurrent_threads{};
    std::vector<io_completion_message> queue{};

    void enqueue(const io_completion_message& message)
    {
        this->queue.push_back(message);
    }

    bool dequeue(io_completion_message& out_message)
    {
        if (this->queue.empty())
        {
            return false;
        }

        out_message = this->queue.front();
        this->queue.erase(this->queue.begin());
        return true;
    }

    bool remove_by_wait_packet(const handle wait_packet_handle)
    {
        const auto entry = std::find_if(this->queue.begin(), this->queue.end(), [&](const io_completion_message& message) {
            return message.wait_packet_handle == wait_packet_handle;
        });

        if (entry == this->queue.end())
        {
            return false;
        }

        this->queue.erase(entry);
        return true;
    }

    void serialize_object(utils::buffer_serializer& buffer) const override
    {
        buffer.write(this->name);
        buffer.write(this->number_of_concurrent_threads);
        buffer.write_vector(this->queue);
    }

    void deserialize_object(utils::buffer_deserializer& buffer) override
    {
        buffer.read(this->name);
        buffer.read(this->number_of_concurrent_threads);
        buffer.read_vector(this->queue);
    }
};

struct wait_completion_packet : ref_counted_object
{
    std::u16string name{};
    handle io_completion_handle{};
    handle target_object_handle{};
    uint64_t key_context{};
    uint64_t apc_context{};
    IO_STATUS_BLOCK<EmulatorTraits<Emu64>> io_status_block{};
    uint64_t io_status_information{};
    bool associated{};
    bool queued_completion{};

    void serialize_object(utils::buffer_serializer& buffer) const override
    {
        buffer.write(this->name);
        buffer.write(this->io_completion_handle);
        buffer.write(this->target_object_handle);
        buffer.write(this->key_context);
        buffer.write(this->apc_context);
        buffer.write(this->io_status_block);
        buffer.write(this->io_status_information);
        buffer.write(this->associated);
        buffer.write(this->queued_completion);
    }

    void deserialize_object(utils::buffer_deserializer& buffer) override
    {
        buffer.read(this->name);
        buffer.read(this->io_completion_handle);
        buffer.read(this->target_object_handle);
        buffer.read(this->key_context);
        buffer.read(this->apc_context);
        buffer.read(this->io_status_block);
        buffer.read(this->io_status_information);
        buffer.read(this->associated);
        buffer.read(this->queued_completion);
    }
};

struct worker_factory : ref_counted_object
{
    std::u16string name{};
    handle io_completion_handle{};
    handle worker_process_handle{};
    uint64_t start_routine{};
    uint64_t start_parameter{};
    uint32_t max_thread_count{};
    uint64_t stack_reserve{};
    uint64_t stack_commit{};
    bool shutdown{};

    int64_t timeout{};
    int64_t retry_timeout{};
    int64_t idle_timeout{};
    uint32_t binding_count{};
    uint32_t thread_minimum{};
    uint32_t thread_maximum{};
    uint32_t paused{};
    uint32_t thread_base_priority{};
    uint32_t timeout_waiters{};
    uint32_t flags{};
    uint32_t thread_soft_maximum{};

    uint32_t last_info_class{};
    uint32_t last_info_length{};
    uint64_t last_info_value{};
    std::vector<handle> worker_threads{};

    void serialize_object(utils::buffer_serializer& buffer) const override
    {
        buffer.write(this->name);
        buffer.write(this->io_completion_handle);
        buffer.write(this->worker_process_handle);
        buffer.write(this->start_routine);
        buffer.write(this->start_parameter);
        buffer.write(this->max_thread_count);
        buffer.write(this->stack_reserve);
        buffer.write(this->stack_commit);
        buffer.write(this->shutdown);
        buffer.write(this->timeout);
        buffer.write(this->retry_timeout);
        buffer.write(this->idle_timeout);
        buffer.write(this->binding_count);
        buffer.write(this->thread_minimum);
        buffer.write(this->thread_maximum);
        buffer.write(this->paused);
        buffer.write(this->thread_base_priority);
        buffer.write(this->timeout_waiters);
        buffer.write(this->flags);
        buffer.write(this->thread_soft_maximum);
        buffer.write(this->last_info_class);
        buffer.write(this->last_info_length);
        buffer.write(this->last_info_value);
        buffer.write_vector(this->worker_threads);
    }

    void deserialize_object(utils::buffer_deserializer& buffer) override
    {
        buffer.read(this->name);
        buffer.read(this->io_completion_handle);
        buffer.read(this->worker_process_handle);
        buffer.read(this->start_routine);
        buffer.read(this->start_parameter);
        buffer.read(this->max_thread_count);
        buffer.read(this->stack_reserve);
        buffer.read(this->stack_commit);
        buffer.read(this->shutdown);
        buffer.read(this->timeout);
        buffer.read(this->retry_timeout);
        buffer.read(this->idle_timeout);
        buffer.read(this->binding_count);
        buffer.read(this->thread_minimum);
        buffer.read(this->thread_maximum);
        buffer.read(this->paused);
        buffer.read(this->thread_base_priority);
        buffer.read(this->timeout_waiters);
        buffer.read(this->flags);
        buffer.read(this->thread_soft_maximum);
        buffer.read(this->last_info_class);
        buffer.read(this->last_info_length);
        buffer.read(this->last_info_value);
        buffer.read_vector(this->worker_threads);
    }
};
