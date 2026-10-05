#include "../std_include.hpp"
#include "sspi_rpc.hpp"
#include "sspi_context.hpp"
#include "sspi_tls_client.hpp"

#include "binary_writer.hpp"
#include "../windows_emulator.hpp"

namespace sogen
{

    namespace
    {
        constexpr std::array<uint8_t, 16> k_sspi_rpc_interface = {0xc8, 0xad, 0x32, 0x4f, 0x52, 0x60, 0x04, 0x4a,
                                                                  0x87, 0x01, 0x29, 0x3c, 0xcf, 0x20, 0x96, 0xf0};
        constexpr std::array<uint8_t, 16> k_sspi_context_uuid = {0x53, 0x6f, 0x67, 0x65, 0x6e, 0x53, 0x73, 0x70,
                                                                 0x69, 0x43, 0x74, 0x78, 0x00, 0x00, 0x00, 0x01};
        constexpr std::array<uint8_t, 20> k_sspi_context_handle = {0x00, 0x00, 0x00, 0x00, 0x53, 0x6f, 0x67, 0x65, 0x6e, 0x53,
                                                                   0x73, 0x70, 0x69, 0x43, 0x74, 0x78, 0x00, 0x00, 0x00, 0x01};
        constexpr uint64_t k_credential_lower = 9;
        constexpr uint64_t k_credential_upper = 0x10498;
        constexpr uint64_t k_context_lower = 0;
        constexpr uint64_t k_context_upper = 0x104a0;
        constexpr uint64_t k_expiry = 0x7fffff36d5969fff;
        constexpr uint32_t k_context_attributes = 0x0000c11c;
        constexpr uint32_t k_continue_needed = 0x00090312;
        constexpr uint32_t k_incomplete_message = 0x80090318;
        constexpr uint32_t k_illegal_message = 0x80090326;
        constexpr uint32_t k_invalid_handle = 0x80090301;

        struct sspi_rpc_port : rpc_port
        {
            NTSTATUS handle_rpc(windows_emulator& win_emu, const uint32_t procedure_id, const lpc_request_context& c,
                                utils::aligned_binary_writer& writer, std::vector<alpc_reply_handle>&) override
            {
                if (this->bound_interface() != k_sspi_rpc_interface)
                {
                    return STATUS_NOT_SUPPORTED;
                }

                switch (procedure_id)
                {
                case 0:
                    return handle_connect_rpc(win_emu, c, writer);
                case 3:
                    return handle_call_rpc(win_emu, c, writer);
                case 4:
                    return handle_acquire_credentials(win_emu, c, writer);
                case 5:
                    return handle_free_credentials(win_emu, c, writer);
                case 6:
                    return handle_process_security_context(win_emu, c, writer);
                case 7:
                    return handle_delete_security_context(win_emu, c, writer);
                default:
                    return STATUS_NOT_SUPPORTED;
                }
            }

          private:
            struct security_handle
            {
                uint64_t lower{};
                uint64_t upper{};

                bool operator==(const security_handle&) const = default;
            };

            struct decoded_buffer
            {
                uint32_t size{};
                uint32_t type{};
                uint64_t referent{};
                size_t payload_offset{};
            };

            struct context_request
            {
                security_handle credential{};
                security_handle context{};
                uint32_t requested_attributes{};
                uint32_t representation{};
                std::string target{};
                std::vector<decoded_buffer> buffers{};
                std::vector<uint8_t> bytes{};
            };

            struct credential_record
            {
                security_handle handle{k_credential_lower, k_credential_upper};
                bool live{};
            };

            struct context_record
            {
                security_handle handle{k_context_lower, k_context_upper};
                security_handle credential{};
                std::string target{};
                std::unique_ptr<sspi::tls_client> tls{};
                bool live{};
                bool finalized{};
            };

            template <typename T>
            static bool read_value(const std::span<const uint8_t> bytes, const size_t offset, T& value)
            {
                if (offset > bytes.size() || bytes.size() - offset < sizeof(value))
                {
                    return false;
                }

                std::memcpy(&value, bytes.data() + offset, sizeof(value));
                return true;
            }

            static size_t align_up(const size_t value, const size_t alignment)
            {
                return (value + alignment - 1) & ~(alignment - 1);
            }

