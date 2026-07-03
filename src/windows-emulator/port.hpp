#pragma once

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>
#include <arch_emulator.hpp>
#include <serialization.hpp>

#include "emulator_utils.hpp"
#include "handles.hpp"

namespace sogen
{

    class windows_emulator;
    struct process_context;
    namespace utils
    {
        class aligned_binary_writer;
    }

    struct lpc_port_message
    {
        PORT_MESSAGE64 native{};
        bool is_wow64{};

        [[nodiscard]] ULONG data_length() const
        {
            return static_cast<ULONG>(native.u1.s1.DataLength);
        }

        [[nodiscard]] ULONG total_length() const
        {
            return static_cast<ULONG>(native.u1.s1.TotalLength);
        }

        [[nodiscard]] ULONG header_size() const
        {
            return total_length() - data_length();
        }

        [[nodiscard]] ULONG wire_size() const
        {
            return wire_size(this->is_wow64);
        }

        [[nodiscard]] static ULONG wire_size(const bool is_32_bit)
        {
            return is_32_bit ? static_cast<ULONG>(sizeof(PORT_MESSAGE32)) : static_cast<ULONG>(sizeof(PORT_MESSAGE64));
        }

        void write(const emulator_object<PORT_MESSAGE64>& message) const
        {
            if (this->is_wow64)
            {
                const auto wow64_message = narrow(native);
                message.get_memory_interface()->write_memory(message.value(), &wow64_message, sizeof(wow64_message));
                return;
            }

            message.write(native);
        }

        static lpc_port_message read(const emulator_object<PORT_MESSAGE64>& message)
        {
            lpc_port_message result{};

            const auto prefix = emulator_object<port_message_prefix>{*message.get_memory_interface(), message.value()}.read();
            const auto data_length = static_cast<ULONG>(prefix.DataLength);
            const auto total_length = static_cast<ULONG>(prefix.TotalLength);
            const auto header_size = total_length >= data_length ? total_length - data_length : 0;

            if (header_size == wire_size(true))
            {
                const emulator_object<PORT_MESSAGE32> wow64_message{*message.get_memory_interface(), message.value()};
                result.native = widen(wow64_message.read());
                result.is_wow64 = true;
            }
            else if (header_size == wire_size(false))
            {
                result.native = message.read();
            }
            else
            {
                throw std::runtime_error("Unexpected LPC header size");
            }

            return result;
        }

      private:
        struct port_message_prefix
        {
            CSHORT DataLength;
            CSHORT TotalLength;
            CSHORT Type;
            CSHORT DataInfoOffset;
        };

        template <typename T, typename U>
        static T narrow(const U value, const char* field_name)
        {
            if constexpr (sizeof(T) < sizeof(U))
            {
                if (value > static_cast<U>(std::numeric_limits<T>::max()))
                {
                    throw std::runtime_error(std::string(field_name) + " does not fit in WOW64 LPC header");
                }
            }

            return static_cast<T>(value);
        }

        static PORT_MESSAGE32 narrow(const PORT_MESSAGE64& message)
        {
            PORT_MESSAGE32 result{};

            result.u1.s1.DataLength = message.u1.s1.DataLength;
            result.u1.s1.TotalLength = message.u1.s1.TotalLength;
            result.u2.s2.Type = message.u2.s2.Type;
            result.u2.s2.DataInfoOffset = message.u2.s2.DataInfoOffset;
            result.ClientId.UniqueProcess =
                narrow<EmulatorTraits<Emu32>::HANDLE>(message.ClientId.UniqueProcess, "LPC ClientId.UniqueProcess");
            result.ClientId.UniqueThread =
                narrow<EmulatorTraits<Emu32>::HANDLE>(message.ClientId.UniqueThread, "LPC ClientId.UniqueThread");
            result.MessageId = message.MessageId;
            result.ClientViewSize = narrow<EmulatorTraits<Emu32>::SIZE_T>(message.ClientViewSize, "LPC ClientViewSize");

            return result;
        }

        static PORT_MESSAGE64 widen(const PORT_MESSAGE32& message)
        {
            PORT_MESSAGE64 result{};

            result.u1.s1.DataLength = message.u1.s1.DataLength;
            result.u1.s1.TotalLength = message.u1.s1.TotalLength;
            result.u2.s2.Type = message.u2.s2.Type;
            result.u2.s2.DataInfoOffset = message.u2.s2.DataInfoOffset;
            result.ClientId.UniqueProcess = message.ClientId.UniqueProcess;
            result.ClientId.UniqueThread = message.ClientId.UniqueThread;
            result.MessageId = message.MessageId;
            result.ClientViewSize = message.ClientViewSize;

            return result;
        }
    };

    struct lpc_message_context
    {
        emulator_object<PORT_MESSAGE64> send_message;
        emulator_object<PORT_MESSAGE64> receive_message;
        EmulatorTraits<Emu64>::SIZE_T receive_buffer_length{};

        lpc_message_context(memory_interface& emu)
            : send_message(emu),
              receive_message(emu)
        {
        }

        lpc_message_context(utils::buffer_deserializer& buffer)
            : lpc_message_context(buffer.read<x64_emulator_wrapper>().get())
        {
        }

        void serialize(utils::buffer_serializer& buffer) const
        {
            buffer.write(send_message);
            buffer.write(receive_message);
            buffer.write(receive_buffer_length);
        }

        void deserialize(utils::buffer_deserializer& buffer)
        {
            buffer.read(send_message);
            buffer.read(receive_message);
            buffer.read(receive_buffer_length);
        }
    };

