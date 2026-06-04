#pragma once

#include <cstdint>
#include <memory>
#include <optional>
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

    struct lpc_message_context
    {
        emulator_object<PORT_MESSAGE64> send_message;
        emulator_object<PORT_MESSAGE64> receive_message;
        EmulatorTraits<Emu64>::SIZE_T receive_buffer_length{};

        lpc_message_context(x86_64_emulator& emu)
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
        PORT_MESSAGE64 message{};
        std::vector<uint8_t> payload{};

        [[nodiscard]] ULONG total_length() const
        {
            if (message.u1.s1.TotalLength != 0)
            {
                return static_cast<ULONG>(message.u1.s1.TotalLength);
            }

            return static_cast<ULONG>(sizeof(message) + payload.size());
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
