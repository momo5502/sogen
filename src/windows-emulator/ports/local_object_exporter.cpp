#include "../std_include.hpp"
#include "local_object_exporter.hpp"
#include "../windows_emulator.hpp"
#include <binary_writer.hpp>

namespace sogen
{
    namespace
    {
        constexpr std::array<uint8_t, 16> exporter_interface{0xe6, 0x73, 0x0c, 0xe6, 0xf9, 0x88, 0xcf, 0x11,
                                                             0x9a, 0xf1, 0x00, 0x20, 0xaf, 0x6e, 0x72, 0xf4};
        constexpr uint32_t maximum_reserved_ids = 0x20000;
        constexpr uint32_t maximum_object_ids = 0x100000;

        class connect_reader
        {
          public:
            connect_reader(std::span<const uint8_t> data, size_t width)
                : data_(data),
                  width_(width)
            {
            }

            template <typename T>
            T read()
            {
                this->offset_ = (this->offset_ + alignof(T) - 1) & ~(alignof(T) - 1);
                if (this->offset_ > this->data_.size() || sizeof(T) > this->data_.size() - this->offset_)
                {
                    throw std::out_of_range("Truncated local object exporter request");
                }
                T result{};
                std::memcpy(&result, this->data_.data() + this->offset_, sizeof(result));
                this->offset_ += sizeof(result);
                return result;
            }

            uint64_t pointer()
            {
                return this->width_ == 8 ? this->read<uint64_t>() : this->read<uint32_t>();
            }

            std::span<const uint8_t> remaining() const
            {
                return this->data_.subspan(this->offset_);
            }

            std::u16string string()
            {
                if (this->pointer() == 0)
                {
                    return {};
                }
                const auto maximum = this->pointer();
                const auto offset = this->pointer();
                const auto count = this->pointer();
                if (offset != 0 || count == 0 || count > maximum || maximum > 32768)
                {
                    throw std::out_of_range("Invalid local object exporter string");
                }
                std::u16string value(static_cast<size_t>(count), u'\0');
                for (auto& character : value)
                {
                    character = this->read<char16_t>();
                }
                if (value.back() != u'\0')
                {
                    throw std::out_of_range("Unterminated local object exporter string");
                }
                value.pop_back();
                return value;
            }

          private:
            std::span<const uint8_t> data_;
            size_t width_;
            size_t offset_{};
        };

        std::vector<uint8_t> make_com_security_descriptor(std::span<const uint8_t> user_sid)
        {
            constexpr std::array<uint8_t, 12> system_sid{1, 1, 0, 0, 0, 0, 0, 5, 18, 0, 0, 0};
            constexpr auto descriptor_size = sizeof(SECURITY_DESCRIPTOR_RELATIVE);
            const auto acl_offset = descriptor_size + user_sid.size() * 2;
            const auto acl_size = sizeof(ACL) + 16 + user_sid.size() + system_sid.size();
            std::vector<uint8_t> data;
            utils::aligned_binary_writer writer(data);
            SECURITY_DESCRIPTOR_RELATIVE descriptor{};
            descriptor.Revision = SECURITY_DESCRIPTOR_REVISION;
            descriptor.Control = SE_SELF_RELATIVE | SE_DACL_PRESENT;
            descriptor.Owner = descriptor_size;
            descriptor.Group = static_cast<ULONG>(descriptor_size + user_sid.size());
            descriptor.Dacl = static_cast<ULONG>(acl_offset);
            writer.write(descriptor);
            writer.write(user_sid.data(), user_sid.size());
            writer.write(user_sid.data(), user_sid.size());
            ACL acl{};
            acl.AclRevision = ACL_REVISION;
            acl.AclSize = static_cast<USHORT>(acl_size);
            acl.AceCount = 2;
            writer.write(acl);
            for (const auto sid : {user_sid, std::span<const uint8_t>{system_sid}})
            {
                ACE_HEADER ace{};
                ace.AceType = ACCESS_ALLOWED_ACE_TYPE;
                ace.AceSize = static_cast<USHORT>(8 + sid.size());
                writer.write(ace);
                writer.write<ULONG>(0x0b);
                writer.write(sid.data(), sid.size());
            }
            return data;
        }

