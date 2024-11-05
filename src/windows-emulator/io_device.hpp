#pragma once

#include <memory>
#include <serialization.hpp>

struct io_device
{
	io_device() = default;
	virtual ~io_device() = default;

	// TODO
	virtual void read() = 0;
	virtual void write() = 0;

	virtual void serialize(utils::buffer_serializer& buffer) const = 0;
	virtual void deserialize(utils::buffer_deserializer& buffer) = 0;
};

// TODO
inline std::unique_ptr<io_device> create_device(const std::wstring_view device)
{
	(void)device;
	return {};
}

class io_device_container : public io_device
{
public:
	io_device_container() = default;

	io_device_container(std::wstring device)
		: device_name_(std::move(device))
	{
		this->setup();
	}

	void read() override
	{
		this->assert_validity();
		this->device_->read();
	}

	void write() override
	{
		this->assert_validity();
		this->device_->write();
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
