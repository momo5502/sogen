#include "../std_include.hpp"
#include "event_log.hpp"
#include "../windows_emulator.hpp"
#include <binary_writer.hpp>

namespace sogen
{
    namespace
    {
        constexpr std::array<uint8_t, 16> event_log_interface{0xdc, 0x3f, 0x27, 0x82, 0x2a, 0xe3, 0xc3, 0x18,
                                                              0x3f, 0x78, 0x82, 0x79, 0x29, 0xdc, 0x23, 0xea};
        constexpr std::array<uint8_t, 8> context_prefix{'S', 'o', 'g', 'e', 'n', 'E', 'v', 't'};
        using event_context = std::array<uint8_t, 20>;

        struct malformed_ndr
        {
        };

        class ndr_reader
        {
          public:
            ndr_reader(std::span<const uint8_t> data, size_t pointer_size)
                : data_(data),
                  pointer_size_(pointer_size)
            {
            }

            template <typename T>
            T read()
            {
                this->align(alignof(T));
                const auto bytes = this->bytes(sizeof(T));
                T value{};
                std::memcpy(&value, bytes.data(), sizeof(value));
                return value;
            }

            uint64_t pointer()
            {
                return this->pointer_size_ == 8 ? this->read<uint64_t>() : this->read<uint32_t>();
            }

            std::span<const uint8_t> bytes(size_t size)
            {
                if (size > this->data_.size() - this->offset_)
                {
                    throw malformed_ndr{};
                }
                const auto result = this->data_.subspan(this->offset_, size);
                this->offset_ += size;
                return result;
            }

            std::u16string unicode_string()
            {
                this->align(this->pointer_size_);
                const auto length = this->read<uint16_t>();
                const auto maximum_length = this->read<uint16_t>();
                const auto referent = this->pointer();
                if (length > maximum_length || length % 2 != 0 || (referent == 0 && length != 0))
                {
                    throw malformed_ndr{};
                }
                if (referent == 0)
                {
                    return {};
                }
                const auto maximum_count = this->pointer();
                const auto offset = this->pointer();
                const auto count = this->pointer();
                if (maximum_count > 0x7fff || offset > maximum_count || count > maximum_count - offset || length > count * 2)
                {
                    throw malformed_ndr{};
                }
                const auto contents = this->bytes(static_cast<size_t>(count * 2));
                std::u16string result(length / 2, u'\0');
                if (!result.empty())
                {
                    std::memcpy(result.data(), contents.data(), length);
                }
                return result;
            }

            std::u16string wide_string()
            {
                const auto maximum = this->pointer();
                const auto offset = this->pointer();
                const auto count = this->pointer();
                if (maximum > 0x7fff || offset != 0 || count == 0 || count > maximum)
                {
                    throw malformed_ndr{};
                }
                const auto contents = this->bytes(static_cast<size_t>(count * 2));
                if (contents[contents.size() - 1] != 0 || contents[contents.size() - 2] != 0)
                {
                    throw malformed_ndr{};
                }
                std::u16string value(static_cast<size_t>(count - 1), u'\0');
                if (!value.empty())
                {
                    std::memcpy(value.data(), contents.data(), value.size() * sizeof(char16_t));
                }
                return value;
            }

            std::vector<uint8_t> sid()
            {
                if (this->pointer() == 0)
                {
                    return {};
                }
                const auto count = this->pointer();
                if (count > 15)
                {
                    throw malformed_ndr{};
                }
                const auto contents = this->bytes(static_cast<size_t>(8 + count * 4));
                if (contents[0] != 1 || contents[1] != count)
                {
                    throw malformed_ndr{};
                }
                return {contents.begin(), contents.end()};
            }

            bool optional_ulong()
            {
                const auto present = this->pointer() != 0;
                if (present)
                {
                    (void)this->read<uint32_t>();
                }
                return present;
            }

          private:
            void align(size_t alignment)
            {
                const auto padding = (alignment - this->offset_ % alignment) % alignment;
                (void)this->bytes(padding);
            }

            std::span<const uint8_t> data_;
            size_t pointer_size_;
            size_t offset_{};
        };

        struct event_record
        {
            std::u16string source{};
            std::u16string computer{};
            uint64_t generated_time{};
            uint32_t written_time{};
            uint32_t number{};
            uint32_t event_id{};
            uint16_t type{};
            uint16_t category{};
            std::vector<uint8_t> sid{};
            std::vector<std::u16string> strings{};
            std::vector<uint8_t> data{};