        class local_object_exporter_port final : public rpc_port
        {
          public:
            NTSTATUS handle_rpc(windows_emulator& win_emu, uint32_t procedure, const lpc_request_context& context,
                                utils::aligned_binary_writer& writer, std::vector<alpc_reply_handle>&) override
            {
                if (this->bound_interface() != exporter_interface)
                {
                    return STATUS_NOT_SUPPORTED;
                }
                try
                {
                    std::vector<uint8_t> request(context.send_buffer_length);
                    if (!win_emu.memory.try_read_memory(context.send_buffer, request.data(), request.size()))
                    {
                        return STATUS_ACCESS_VIOLATION;
                    }
                    connect_reader reader(request, writer.pointer_size());
                    if (procedure == 6)
                    {
                        return this->allocate_apartment(win_emu, reader, writer);
                    }
                    if (procedure == 8)
                    {
                        return this->free_apartment(reader, writer);
                    }
                    if (procedure == 10)
                    {
                        const auto connection = this->read_connection(reader);
                        (void)reader.read<uint32_t>();
                        writer.write(std::array<uint8_t, 20>{});
                        writer.write<uint32_t>(0);
                        this->clients_.at(static_cast<size_t>(connection - 1)).connected = false;
                        std::erase_if(this->apartments_, [&](const auto& entry) { return entry.second.connection == connection; });
                        return STATUS_SUCCESS;
                    }
                    if (procedure != 0)
                    {
                        return STATUS_NOT_SUPPORTED;
                    }
                    const auto desktop = reader.string();
                    const auto executable = reader.string();
                    if (reader.pointer() != 0)
                    {
                        return STATUS_NOT_SUPPORTED;
                    }
                    const auto process_flags = reader.read<uint32_t>();
                    const auto architecture = reader.read<uint16_t>();
                    const auto reserve_count = reader.read<uint32_t>();
                    if (reserve_count > maximum_reserved_ids)
                    {
                        return STATUS_INVALID_PARAMETER;
                    }
                    return this->connect(win_emu, writer, desktop, executable, process_flags, architecture, reserve_count);
                }
                catch (const std::out_of_range&)
                {
                    win_emu.log.warn("COM exporter malformed request\n");
                    return STATUS_INVALID_PARAMETER;
                }
                catch (const std::bad_alloc&)
                {
                    return STATUS_NO_MEMORY;
                }
            }

            void serialize_object(utils::buffer_serializer& buffer) const override
            {
                rpc_port::serialize_object(buffer);
                buffer.write(this->next_id_);
                buffer.write_vector(this->clients_);
                buffer.write_map(this->apartments_);
            }

            void deserialize_object(utils::buffer_deserializer& buffer) override
            {
                rpc_port::deserialize_object(buffer);
                buffer.read(this->next_id_);
                buffer.read_vector(this->clients_);
                buffer.read_map(this->apartments_);
            }

          private:
            struct apartment_registration
            {
                uint64_t connection{};
                uint32_t thread_id{};
                std::vector<uint64_t> object_ids{};
                std::vector<uint8_t> metadata{};

                void serialize(utils::buffer_serializer& buffer) const
                {
                    buffer.write(this->connection);
                    buffer.write(this->thread_id);
                    buffer.write_vector(this->object_ids);
                    buffer.write_vector(this->metadata);
                }

                void deserialize(utils::buffer_deserializer& buffer)
                {
                    buffer.read(this->connection);
                    buffer.read(this->thread_id);
                    buffer.read_vector(this->object_ids);
                    buffer.read_vector(this->metadata);
                }
            };

            uint64_t read_connection(connect_reader& reader) const
            {
                const auto context = reader.read<std::array<uint8_t, 20>>();
                constexpr std::array<uint8_t, 8> prefix{'S', 'o', 'g', 'e', 'n', 'C', 'O', 'M'};
                uint64_t id{};
                std::memcpy(&id, context.data() + 12, sizeof(id));
                if (std::ranges::any_of(std::span{context}.first(4), [](uint8_t byte) { return byte != 0; }) ||
                    !std::ranges::equal(prefix, std::span{context}.subspan(4, 8)) || id == 0 || id > this->clients_.size() ||
                    !this->clients_.at(static_cast<size_t>(id - 1)).connected)
                {
                    throw std::out_of_range("Invalid local object exporter context");
                }
                return id;
            }