            static bool has_port_context(const std::span<const uint8_t> request)
            {
                return request.size() >= k_sspi_context_handle.size() &&
                       std::memcmp(request.data(), k_sspi_context_handle.data(), k_sspi_context_handle.size()) == 0;
            }

            static std::optional<std::vector<uint8_t>> read_request(windows_emulator& win_emu, const lpc_request_context& c)
            {
                if (c.send_buffer == 0)
                {
                    return std::nullopt;
                }

                std::vector<uint8_t> request(c.send_buffer_length);
                if (!request.empty())
                {
                    win_emu.emu().read_memory(c.send_buffer, request.data(), request.size());
                }
                return request;
            }

            static bool matches_marshaled_handle(const security_handle value, const security_handle expected)
            {
                return value.upper == expected.upper && (value.lower == 0 || value.lower == expected.lower);
            }

            static bool finish_reply(utils::aligned_binary_writer& writer, const uint64_t start, const size_t expected_size)
            {
                const auto size = writer.offset() - start;
                if (size > expected_size)
                {
                    return false;
                }

                writer.pad(expected_size - static_cast<size_t>(size));
                return true;
            }

            static void write_callback_result(utils::aligned_binary_writer& writer)
            {
                writer.write<uint32_t>(0);
                writer.write_pointer_sized(0);
                writer.write_pointer_sized(0);
                writer.write<uint32_t>(0);
                writer.write<uint32_t>(0);
                writer.write_ndr_pointer(false);
                writer.write<int8_t>(0);
                writer.align_to(8);
                writer.write<int32_t>(0);
                writer.align_to(8);
            }

