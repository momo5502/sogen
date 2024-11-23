#pragma once

#include "std_include.hpp"
#include <serialization.hpp>

class windows_emulator;

class kusd_mmio
{
public:
	kusd_mmio(windows_emulator& win_emu,  bool use_relative_time, bool perform_registration = true);
	~kusd_mmio();

	kusd_mmio(kusd_mmio&& obj);

	kusd_mmio(utils::buffer_deserializer& buffer);

	kusd_mmio(const kusd_mmio&) = delete;
	kusd_mmio& operator=(kusd_mmio&& obj) = delete;
	kusd_mmio& operator=(const kusd_mmio&) = delete;

	void serialize(utils::buffer_serializer& buffer) const;
	void deserialize(utils::buffer_deserializer& buffer);

	KUSER_SHARED_DATA& get()
	{
		return this->kusd_;
	}

	const KUSER_SHARED_DATA& get() const
	{
		return this->kusd_;
	}

	static uint64_t address();

private:
	bool registered_{};
	bool use_relative_time_{};
	windows_emulator* win_emu_{};
	KUSER_SHARED_DATA kusd_{};
	std::chrono::system_clock::time_point start_time_{};

	uint64_t read(uint64_t addr, size_t size);
	void write(uint64_t addr, size_t size, uint64_t data);

	void update();

	void register_mmio();
	void deregister_mmio();
};
