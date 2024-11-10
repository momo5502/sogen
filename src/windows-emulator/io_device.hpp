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

struct io_device_creation_data
{
	uint64_t buffer;
	uint32_t length;
};

inline void write_io_status(const emulator_object<IO_STATUS_BLOCK> io_status_block, const NTSTATUS status)
{
	if (io_status_block)
	{
		io_status_block.access([&](IO_STATUS_BLOCK& status_block)
		{
			status_block.Status = status;
		});
	}
}

struct io_device
{
	io_device() = default;
	virtual ~io_device() = default;

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

	virtual void serialize(utils::buffer_serializer& buffer) const = 0;
	virtual void deserialize(utils::buffer_deserializer& buffer) = 0;

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

	io_device_container(std::wstring device, windows_emulator& win_emu, const io_device_creation_data& data)
		: device_name_(std::move(device))
	{
		this->setup();
		this->device_->create(win_emu, data);
	}

	NTSTATUS io_control(windows_emulator& win_emu, const io_device_context& context) override
	{
		this->assert_validity();
		return this->device_->io_control(win_emu, context);
	}

	void work(windows_emulator& win_emu) override
	{
		this->assert_validity();
		return this->device_->work(win_emu);
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

	template <typename T = io_device>
		requires(std::is_base_of_v<io_device, T> || std::is_same_v<io_device, T>)
	T* get_internal_device()
	{
		this->assert_validity();
		auto* value = this->device_.get();
		return dynamic_cast<T*>(value);
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
