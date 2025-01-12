#pragma once
#include <cstdint>
#include <network/address.hpp>

namespace gdb_stub
{
    /*
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

        virtual gdb_action cont() = 0;
        virtual gdb_action stepi() = 0;

        virtual bool read_reg(int regno, size_t* value) = 0;
        virtual bool write_reg(int regno, size_t value) = 0;

        virtual bool read_mem(size_t addr, size_t len, void* val) = 0;
        virtual bool write_mem(size_t addr, size_t len, void* val) = 0;

        virtual bool set_bp(breakpoint_type type, size_t addr, size_t size) = 0;
        virtual bool del_bp(breakpoint_type type, size_t addr, size_t size) = 0;

        virtual void on_interrupt() = 0;

        virtual std::string get_target_description() const = 0;
    };

    bool run_gdb_stub(gdb_stub_handler& handler, size_t register_count, const network::address& bind_address);
    */
    bool run_gdb_stub(const network::address& bind_address);
}