            static bool decode_context_request(windows_emulator& win_emu, const lpc_request_context& c, context_request& result)
            {
                auto request = read_request(win_emu, c);
                if (!request || request->size() < 0x60 || !has_port_context(*request))
                {
                    return false;
                }

                result.bytes = std::move(*request);
                const std::span<const uint8_t> bytes{result.bytes};
                uint16_t target_length{};
                uint16_t target_maximum_length{};
                uint64_t target_referent{};
                uint64_t target_buffer_referent{};
                uint64_t target_maximum_count{};
                uint64_t target_offset{};
                uint64_t target_count{};
                if (!read_value(bytes, 0x30, target_referent) || !read_value(bytes, 0x38, target_length) ||
                    !read_value(bytes, 0x3a, target_maximum_length) || !read_value(bytes, 0x40, target_buffer_referent) ||
                    !read_value(bytes, 0x48, target_maximum_count) || !read_value(bytes, 0x50, target_offset) ||
                    !read_value(bytes, 0x58, target_count) || target_referent == 0 || target_buffer_referent == 0 ||
                    target_length % 2 != 0 || target_maximum_length % 2 != 0 || target_length > target_maximum_length ||
                    target_offset != 0 || target_maximum_count != target_maximum_length / 2 || target_count != target_length / 2 ||
                    target_count > (bytes.size() - 0x60) / sizeof(char16_t))
                {
                    return false;
                }

                result.target.reserve(static_cast<size_t>(target_count));
                for (size_t index = 0; index < target_count; ++index)
                {
                    uint16_t character{};
                    if (!read_value(bytes, 0x60 + index * sizeof(character), character) || character > 0x7f)
                    {
                        return false;
                    }
                    result.target.push_back(static_cast<char>(character));
                }

                const size_t arguments = align_up(0x60 + static_cast<size_t>(target_count) * sizeof(char16_t), 8);
                uint64_t input_ip_referent{};
                uint64_t arg_9_referent{};
                uint32_t arg_10_version{};
                uint32_t arg_10_buffer_count{};
                uint64_t arg_10_buffers_referent{};
                if (!read_value(bytes, arguments + 0x00, result.credential.lower) ||
                    !read_value(bytes, arguments + 0x08, result.credential.upper) ||
                    !read_value(bytes, arguments + 0x10, result.context.lower) ||
                    !read_value(bytes, arguments + 0x18, result.context.upper) ||
                    !read_value(bytes, arguments + 0x20, result.requested_attributes) ||
                    !read_value(bytes, arguments + 0x24, result.representation) ||
                    !read_value(bytes, arguments + 0x28, input_ip_referent) || !read_value(bytes, arguments + 0x30, arg_9_referent) ||
                    !read_value(bytes, arguments + 0x38, arg_10_version) || !read_value(bytes, arguments + 0x3c, arg_10_buffer_count) ||
                    !read_value(bytes, arguments + 0x40, arg_10_buffers_referent) || input_ip_referent != 0 || arg_9_referent != 0 ||
                    arg_10_version != 0)
                {
                    return false;
                }

                size_t offset = arguments + 0x48;
                uint64_t arg_10_deferred_count{};
                if (arg_10_buffers_referent != 0)
                {
                    if (!read_value(bytes, offset, arg_10_deferred_count) || arg_10_deferred_count != arg_10_buffer_count)
                    {
                        return false;
                    }
                    offset += sizeof(uint64_t);
                }
                else if (arg_10_buffer_count != 0)
                {
                    return false;
                }

                if (arg_10_deferred_count > (bytes.size() - offset) / 0x10)
                {
                    return false;
                }

                result.buffers.reserve(static_cast<size_t>(arg_10_deferred_count));
                for (size_t index = 0; index < arg_10_deferred_count; ++index)
                {
                    decoded_buffer buffer{};
                    const size_t buffer_offset = offset + index * 0x10;
                    if (!read_value(bytes, buffer_offset, buffer.size) || !read_value(bytes, buffer_offset + 4, buffer.type) ||
                        !read_value(bytes, buffer_offset + 8, buffer.referent))
                    {
                        return false;
                    }
                    result.buffers.push_back(buffer);
                }
                offset += static_cast<size_t>(arg_10_deferred_count) * 0x10;

                for (auto& buffer : result.buffers)
                {
                    if (buffer.referent == 0)
                    {
                        if (buffer.size != 0)
                        {
                            return false;
                        }
                        continue;
                    }

                    uint64_t payload_count{};
                    if (!read_value(bytes, offset, payload_count) || payload_count != buffer.size)
                    {
                        return false;
                    }
                    offset += sizeof(uint64_t);
                    if (payload_count > bytes.size() - offset)
                    {
                        return false;
                    }
                    buffer.payload_offset = offset;
                    offset = align_up(offset + static_cast<size_t>(payload_count), 8);
                }

                uint32_t arg_11_member_0{};
                uint32_t arg_11_member_1{};
                uint64_t arg_11_array_referent{};
                if (!read_value(bytes, offset, arg_11_member_0) || !read_value(bytes, offset + 4, arg_11_member_1) ||
                    !read_value(bytes, offset + 8, arg_11_array_referent) || arg_11_member_0 != 0)
                {
                    return false;
                }
                offset += 0x10;
                if (arg_11_array_referent != 0)
                {
                    uint64_t conformant_count{};
                    if (!read_value(bytes, offset, conformant_count) || conformant_count != arg_11_member_1 ||
                        conformant_count > (bytes.size() - offset - sizeof(uint64_t)) / 8)
                    {
                        return false;
                    }
                    offset += sizeof(uint64_t) + static_cast<size_t>(conformant_count) * 8;
                }
                else if (arg_11_member_1 != 0)
                {
                    return false;
                }

                return offset <= bytes.size() && bytes.size() - offset == 0x30;
            }

            static std::span<const uint8_t> buffer_payload(const context_request& request, const decoded_buffer& buffer)
            {
                return {request.bytes.data() + buffer.payload_offset, buffer.size};
            }

