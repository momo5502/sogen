#pragma once

#include <memory>
#include <arch_emulator.hpp>
#include <serialization.hpp>

#include "emulator_utils.hpp"
#include "handles.hpp"

class windows_emulator;
struct process_context;

struct io_device_context
{
    handle event{};
    emulator_pointer /*PIO_APC_ROUTINE*/ apc_routine{};
    emulator_pointer apc_context{};
    emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block;
    ULONG io_control_code{};
    emulator_pointer input_buffer{};
    ULONG input_buffer_length{};
    emulator_pointer output_buffer{};
    ULONG output_buffer_length{};

    io_device_context(x86_64_emulator& emu)
        : io_status_block(emu)
    {
    }

    io_device_context(utils::buffer_deserializer& buffer)
        : io_device_context(buffer.read<x64_emulator_wrapper>().get())
    {
    }

    void serialize(utils::buffer_serializer& buffer) const
    {
        buffer.write(event);
        buffer.write(apc_routine);
        buffer.write(apc_context);
        buffer.write(io_status_block);
        buffer.write(io_control_code);
        buffer.write(input_buffer);
        buffer.write(input_buffer_length);
        buffer.write(output_buffer);
        buffer.write(output_buffer_length);
    }

    void deserialize(utils::buffer_deserializer& buffer)
    {
        buffer.read(event);
        buffer.read(apc_routine);
        buffer.read(apc_context);
        buffer.read(io_status_block);
        buffer.read(io_control_code);
        buffer.read(input_buffer);
        buffer.read(input_buffer_length);
        buffer.read(output_buffer);
        buffer.read(output_buffer_length);
    }
};

struct io_device_creation_data
{
    uint64_t buffer;
    uint32_t length;
};

inline NTSTATUS write_io_status(const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block, const NTSTATUS status,
                                const bool clear_struct = false)
{
    io_status_block.access([=](IO_STATUS_BLOCK<EmulatorTraits<Emu64>>& status_block) {
        if (clear_struct)
        {
            status_block = {};
        }

        status_block.Status = status;
    });

    return status;
}

struct io_device : ref_counted_object
{
    io_device() = default;
    ~io_device() override = default;

    io_device(io_device&&) = default;
    io_device& operator=(io_device&&) = default;

    io_device(const io_device&) = delete;
    io_device& operator=(const io_device&) = delete;

    virtual NTSTATUS io_control(windows_emulator& win_emu, const io_device_context& context) = 0;

    virtual void create(windows_emulator& win_emu, const io_device_creation_data& data)
    {
        (void)win_emu;
        (void)data;
    }

    virtual void work(windows_emulator& win_emu)
    {
        (void)win_emu;
    }

    NTSTATUS execute_ioctl(windows_emulator& win_emu, const io_device_context& c)
    {
        if (c.io_status_block)
        {
            c.io_status_block.write({});
        }

        const auto result = this->io_control(win_emu, c);
        write_io_status(c.io_status_block, result);
        return result;
    }
};

struct stateless_device : io_device
{
    void create(windows_emulator&, const io_device_creation_data&) final
    {
    }

    void serialize_object(utils::buffer_serializer&) const override
    {
    }

    void deserialize_object(utils::buffer_deserializer&) override
    {
    }
};

std::unique_ptr<io_device> create_device(std::u16string_view device);

class io_device_container : public io_device
{
  public:
    io_device_container() = default;

    io_device_container(std::u16string device, windows_emulator& win_emu, const io_device_creation_data& data)
        : device_name_(std::move(device))
    {
        this->setup();
        this->device_->create(win_emu, data);
    }

    void work(windows_emulator& win_emu) override;
    NTSTATUS io_control(windows_emulator& win_emu, const io_device_context& context) override;

    void serialize_object(utils::buffer_serializer& buffer) const override;
    void deserialize_object(utils::buffer_deserializer& buffer) override;

    template <typename T = io_device>
        requires(std::is_base_of_v<io_device, T> || std::is_same_v<io_device, T>)
    T* get_internal_device() const
    {
        this->assert_validity();
        auto* value = this->device_.get();
        return dynamic_cast<T*>(value);
    }

    std::u16string_view get_device_name() const
    {
        this->assert_validity();
        return this->device_name_;
    }

    std::u16string get_device_path() const
    {
        this->assert_validity();
        return u"\\Device\\" + this->device_name_;
    }

  private:
    std::u16string device_name_{};
    std::unique_ptr<io_device> device_{};

    void setup()
    {
        this->device_ = create_device(this->device_name_);
    }

    void assert_validity() const
    {
        if (!this->device_)
        {
            throw std::runtime_error("Device not created!");
        }
    }
};
