#include "../std_include.hpp"
#include "nsi_service.hpp"

#include "../windows_emulator.hpp"
#include <binary_writer.hpp>

namespace sogen
{
    namespace
    {
        // winnsi's NsiRpcRegisterChangeNotificationEx registers (5), then keeps one change request pending (7) until a
        // change happens or the registration is closed (6).
        constexpr uint32_t register_change_notification = 5;
        constexpr uint32_t deregister_change_notification = 6;
        constexpr uint32_t request_change_notification = 7;
        constexpr uint32_t error_cancelled = 1223;
        constexpr std::array<uint8_t, 8> registration_prefix{'S', 'o', 'g', 'e', 'n', 'N', 'S', 'I'};
        constexpr size_t context_handle_size = 20;

        std::array<uint8_t, context_handle_size> make_context_handle(const uint64_t registration)
        {
            std::array<uint8_t, context_handle_size> handle{};
            std::memcpy(handle.data() + 4, registration_prefix.data(), registration_prefix.size());
            std::memcpy(handle.data() + 4 + registration_prefix.size(), &registration, sizeof(registration));
            return handle;
        }

        // The registration's context handle sits at a parameter-dependent offset; its marker identifies it.
        std::optional<uint64_t> find_registration(const std::span<const uint8_t> request)
        {
            for (size_t offset = 0; offset + context_handle_size <= request.size(); offset += 4)
            {
                if (std::memcmp(request.data() + offset + 4, registration_prefix.data(), registration_prefix.size()) == 0)
                {
                    uint64_t registration{};
                    std::memcpy(&registration, request.data() + offset + 4 + registration_prefix.size(), sizeof(registration));
                    return registration;
                }
            }
            return std::nullopt;
        }

        class nsi_service_port final : public rpc_port
        {
          public:
            NTSTATUS handle_rpc(windows_emulator& win_emu, const uint32_t procedure, const lpc_request_context& c,
                                utils::aligned_binary_writer& writer, std::vector<alpc_reply_handle>&) override
            {
                if (this->bound_interface() != nsi_service_interface)
                {
                    return STATUS_NOT_SUPPORTED;
                }
                this->pointer_size_ = writer.pointer_size();

                std::vector<uint8_t> request(c.send_buffer_length);
                if (!win_emu.memory.try_read_memory(c.send_buffer, request.data(), request.size()))
                {
                    return STATUS_ACCESS_VIOLATION;
                }

                switch (procedure)
                {
                case register_change_notification:
                    return this->register_notification(writer);
                case deregister_change_notification:
                    return this->deregister_notification(request, writer);
                case request_change_notification:
                    return this->request_notification(request);
                default:
                    return STATUS_NOT_SUPPORTED;
                }
            }

            void serialize_object(utils::buffer_serializer& buffer) const override
            {
                rpc_port::serialize_object(buffer);
                buffer.write(this->next_registration_);
                buffer.write_map(this->registrations_);
            }

            void deserialize_object(utils::buffer_deserializer& buffer) override
            {
                rpc_port::deserialize_object(buffer);
                buffer.read(this->next_registration_);
                buffer.read_map(this->registrations_);
            }

          private:
            NTSTATUS register_notification(utils::aligned_binary_writer& writer)
            {
                const auto registration = this->next_registration_++;
                this->registrations_.emplace(registration, uint64_t{0});
                writer.write(make_context_handle(registration));
                writer.write<uint32_t>(0);
                return STATUS_SUCCESS;
            }

            NTSTATUS deregister_notification(const std::span<const uint8_t> request, utils::aligned_binary_writer& writer)
            {
                const auto registration = find_registration(request);
                const auto entry = registration ? this->registrations_.find(*registration) : this->registrations_.end();
                if (entry == this->registrations_.end())
                {
                    return STATUS_INVALID_PARAMETER;
                }

                if (entry->second != 0)
                {
                    this->complete_cancelled_request(entry->second);
                }
                this->registrations_.erase(entry);

                writer.write(std::array<uint8_t, context_handle_size>{});
                writer.write<uint32_t>(0);
                return STATUS_SUCCESS;
            }

            // The emulated network never changes, so a change request stays pending until its registration closes.
            NTSTATUS request_notification(const std::span<const uint8_t> request)
            {
                const auto registration = find_registration(request);
                const auto entry = registration ? this->registrations_.find(*registration) : this->registrations_.end();
                if (entry == this->registrations_.end() || entry->second != 0)
                {
                    return STATUS_INVALID_PARAMETER;
                }

                const auto token = this->defer_current_call();
                if (!token)
                {
                    return STATUS_NOT_SUPPORTED;
                }
                entry->second = *token;
                return STATUS_SUCCESS;
            }

            void complete_cancelled_request(const uint64_t token)
            {
                std::vector<uint8_t> reply{};
                utils::aligned_binary_writer writer(reply, this->pointer_size_);
                const bool ndr64 = this->pointer_size_ == utils::aligned_binary_writer::pointer_size_64;
                // NDR64 widens enum16 to four bytes; classic NDR sends two.
                const auto write_enum = [&](const uint32_t value) {
                    if (ndr64)
                    {
                        writer.write<uint32_t>(value);
                    }
                    else
                    {
                        writer.write<uint16_t>(static_cast<uint16_t>(value));
                    }
                };
                writer.write_ndr_pointer(false); // key buffer
                writer.write<uint32_t>(0);
                writer.align_to(ndr64 ? 8 : 4);
                write_enum(0); // parameter type
                writer.align_to(ndr64 ? 8 : 4);
                writer.write_ndr_pointer(false); // parameter buffer
                writer.write<uint32_t>(0);
                writer.write<uint32_t>(0);
                write_enum(0); // notification type
                writer.align_to(4);
                writer.write<uint32_t>(error_cancelled);
                this->complete_deferred_call(token, reply);
            }

            size_t pointer_size_{utils::aligned_binary_writer::pointer_size_64};
            uint64_t next_registration_{1};
            // Registration id to the token of its pending change request (0 while none is pending).
            std::map<uint64_t, uint64_t> registrations_{};
        };
    }

    std::unique_ptr<port> create_nsi_service_port()
    {
        return std::make_unique<nsi_service_port>();
    }
}