            NTSTATUS handle_call_rpc(windows_emulator& win_emu, const lpc_request_context& c, utils::aligned_binary_writer& writer)
            {
                auto request = read_request(win_emu, c);
                uint32_t declared_length{};
                uint64_t conformant_count{};
                uint32_t api{};
                uint64_t package_id{};
                uint64_t client_context{};
                uint64_t client_process{};
                if (writer.pointer_size() != utils::aligned_binary_writer::pointer_size_64 || !request || request->size() != 0xf8 ||
                    !has_port_context(*request) || !read_value(*request, 0x14, declared_length) ||
                    !read_value(*request, 0x18, conformant_count) || !read_value(*request, 0x28, client_context) ||
                    !read_value(*request, 0x30, client_process) || !read_value(*request, 0x48, api) ||
                    !read_value(*request, 0x60, package_id) || declared_length != 0xd8 || conformant_count != 0xd8 || api != 4 ||
                    package_id >= 12 || writer.offset() != 0x18)
                {
                    return STATUS_INVALID_PARAMETER;
                }

                constexpr std::u16string_view package_name = u"Microsoft Unified Security Protocol Provider";
                constexpr std::u16string_view package_comment = u"Schannel Security Package";
                constexpr std::u16string_view module_name = u"C:\\Windows\\system32\\schannel.DLL";
                if (this->package_strings_ == 0)
                {
                    this->package_strings_ = win_emu.memory.allocate_memory(0x1000, nt_memory_permission{memory_permission::read_write});
                    if (this->package_strings_ == 0)
                    {
                        return STATUS_NO_MEMORY;
                    }
                    win_emu.emu().write_memory(this->package_strings_, package_name.data(), package_name.size() * sizeof(char16_t));
                    win_emu.emu().write_memory(this->package_strings_ + 0x5a, package_comment.data(),
                                               package_comment.size() * sizeof(char16_t));
                    win_emu.emu().write_memory(this->package_strings_ + 0x8e, module_name.data(), module_name.size() * sizeof(char16_t));
                }

                const auto start = writer.offset();
                writer.write<int32_t>(0xd8);
                writer.write_ndr_pointer(true);
                writer.write_pointer_sized(0xd8);
                writer.write<uint16_t>(0xb0);
                writer.write<uint16_t>(0xd8);
                writer.write<uint32_t>(0);
                writer.write<uint64_t>(client_context);
                writer.write<uint64_t>(client_process);
                writer.write<uint64_t>(0);
                writer.write<uint64_t>(0);
                writer.write<uint32_t>(api);
                writer.align_to(8);
                writer.write<uint64_t>(0);
                writer.write<uint64_t>(package_id);
                writer.write<uint64_t>(0);
                writer.write<uint16_t>(0x58);
                writer.write<uint16_t>(0x5a);
                writer.align_to(8);
                writer.write<uint64_t>(this->package_strings_);
                writer.write<uint16_t>(0x32);
                writer.write<uint16_t>(0x34);
                writer.align_to(8);
                writer.write<uint64_t>(this->package_strings_ + 0x5a);
                writer.write<uint16_t>(0x40);
                writer.write<uint16_t>(0x42);
                writer.align_to(8);
                writer.write<uint64_t>(this->package_strings_ + 0x8e);
                writer.write<uint32_t>(1);
                writer.write<uint32_t>(0x004107b3);
                writer.write<uint32_t>(0);
                writer.write<uint32_t>(14);
                writer.write<uint32_t>(1);
                writer.write<uint32_t>(0x6000);
                writer.write<uint32_t>(14);
                constexpr std::array<uint32_t, 16> context_thunks = {
                    6, 89, 84, 82, 91, 94, 102, 34, 103, 104, 113, 107, 117, 0x00c10076, 0, 0,
                };
                for (const auto thunk : context_thunks)
                {
                    writer.write<uint32_t>(thunk);
                }
                writer.write<uint32_t>(0);
                writer.write_pointer_sized(0);
                writer.write_pointer_sized(0);
                writer.write<uint32_t>(0);
                writer.write<uint32_t>(0);
                writer.write_ndr_pointer(false);
                writer.write<int8_t>(0);
                writer.align_to(8);
                writer.write<int32_t>(0);
                writer.align_to(8);
                if (!finish_reply(writer, start, 0x140))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                this->package_calls_ |= uint16_t{1} << package_id;
                return STATUS_SUCCESS;
            }