            void serialize(utils::buffer_serializer& buffer) const
            {
                buffer.write(this->source);
                buffer.write(this->computer);
                buffer.write(this->generated_time);
                buffer.write(this->written_time);
                buffer.write(this->number);
                buffer.write(this->event_id);
                buffer.write(this->type);
                buffer.write(this->category);
                buffer.write_vector(this->sid);
                buffer.write_vector(this->strings);
                buffer.write_vector(this->data);
            }

            void deserialize(utils::buffer_deserializer& buffer)
            {
                buffer.read(this->source);
                buffer.read(this->computer);
                buffer.read(this->generated_time);
                buffer.read(this->written_time);
                buffer.read(this->number);
                buffer.read(this->event_id);
                buffer.read(this->type);
                buffer.read(this->category);
                buffer.read_vector(this->sid);
                buffer.read_vector(this->strings);
                buffer.read_vector(this->data);
            }
        };

        event_context make_context(uint64_t id)
        {
            event_context result{};
            std::ranges::copy(context_prefix, result.begin() + 4);
            std::memcpy(result.data() + 12, &id, sizeof(id));
            return result;
        }

        uint64_t context_id(std::span<const uint8_t> context)
        {
            if (!std::ranges::equal(context.first(4), std::array<uint8_t, 4>{}) ||
                !std::ranges::equal(context.subspan(4, 8), context_prefix))
            {
                return 0;
            }
            uint64_t id{};
            std::memcpy(&id, context.data() + 12, sizeof(id));
            return id;
        }

        class event_log_port final : public rpc_port
        {
          public:
            void serialize_object(utils::buffer_serializer& buffer) const override
            {
                rpc_port::serialize_object(buffer);
                buffer.write_map(this->sources_);
                buffer.write_vector(this->records_);
                buffer.write(this->next_context_);
                buffer.write(this->stored_bytes_);
            }

            void deserialize_object(utils::buffer_deserializer& buffer) override
            {
                rpc_port::deserialize_object(buffer);
                buffer.read_map(this->sources_);
                buffer.read_vector(this->records_);
                buffer.read(this->next_context_);
                buffer.read(this->stored_bytes_);
            }

            NTSTATUS handle_rpc(windows_emulator& win_emu, uint32_t procedure, const lpc_request_context& c,
                                utils::aligned_binary_writer& writer, std::vector<alpc_reply_handle>&) override
            {
                if (this->bound_interface() != event_log_interface)
                {
                    return STATUS_NOT_SUPPORTED;
                }
                try
                {
                    std::vector<uint8_t> request(c.send_buffer_length);
                    if (!win_emu.memory.try_read_memory(c.send_buffer, request.data(), request.size()))
                    {
                        return STATUS_ACCESS_VIOLATION;
                    }
                    ndr_reader reader(request, writer.pointer_size());
                    switch (procedure)
                    {
                    case 7:
                    case 8:
                        return this->open(reader, writer);
                    case 2:
                    case 3:
                        return this->close(reader, writer);
                    case 4:
                        return this->record_count(reader, writer);
                    case 11:
                    case 25:
                        return this->report(win_emu, reader, writer, procedure == 25, request.size());
                    default:
                        win_emu.log.warn("Unsupported event log procedure: %u\n", procedure);
                        return STATUS_NOT_SUPPORTED;
                    }
                }
                catch (const malformed_ndr&)
                {
                    win_emu.log.warn("Malformed event log procedure: %u\n", procedure);
                    return STATUS_INVALID_PARAMETER;
                }
                catch (const std::bad_alloc&)
                {
                    return STATUS_NO_MEMORY;
                }
            }

          private:
            NTSTATUS open(ndr_reader& reader, utils::aligned_binary_writer& writer)
            {
                if (reader.pointer() != 0)
                {
                    if (reader.wide_string() != u".")
                    {
                        return STATUS_NOT_SUPPORTED;
                    }
                }
                auto source = reader.unicode_string();
                (void)reader.unicode_string();
                const auto major = reader.read<uint32_t>();
                const auto minor = reader.read<uint32_t>();
                if (source.empty() || major != 1 || minor != 1)
                {
                    writer.write(event_context{});
                    writer.write(STATUS_INVALID_PARAMETER);
                    return STATUS_SUCCESS;
                }
                const auto id = this->next_context_;
                writer.write(make_context(id));
                writer.write(STATUS_SUCCESS);
                this->sources_.emplace(id, std::move(source));
                ++this->next_context_;
                return STATUS_SUCCESS;
            }

