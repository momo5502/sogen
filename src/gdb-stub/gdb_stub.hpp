#pragma once
#include <cstdint>
#include <network/address.hpp>

namespace gdb_stub
{
    enum class gdb_action : uint8_t
    {
        none,
        resume,
        shutdown,
    };

    enum class breakpoint_type : uint8_t
    {
        software,
        hardware_exec,
        hardware_write,
        hardware_read,
        hardware_read_write,
    };

    struct gdb_stub_handler
    {
        virtual ~gdb_stub_handler() = default;

        virtual gdb_action run() = 0;
        virtual gdb_action singlestep() = 0;

        virtual size_t get_register_count() = 0;
        virtual size_t get_max_register_size() = 0;

        virtual bool read_register(size_t reg, void* data, size_t max_length) = 0;
        virtual bool write_register(size_t reg, const void* data, size_t size) = 0;

        virtual bool read_memory(size_t address, void* data, size_t length) = 0;
        virtual bool write_memory(size_t address, const void* data, size_t length) = 0;

        virtual bool set_breakpoint(breakpoint_type type, size_t address, size_t size) = 0;
        virtual bool delete_breakpoint(breakpoint_type type, size_t address, size_t size) = 0;

        virtual void on_interrupt() = 0;

        virtual std::string get_target_description() = 0;
    };

    bool run_gdb_stub(const network::address& bind_address, gdb_stub_handler& handler);
}