            static void write_empty_bindings(utils::aligned_binary_writer& writer)
            {
                writer.write_ndr_pointer(true);
                writer.write_pointer_sized(4);
                writer.write<uint16_t>(4);
                writer.write<uint16_t>(2);
                for (size_t index = 0; index < 4; ++index)
                {
                    writer.write<uint16_t>(0);
                }
            }

            NTSTATUS allocate_apartment(windows_emulator& win_emu, connect_reader& reader, utils::aligned_binary_writer& writer)
            {
                apartment_registration registration{};
                registration.connection = this->read_connection(reader);
                const auto count = reader.read<uint32_t>();
                registration.thread_id = reader.read<uint32_t>();
                const auto process_id = reader.read<uint32_t>();
                if (count > maximum_object_ids || process_id != win_emu.process.process_id ||
                    this->next_id_ > std::numeric_limits<uint64_t>::max() - count - 1)
                {
                    return STATUS_INVALID_PARAMETER;
                }
                (void)reader.read<uint32_t>();
                (void)reader.read<uint16_t>();
                (void)reader.read<uint16_t>();
                (void)reader.read<uint32_t>();
                (void)reader.read<uint64_t>();
                if (reader.pointer() != 0)
                {
                    return STATUS_NOT_SUPPORTED;
                }
                (void)reader.read<std::array<uint8_t, 16>>();
                (void)reader.read<uint32_t>();
                if (reader.pointer() != 0)
                {
                    return STATUS_NOT_SUPPORTED;
                }
                (void)reader.read<std::array<uint8_t, 16>>();
                (void)reader.read<uint64_t>();
                (void)reader.read<uint32_t>();
                const auto metadata = reader.remaining();
                registration.metadata.assign(metadata.begin(), metadata.end());
                std::array<uint8_t, 16> oxid{};
                // Build 26100 uses GUID local OXIDs, unlike the 64-bit OXIDs in the public DCOM protocol.
                constexpr std::array<uint8_t, 8> prefix{'S', 'o', 'g', 'e', 'n', 'O', 'X', 'D'};
                std::memcpy(oxid.data(), prefix.data(), prefix.size());
                const auto first_id = this->next_id_;
                std::memcpy(oxid.data() + 8, &first_id, sizeof(first_id));
                registration.object_ids.reserve(count);
                writer.write(oxid);
                writer.write_pointer_sized(count);
                writer.write_pointer_sized(0);
                writer.write_pointer_sized(count);
                for (uint32_t index = 0; index < count; ++index)
                {
                    const auto id = first_id + 1 + index;
                    writer.write(id);
                    registration.object_ids.push_back(id);
                }
                writer.write(count);
                writer.write<uint64_t>(0);
                write_empty_bindings(writer);
                writer.write<uint32_t>(0);
                this->apartments_.emplace(oxid, std::move(registration));
                this->next_id_ += 1 + count;
                return STATUS_SUCCESS;
            }

            NTSTATUS free_apartment(connect_reader& reader, utils::aligned_binary_writer& writer)
            {
                const auto connection = this->read_connection(reader);
                const auto oxid = reader.read<std::array<uint8_t, 16>>();
                const auto count = reader.read<uint32_t>();
                if (count > maximum_object_ids || reader.pointer() != count)
                {
                    return STATUS_INVALID_PARAMETER;
                }
                for (uint32_t index = 0; index < count; ++index)
                {
                    (void)reader.read<uint64_t>();
                }
                const auto apartment = this->apartments_.find(oxid);
                if (apartment == this->apartments_.end() || apartment->second.connection != connection)
                {
                    return STATUS_INVALID_HANDLE;
                }
                writer.write<uint32_t>(0);
                this->apartments_.erase(apartment);
                return STATUS_SUCCESS;
            }

            struct client_connection
            {
                std::u16string desktop{};
                std::u16string executable{};
                uint32_t flags{};
                uint16_t architecture{};
                bool connected{true};

                void serialize(utils::buffer_serializer& buffer) const
                {
                    buffer.write_string(this->desktop);
                    buffer.write_string(this->executable);
                    buffer.write(this->flags);
                    buffer.write(this->architecture);
                    buffer.write(this->connected);
                }

