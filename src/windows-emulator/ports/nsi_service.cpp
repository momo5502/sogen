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

        // An NDR context handle: its attributes, then the 16-byte id the server handed out.
        constexpr size_t context_handle_size = 20;
        constexpr std::array<uint8_t, 8> registration_prefix{'S', 'o', 'g', 'e', 'n', 'N', 'S', 'I'};

        std::array<uint8_t, context_handle_size> make_context_handle(const uint64_t registration)
        {
            std::array<uint8_t, context_handle_size> handle{};
            std::memcpy(handle.data() + 4, registration_prefix.data(), registration_prefix.size());
            std::memcpy(handle.data() + 4 + registration_prefix.size(), &registration, sizeof(registration));
            return handle;
        }

        std::optional<uint64_t> read_registration(const std::span<const uint8_t> request, const size_t offset)
        {
            if (offset > request.size() || request.size() - offset < context_handle_size)
            {
                return std::nullopt;
            }

            const auto handle = request.subspan(offset, context_handle_size);
            uint32_t attributes{};
            std::memcpy(&attributes, handle.data(), sizeof(attributes));
            if (attributes != 0 || std::memcmp(handle.data() + 4, registration_prefix.data(), registration_prefix.size()) != 0)
            {
                return std::nullopt;
            }

            uint64_t registration{};
            std::memcpy(&registration, handle.data() + 4 + registration_prefix.size(), sizeof(registration));
            return registration;
        }

        // A change request held until its registration closes, with the encoding its reply must use.
        struct pending_change_request
        {
            uint64_t token{};
            uint64_t pointer_size{};
        };

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

                std::vector<uint8_t> request(c.send_buffer_length);
                if (!win_emu.memory.try_read_memory(c.send_buffer, request.data(), request.size()))
                {
                    return STATUS_ACCESS_VIOLATION;
                }

                switch (procedure)
                {
                case register_change_notification:
                    return this->register_notification(request, writer);
                case deregister_change_notification:
                    return this->deregister_notification(request, writer);
                case request_change_notification:
                    return this->request_notification(request, writer.pointer_size());
                default:
                    return STATUS_NOT_SUPPORTED;
                }
            }

            void serialize_object(utils::buffer_serializer& buffer) const override
            {
                rpc_port::serialize_object(buffer);
                buffer.write(this->next_registration_);
                buffer.write_vector(std::vector<uint64_t>(this->registrations_.begin(), this->registrations_.end()));
                buffer.write_map(this->pending_requests_);
            }

            void deserialize_object(utils::buffer_deserializer& buffer) override
            {
                rpc_port::deserialize_object(buffer);
                buffer.read(this->next_registration_);
                std::vector<uint64_t> registrations{};
                buffer.read_vector(registrations);
                this->registrations_ = {registrations.begin(), registrations.end()};
                buffer.read_map(this->pending_requests_);
            }

          private:
            // The request carries what to watch; a call without it is malformed.
            NTSTATUS register_notification(const std::span<const uint8_t> request, utils::aligned_binary_writer& writer)
            {
                if (request.empty())
                {
                    return STATUS_INVALID_PARAMETER;
                }

                const auto registration = this->next_registration_++;
                this->registrations_.insert(registration);
                writer.write(make_context_handle(registration));
                writer.write<uint32_t>(0);
                return STATUS_SUCCESS;
            }

            // The registration descriptor comes first and the [in, out] context handle last.
            NTSTATUS deregister_notification(const std::span<const uint8_t> request, utils::aligned_binary_writer& writer)
            {
                if (request.size() <= context_handle_size)
                {
                    return STATUS_INVALID_PARAMETER;
                }

                const auto registration = read_registration(request, request.size() - context_handle_size);
                if (!registration || !this->registrations_.erase(*registration))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                if (const auto pending = this->pending_requests_.find(*registration); pending != this->pending_requests_.end())
                {
                    this->complete_cancelled_request(pending->second);
                    this->pending_requests_.erase(pending);
                }

                writer.write(std::array<uint8_t, context_handle_size>{});
                writer.write<uint32_t>(0);
                return STATUS_SUCCESS;
            }

            // The context handle is the first parameter. The emulated network never changes, so a change request stays
            // pending until its registration closes.
            NTSTATUS request_notification(const std::span<const uint8_t> request, const size_t pointer_size)
            {
                const auto registration = read_registration(request, 0);
                if (!registration || !this->registrations_.contains(*registration) || this->pending_requests_.contains(*registration))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                const auto token = this->defer_current_call();
                if (!token)
                {
                    return STATUS_NOT_SUPPORTED;
                }
                this->pending_requests_.emplace(*registration, pending_change_request{.token = *token, .pointer_size = pointer_size});
                return STATUS_SUCCESS;
            }

            void complete_cancelled_request(const pending_change_request& pending)
            {
                std::vector<uint8_t> reply{};
                utils::aligned_binary_writer writer(reply, static_cast<size_t>(pending.pointer_size));
                const bool ndr64 = pending.pointer_size == utils::aligned_binary_writer::pointer_size_64;
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
                this->complete_deferred_call(pending.token, reply);
            }

            uint64_t next_registration_{1};
            std::set<uint64_t> registrations_{};
            // Registration id to its pending change request.
            std::map<uint64_t, pending_change_request> pending_requests_{};
        };
    }

    std::unique_ptr<port> create_nsi_service_port()
    {
        return std::make_unique<nsi_service_port>();
    }
}