            NTSTATUS handle_acquire_credentials(windows_emulator& win_emu, const lpc_request_context& c,
                                                utils::aligned_binary_writer& writer)
            {
                auto request = read_request(win_emu, c);
                uint16_t package_length{};
                uint16_t package_maximum_length{};
                uint64_t package_referent{};
                uint64_t package_count{};
                uint32_t credential_use{};
                if (writer.pointer_size() != utils::aligned_binary_writer::pointer_size_64 || !request || request->size() < 0xc4 ||
                    !has_port_context(*request) || !read_value(*request, 0x40, package_length) ||
                    !read_value(*request, 0x42, package_maximum_length) || !read_value(*request, 0x48, package_referent) ||
                    !read_value(*request, 0x60, package_count) || !read_value(*request, 0xc0, credential_use) || package_length != 0x58 ||
                    package_maximum_length != 0x5a || package_referent == 0 || package_count != 44 || credential_use != 2 ||
                    writer.offset() != 0x18 || this->package_calls_ == 0)
                {
                    return STATUS_INVALID_PARAMETER;
                }

                constexpr std::u16string_view package_name = u"Microsoft Unified Security Protocol Provider";
                if (std::memcmp(request->data() + 0x68, package_name.data(), package_name.size() * sizeof(char16_t)) != 0)
                {
                    return STATUS_INVALID_PARAMETER;
                }

                this->credential_.live = true;
                const auto start = writer.offset();
                writer.write<uint64_t>(this->credential_.handle.lower);
                writer.write<uint64_t>(this->credential_.handle.upper);
                writer.write<uint64_t>(k_expiry);
                write_callback_result(writer);
                if (!finish_reply(writer, start, 0xa0))
                {
                    return STATUS_INVALID_PARAMETER;
                }
                return STATUS_SUCCESS;
            }

            static bool write_context_reply(utils::aligned_binary_writer& writer, const uint32_t context_flags,
                                            const std::span<const uint8_t> token, const uint32_t input_extra_size,
                                            const uint32_t input_extra_type, const security_handle context, const uint32_t package_status,
                                            const uint64_t expiry)
            {
                const auto start = writer.offset();
                writer.write<uint32_t>(context_flags);
                writer.align_to(8);
                writer.write<uint32_t>(0);
                writer.write<uint32_t>(1);
                writer.write_ndr_pointer(true);
                writer.write_pointer_sized(1);
                writer.write<uint32_t>(static_cast<uint32_t>(token.size()));
                writer.write<uint32_t>(2);
                writer.write_ndr_pointer(!token.empty());
                if (!token.empty())
                {
                    writer.write_pointer_sized(token.size());
                    writer.write(token.data(), token.size(), 1);
                    writer.align_to(8);
                }

                writer.write_ndr_pointer(input_extra_type != 0);
                writer.write<uint32_t>(0);
                writer.write<uint32_t>(input_extra_type != 0 ? 2 : 0);
                writer.write_ndr_pointer(input_extra_type != 0);
                if (input_extra_type != 0)
                {
                    writer.write_pointer_sized(2);
                    writer.write<uint32_t>(0);
                    writer.write<uint32_t>(0);
                    writer.write<uint32_t>(input_extra_size);
                    writer.write<uint32_t>(input_extra_type);
                    writer.write_ndr_pointer(false);
                    writer.write_ndr_pointer(false);
                }

                writer.write<uint64_t>(context.lower);
                writer.write<uint64_t>(context.upper);
                writer.write<uint32_t>(k_context_attributes);
                writer.align_to(8);
                writer.write<uint64_t>(expiry);
                writer.write<uint32_t>(package_status);
                writer.align_to(8);
                write_callback_result(writer);
                const size_t token_size = token.empty() ? 0 : align_up(sizeof(uint64_t) + token.size(), 8);
                const size_t mutation_size = input_extra_type == 0 ? 0 : 0x28;
                return finish_reply(writer, start, 0x140 + token_size + mutation_size);
            }

            static bool write_final_context_reply(utils::aligned_binary_writer& writer, const std::span<const uint8_t> token,
                                                  const std::span<const uint8_t> provider_context, const uint32_t input_extra_size,
                                                  const uint32_t input_extra_type, const security_handle context)
            {
                const auto start = writer.offset();
                writer.write<uint32_t>(0x2000);
                writer.align_to(8);
                writer.write<uint32_t>(0);
                writer.write<uint32_t>(1);
                writer.write_ndr_pointer(true);
                writer.write_pointer_sized(1);
                writer.write<uint32_t>(static_cast<uint32_t>(token.size()));
                writer.write<uint32_t>(2);
                writer.write_ndr_pointer(!token.empty());
                if (!token.empty())
                {
                    writer.write_pointer_sized(token.size());
                    writer.write(token.data(), token.size(), 1);
                    writer.align_to(8);
                }

                writer.write_ndr_pointer(input_extra_type != 0);
                writer.write<uint32_t>(static_cast<uint32_t>(provider_context.size()));
                writer.write<uint32_t>(0);
                writer.write_ndr_pointer(true);
                if (input_extra_type != 0)
                {
                    writer.write_pointer_sized(2);
                    writer.write<uint32_t>(0);
                    writer.write<uint32_t>(0);
                    writer.write<uint32_t>(input_extra_size);
                    writer.write<uint32_t>(input_extra_type);
                    writer.write_ndr_pointer(false);
                    writer.write_ndr_pointer(false);
                }
                writer.write_pointer_sized(provider_context.size());
                writer.write(provider_context.data(), provider_context.size(), 1);
                writer.align_to(8);
                writer.write<uint64_t>(context.lower);
                writer.write<uint64_t>(context.upper);
                writer.write<uint32_t>(k_context_attributes);
                writer.align_to(8);
                writer.write<uint64_t>(k_expiry);
                writer.write<uint32_t>(0);
                writer.align_to(8);
                write_callback_result(writer);
                const size_t token_size = token.empty() ? 0 : align_up(sizeof(uint64_t) + token.size(), 8);
                const size_t mutation_size = input_extra_type == 0 ? 0 : 0x28;
                return finish_reply(writer, start, 0x148 + token_size + align_up(provider_context.size(), 8) + mutation_size);
            }