    struct lpc_request_context
    {
        emulator_pointer send_buffer{};
        ULONG send_buffer_length{};
        emulator_pointer recv_buffer{};
        ULONG recv_buffer_length{};

        void serialize(utils::buffer_serializer& buffer) const
        {
            buffer.write(send_buffer);
            buffer.write(send_buffer_length);
            buffer.write(recv_buffer);
            buffer.write(recv_buffer_length);
        }

        void deserialize(utils::buffer_deserializer& buffer)
        {
            buffer.read(send_buffer);
            buffer.read(send_buffer_length);
            buffer.read(recv_buffer);
            buffer.read(recv_buffer_length);
        }
    };

    struct lpc_request_result
    {
        struct reply_in_place_t
        {
        };

        static constexpr reply_in_place_t reply_in_place{};

        NTSTATUS status{};
        std::optional<std::vector<uint8_t>> payload{};

        lpc_request_result() = default;

        lpc_request_result(NTSTATUS s)
            : status{s},
              payload{std::vector<uint8_t>{}}
        {
        }

        lpc_request_result(NTSTATUS s, std::vector<uint8_t>&& p)
            : status{s},
              payload{std::move(p)}
        {
        }

        lpc_request_result(NTSTATUS s, reply_in_place_t)
            : status{s}
        {
        }
    };

    struct lpc_message_result
    {
        NTSTATUS status{};
        lpc_port_message message{};
        std::vector<uint8_t> payload{};

        [[nodiscard]] ULONG total_length() const
        {
            if (message.native.u1.s1.TotalLength != 0)
            {
                return message.total_length();
            }

            return static_cast<ULONG>(message.wire_size() + payload.size());
        }

        void serialize(utils::buffer_serializer& buffer) const
        {
            buffer.write(status);
            buffer.write(message);
            buffer.write_vector(payload);
        }

        void deserialize(utils::buffer_deserializer& buffer)
        {
            buffer.read(status);
            buffer.read(message);
            buffer.read_vector(payload);
        }
    };

    struct port_creation_data
    {
        uint64_t view_base;
        int64_t view_size;
    };

    struct port : ref_counted_object
    {
        uint64_t view_base{};
        int64_t view_size{};

        port() = default;
        ~port() override = default;

        port(port&&) = default;
        port& operator=(port&&) = default;

        port(const port&) = delete;
        port& operator=(const port&) = delete;

        void serialize_object(utils::buffer_serializer& buffer) const override
        {
            buffer.write(this->view_base);
            buffer.write(this->view_size);
        }

        void deserialize_object(utils::buffer_deserializer& buffer) override
        {
            buffer.read(this->view_base);
            buffer.read(this->view_size);
        }

        virtual void create(windows_emulator& win_emu, const port_creation_data& data)
        {
            (void)win_emu;
            view_base = data.view_base;
            view_size = data.view_size;
        }

        virtual lpc_message_result handle_message(windows_emulator& win_emu, const lpc_message_context& c);

        virtual lpc_request_result handle_request(windows_emulator& win_emu, const lpc_request_context& c) = 0;
    };

    struct rpc_port : port
    {
        lpc_request_result handle_request(windows_emulator& win_emu, const lpc_request_context& c) override;

        virtual NTSTATUS handle_rpc(windows_emulator& win_emu, uint32_t procedure_id, const lpc_request_context& c,
                                    utils::aligned_binary_writer& writer) = 0;

      private:
        static lpc_request_result handle_handshake(windows_emulator& win_emu, const lpc_request_context& c);
        lpc_request_result handle_rpc_call(windows_emulator& win_emu, const lpc_request_context& c);
    };

    std::unique_ptr<port> create_port(std::u16string_view port);

    class port_container : public port
    {
      public:
        port_container() = default;

        port_container(std::u16string port, windows_emulator& win_emu, const port_creation_data& data)
            : port_name_(std::move(port))
        {
            this->setup();
            this->port_->create(win_emu, data);
        }

        lpc_request_result handle_request(windows_emulator& win_emu, const lpc_request_context& c) override
        {
            this->assert_validity();
            return this->port_->handle_request(win_emu, c);
        }

        lpc_message_result handle_message(windows_emulator& win_emu, const lpc_message_context& c) override;

        void serialize_object(utils::buffer_serializer& buffer) const override
        {
            this->assert_validity();

            buffer.write_string(this->port_name_);
            buffer.write_vector(this->reply_queue_);
            this->port_->serialize(buffer);
        }

        void deserialize_object(utils::buffer_deserializer& buffer) override
        {
            buffer.read_string(this->port_name_);
            buffer.read_vector(this->reply_queue_);
            this->setup();
            this->port_->deserialize(buffer);
        }

        template <typename T = port>
            requires(std::is_base_of_v<port, T> || std::is_same_v<port, T>)
        T* get_internal_port() const
        {
            this->assert_validity();
            auto* value = this->port_.get();
            return dynamic_cast<T*>(value);
        }

        std::u16string_view get_port_name() const
        {
            this->assert_validity();
            return this->port_name_;
        }

        bool has_pending_reply() const
        {
            return !reply_queue_.empty();
        }

      private:
        std::u16string port_name_{};
        std::unique_ptr<port> port_{};
        std::vector<lpc_message_result> reply_queue_;

        void setup()
        {
            this->port_ = create_port(this->port_name_);
        }

        void assert_validity() const
        {
            if (!this->port_)
            {
                throw std::runtime_error("Port not created!");
            }
        }
    };

} // namespace sogen