                void deserialize(utils::buffer_deserializer& buffer)
                {
                    buffer.read_string(this->desktop);
                    buffer.read_string(this->executable);
                    buffer.read(this->flags);
                    buffer.read(this->architecture);
                    buffer.read(this->connected);
                }
            };

            NTSTATUS connect(windows_emulator& win_emu, utils::aligned_binary_writer& writer, const std::u16string& desktop,
                             const std::u16string& executable, uint32_t process_flags, uint16_t architecture, uint32_t reserve_count)
            {
                const auto process_id = static_cast<uint32_t>(win_emu.process.process_id);
                const auto context_id = static_cast<uint64_t>(this->clients_.size() + 1);
                if (this->next_id_ > std::numeric_limits<uint64_t>::max() - reserve_count)
                {
                    return STATUS_INVALID_PARAMETER;
                }
                std::array<uint8_t, 20> context{};
                constexpr std::array<uint8_t, 8> prefix{'S', 'o', 'g', 'e', 'n', 'C', 'O', 'M'};
                std::memcpy(context.data() + 4, prefix.data(), prefix.size());
                std::memcpy(context.data() + 12, &context_id, sizeof(context_id));
                writer.write(context);
                writer.write<uint32_t>(360);
                writer.write_ndr_pointer(true);
                writer.write_pointer_sized(4);
                writer.write<uint16_t>(4);
                writer.write<uint16_t>(2);
                writer.write<uint16_t>(0);
                writer.write<uint16_t>(0);
                writer.write<uint16_t>(0);
                writer.write<uint16_t>(0);
                writer.write<uint64_t>(process_id);
                writer.write_pointer_sized(reserve_count);
                for (uint32_t index = 0; index < reserve_count; ++index)
                {
                    writer.write<uint64_t>(this->next_id_ + index);
                }
                writer.write(reserve_count);
                writer.write<uint32_t>(1);
                writer.write<uint32_t>(0);
                writer.write<uint32_t>(0);
                writer.write_ndr_pointer(false);
                writer.write<uint32_t>(2);
                writer.write<uint32_t>(2);
                writer.write<uint32_t>(1);
                writer.write_ndr_pointer(true);
                writer.write_pointer_sized(1);
                writer.write<uint16_t>(10);
                writer.write<uint32_t>(1);
                writer.write_ndr_pointer(true);
                writer.write_pointer_sized(1);
                writer.write_ndr_pointer(true);
                writer.write<uint16_t>(10);
                writer.write_ndr_u16string(u"NTLM");
                writer.write<int32_t>(0);
                writer.write_ndr_pointer(false);
                writer.write(process_id);
                writer.write(process_id);
                writer.write<uint64_t>(static_cast<uint64_t>(process_id) << 32);
                std::array<uint8_t, 16> service_id{};
                std::memcpy(service_id.data(), prefix.data(), prefix.size());
                std::memcpy(service_id.data() + 8, &process_id, sizeof(process_id));
                writer.write(service_id);
                const auto descriptor = make_com_security_descriptor(win_emu.process.sid);
                for (size_t index = 0; index < 5; ++index)
                {
                    writer.write(static_cast<uint32_t>(descriptor.size()));
                }
                const auto blob_size = descriptor.size() * 5;
                writer.write(static_cast<uint32_t>(blob_size));
                writer.write_ndr_pointer(true);
                writer.write_pointer_sized(blob_size);
                for (size_t index = 0; index < 5; ++index)
                {
                    writer.write(descriptor.data(), descriptor.size());
                }
                writer.write<uint32_t>(0);
                this->clients_.push_back(
                    {.desktop = desktop, .executable = executable, .flags = process_flags, .architecture = architecture});
                this->next_id_ += reserve_count;
                return STATUS_SUCCESS;
            }

            uint64_t next_id_{1};
            std::vector<client_connection> clients_{};
            std::map<std::array<uint8_t, 16>, apartment_registration> apartments_{};
        };
    }

    std::unique_ptr<port> create_local_object_exporter_port()
    {
        return std::make_unique<local_object_exporter_port>();
    }
}