            NTSTATUS handle_process_security_context(windows_emulator& win_emu, const lpc_request_context& c,
                                                     utils::aligned_binary_writer& writer)
            {
                context_request request{};
                if (writer.pointer_size() != utils::aligned_binary_writer::pointer_size_64 || writer.offset() != 0x18 ||
                    !decode_context_request(win_emu, c, request) || !this->credential_.live ||
                    !matches_marshaled_handle(request.credential, this->credential_.handle) || request.target.empty() ||
                    request.requested_attributes != k_context_attributes || request.representation != 0x10)
                {
                    return STATUS_INVALID_PARAMETER;
                }

                std::span<const uint8_t> input{};
                if (!this->context_.live)
                {
                    if (request.context != security_handle{} || !request.buffers.empty())
                    {
                        return STATUS_INVALID_PARAMETER;
                    }
                    auto tls = sspi::tls_client::create(request.target);
                    if (!tls)
                    {
                        return write_context_reply(writer, 0, {}, 0, 0, {}, k_illegal_message, 0) ? STATUS_SUCCESS
                                                                                                  : STATUS_INVALID_PARAMETER;
                    }
                    this->context_ = {
                        .handle = {k_context_lower, k_context_upper},
                        .credential = this->credential_.handle,
                        .target = request.target,
                        .tls = std::move(tls),
                        .live = true,
                    };
                }
                else
                {
                    if (this->context_.finalized || request.target != this->context_.target ||
                        !matches_marshaled_handle(request.context, this->context_.handle) || request.buffers.size() != 2 ||
                        request.buffers[0].type != 2 || request.buffers[0].referent == 0 || request.buffers[1].size != 0 ||
                        request.buffers[1].type != 0 || request.buffers[1].referent != 0)
                    {
                        return STATUS_INVALID_PARAMETER;
                    }
                    input = buffer_payload(request, request.buffers[0]);
                }

                auto result = this->context_.tls->process(input);
                if (result.output_token.size() > std::numeric_limits<uint32_t>::max() ||
                    result.missing_size > std::numeric_limits<uint32_t>::max() || result.extra_size > std::numeric_limits<uint32_t>::max())
                {
                    result.status = sspi::handshake_status::failed;
                }

                if (result.status == sspi::handshake_status::failed)
                {
                    return write_context_reply(writer, 0, result.output_token, 0, 0, this->context_.handle, k_illegal_message, 0)
                               ? STATUS_SUCCESS
                               : STATUS_INVALID_PARAMETER;
                }
                if (result.status == sspi::handshake_status::incomplete_message)
                {
                    return write_context_reply(writer, 0x10, result.output_token, static_cast<uint32_t>(result.missing_size), 4,
                                               this->context_.handle, k_incomplete_message, 0)
                               ? STATUS_SUCCESS
                               : STATUS_INVALID_PARAMETER;
                }
                if (result.status == sspi::handshake_status::continue_needed)
                {
                    return write_context_reply(writer, 0, result.output_token, 0, 0, this->context_.handle, k_continue_needed, k_expiry)
                               ? STATUS_SUCCESS
                               : STATUS_INVALID_PARAMETER;
                }

                const auto handoff = this->context_.tls->take_handoff_state();
                const auto provider_context = handoff ? sspi::build_provider_context(*handoff) : std::nullopt;
                if (!provider_context)
                {
                    return write_context_reply(writer, 0, result.output_token, 0, 0, this->context_.handle, k_illegal_message, 0)
                               ? STATUS_SUCCESS
                               : STATUS_INVALID_PARAMETER;
                }
                const uint32_t extra_type = result.extra_size == 0 ? 0 : 5;
                if (!write_final_context_reply(writer, result.output_token, *provider_context, static_cast<uint32_t>(result.extra_size),
                                               extra_type, this->context_.handle))
                {
                    return STATUS_INVALID_PARAMETER;
                }
                this->context_.tls.reset();
                this->context_.finalized = true;
                return STATUS_SUCCESS;
            }