            NTSTATUS close(ndr_reader& reader, utils::aligned_binary_writer& writer)
            {
                const auto id = context_id(reader.bytes(20));
                const auto removed = this->sources_.erase(id) != 0;
                writer.write(event_context{});
                writer.write(removed ? STATUS_SUCCESS : STATUS_INVALID_HANDLE);
                return STATUS_SUCCESS;
            }

            NTSTATUS record_count(ndr_reader& reader, utils::aligned_binary_writer& writer) const
            {
                const auto valid = this->sources_.contains(context_id(reader.bytes(20)));
                writer.write(static_cast<uint32_t>(valid ? this->records_.size() : 0));
                writer.write(valid ? STATUS_SUCCESS : STATUS_INVALID_HANDLE);
                return STATUS_SUCCESS;
            }

            NTSTATUS report(windows_emulator& win_emu, ndr_reader& reader, utils::aligned_binary_writer& writer, const bool extended,
                            size_t request_size)
            {
                const auto id = context_id(reader.bytes(20));
                const auto source = this->sources_.find(id);
                if (source == this->sources_.end())
                {
                    return STATUS_INVALID_HANDLE;
                }
                event_record record{};
                record.source = source->second;
                record.generated_time = reader.read<uint32_t>();
                if (extended)
                {
                    record.generated_time |= static_cast<uint64_t>(reader.read<uint32_t>()) << 32;
                }
                record.type = reader.read<uint16_t>();
                record.category = reader.read<uint16_t>();
                record.event_id = reader.read<uint32_t>();
                const auto string_count = reader.read<uint16_t>();
                const auto data_size = reader.read<uint32_t>();
                if (string_count > 256 || data_size > 61440)
                {
                    throw malformed_ndr{};
                }
                record.computer = reader.unicode_string();
                record.sid = reader.sid();
                const auto has_strings = reader.pointer() != 0;
                if (has_strings)
                {
                    if (reader.pointer() != string_count)
                    {
                        throw malformed_ndr{};
                    }
                    for (size_t index = 0; index < string_count; ++index)
                    {
                        if (reader.pointer() == 0)
                        {
                            throw malformed_ndr{};
                        }
                    }
                    for (size_t index = 0; index < string_count; ++index)
                    {
                        record.strings.push_back(reader.unicode_string());
                    }
                }
                else if (string_count != 0)
                {
                    throw malformed_ndr{};
                }
                const auto has_data = reader.pointer() != 0;
                if (has_data)
                {
                    if (reader.pointer() != data_size)
                    {
                        throw malformed_ndr{};
                    }
                    const auto data = reader.bytes(data_size);
                    record.data.assign(data.begin(), data.end());
                }
                else if (data_size != 0)
                {
                    throw malformed_ndr{};
                }
                (void)reader.read<uint16_t>();
                const auto return_number = reader.optional_ulong();
                const auto return_time = !extended && reader.optional_ulong();
                constexpr size_t maximum_log_size = 16 * 1024 * 1024;
                constexpr NTSTATUS log_file_full = 0xC0000188;
                auto status = STATUS_SUCCESS;
                record.number = static_cast<uint32_t>(this->records_.size() + 1);
                record.written_time = static_cast<uint32_t>(
                    std::chrono::duration_cast<std::chrono::seconds>(win_emu.clock().system_now().time_since_epoch()).count());
                const auto record_number = record.number;
                const auto written_time = record.written_time;
                if (request_size > maximum_log_size - this->stored_bytes_)
                {
                    status = log_file_full;
                }
                else
                {
                    this->records_.push_back(std::move(record));
                    this->stored_bytes_ += request_size;
                    const auto& stored = this->records_.back();
                    win_emu.log.info("Event log: %s event %u record %u\n", u16_to_u8(stored.source).c_str(), stored.event_id,
                                     stored.number);
                    if (std::getenv("SOGEN_TRACE_EVENT_LOG") != nullptr)
                    {
                        for (const auto& message : stored.strings)
                        {
                            win_emu.log.info("%s\n", u16_to_u8(message).c_str());
                        }
                    }
                }
                writer.write_ndr_pointer(return_number);
                if (return_number)
                {
                    writer.write(record_number);
                }
                if (!extended)
                {
                    writer.write_ndr_pointer(return_time);
                    if (return_time)
                    {
                        writer.write(written_time);
                    }
                }
                writer.write(status);
                return STATUS_SUCCESS;
            }

            std::map<uint64_t, std::u16string> sources_{};
            std::vector<event_record> records_{};
            uint64_t next_context_{1};
            size_t stored_bytes_{};
        };
    }

    std::unique_ptr<port> create_event_log_port()
    {
        return std::make_unique<event_log_port>();
    }
}
