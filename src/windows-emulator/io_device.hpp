#pragma once

#include <memory>
#include <x64_emulator.hpp>
#include <serialization.hpp>

#include "emulator_utils.hpp"
#include "handles.hpp"

class windows_emulator;
struct process_context;

struct io_device_context
{
	windows_emulator& win_emu;
	x64_emulator& emu;
	process_context& proc;

	handle event;
	emulator_pointer /*PIO_APC_ROUTINE*/ apc_routine;
	emulator_pointer apc_context;
	emulator_object<IO_STATUS_BLOCK> io_status_block;
	ULONG io_control_code;
	emulator_pointer input_buffer;
	ULONG input_buffer_length;
	emulator_pointer output_buffer;
	ULONG output_buffer_length;
};

struct io_device
{
	io_device() = default;
	virtual ~io_device() = default;

	io_device(io_device&&) = default;
	io_device& operator=(io_device&&) = default;

	io_device(const io_device&) = delete;
	io_device& operator=(const io_device&) = delete;

	virtual NTSTATUS io_control(const io_device_context& context) = 0;

	virtual void serialize(utils::buffer_serializer& buffer) const = 0;
	virtual void deserialize(utils::buffer_deserializer& buffer) = 0;
};

struct stateless_device : io_device
{
	void serialize(utils::buffer_serializer&) const override
	{
	}

	void deserialize(utils::buffer_deserializer&) override
	{
	}
};

std::unique_ptr<io_device> create_device(const std::wstring_view device);

class io_device_container : public io_device
{
public:
	io_device_container() = default;

	io_device_container(std::wstring device)
		: device_name_(std::move(device))
	{
		this->setup();
	}

	NTSTATUS io_control(const io_device_context& context) override
	{
		this->assert_validity();
		return this->device_->io_control(context);
	}

	void serialize(utils::buffer_serializer& buffer) const override
	{
		this->assert_validity();

		buffer.write_string(this->device_name_);
		this->device_->serialize(buffer);
	}

	void deserialize(utils::buffer_deserializer& buffer) override
	{
		buffer.read_string(this->device_name_);
		this->setup();
		this->device_->deserialize(buffer);
	}

private:
	std::wstring device_name_{};
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