            NTSTATUS handle_delete_security_context(windows_emulator& win_emu, const lpc_request_context& c,
                                                    utils::aligned_binary_writer& writer)
            {
                auto request = read_request(win_emu, c);
                security_handle handle{};
                if (writer.pointer_size() != utils::aligned_binary_writer::pointer_size_64 || !request || request->size() != 0x38 ||
                    !has_port_context(*request) || !read_value(*request, 0x28, handle.lower) || !read_value(*request, 0x30, handle.upper) ||
                    writer.offset() != 0x18)
                {
                    return STATUS_INVALID_PARAMETER;
                }

                const uint32_t status =
                    this->context_.live && matches_marshaled_handle(handle, this->context_.handle) ? 0 : k_invalid_handle;
                if (status == 0)
                {
                    this->context_.tls.reset();
                    this->context_.live = false;
                }
                const auto start = writer.offset();
                writer.write<uint32_t>(status);
                write_callback_result(writer);
                if (!finish_reply(writer, start, 0x38))
                {
                    return STATUS_INVALID_PARAMETER;
                }
                return STATUS_SUCCESS;
            }

            NTSTATUS handle_free_credentials(windows_emulator& win_emu, const lpc_request_context& c, utils::aligned_binary_writer& writer)
            {
                auto request = read_request(win_emu, c);
                security_handle handle{};
                if (writer.pointer_size() != utils::aligned_binary_writer::pointer_size_64 || !request || request->size() != 0x38 ||
                    !has_port_context(*request) || !read_value(*request, 0x28, handle.lower) || !read_value(*request, 0x30, handle.upper) ||
                    writer.offset() != 0x18)
                {
                    return STATUS_INVALID_PARAMETER;
                }

                const uint32_t status =
                    this->credential_.live && matches_marshaled_handle(handle, this->credential_.handle) ? 0 : k_invalid_handle;
                if (status == 0)
                {
                    this->credential_.live = false;
                }
                const auto start = writer.offset();
                writer.write<uint32_t>(status);
                write_callback_result(writer);
                if (!finish_reply(writer, start, 0x38))
                {
                    return STATUS_INVALID_PARAMETER;
                }
                return STATUS_SUCCESS;
            }

            NTSTATUS handle_connect_rpc(windows_emulator& win_emu, const lpc_request_context& c, utils::aligned_binary_writer& writer)
            {
                uint64_t client_name_referent{};
                uint32_t mode{};
                if (c.send_buffer_length != 12 || c.send_buffer == 0)
                {
                    return STATUS_INVALID_PARAMETER;
                }
                client_name_referent = win_emu.emu().read_memory<uint64_t>(c.send_buffer);
                mode = win_emu.emu().read_memory<uint32_t>(c.send_buffer + 8);
                if (client_name_referent != 0 || mode != 2)
                {
                    return STATUS_INVALID_PARAMETER;
                }

                writer.write<int32_t>(0);
                writer.write<int32_t>(1);
                writer.write<uint32_t>(0);
                writer.write(k_sspi_context_uuid.data(), k_sspi_context_uuid.size(), 1);
                writer.align_to(sizeof(uint32_t));
                writer.write<int32_t>(0);
                this->connected_ = true;
                return STATUS_SUCCESS;
            }

            bool connected_{};
            uint16_t package_calls_{};
            uint64_t package_strings_{};
            credential_record credential_{};
            context_record context_{};
        };
    }

    std::unique_ptr<port> create_sspi_rpc_port()
    {
        return std::make_unique<sspi_rpc_port>();
    }

} // namespace sogen
