#pragma once

#include "std_include.hpp"
#include <serialization.hpp>

#include "x64_emulator.hpp"

struct process_context;
class windows_emulator;

class kusd_mmio
{
  public:
    kusd_mmio(x64_emulator& emu, process_context& process);
    ~kusd_mmio();

    kusd_mmio(utils::buffer_deserializer& buffer);

    kusd_mmio(kusd_mmio&&) = delete;
    kusd_mmio(const kusd_mmio&) = delete;
    kusd_mmio& operator=(kusd_mmio&& obj) = delete;
    kusd_mmio& operator=(const kusd_mmio&) = delete;

    void serialize(utils::buffer_serializer& buffer) const;
    void deserialize(utils::buffer_deserializer& buffer);

    KUSER_SHARED_DATA64& get()
    {
        return this->kusd_;
    }

    const KUSER_SHARED_DATA64& get() const
    {
        return this->kusd_;
    }

    static uint64_t address();

    void setup(bool use_relative_time);

  private:
    x64_emulator* emu_{};
    process_context* process_{};

    bool registered_{};
    bool use_relative_time_{};

    KUSER_SHARED_DATA64 kusd_{};
    std::chrono::system_clock::time_point start_time_{};

    uint64_t read(uint64_t addr, size_t size);

    void update();

    void register_mmio();
    void deregister_mmio();
};
