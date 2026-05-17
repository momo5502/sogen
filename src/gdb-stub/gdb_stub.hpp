#pragma once
#include <cstdint>
#include <network/address.hpp>
#include <stdexcept>

#include "filesystem_interface.hpp"

namespace gdb_stub
{
    enum class action : uint8_t
    {
        none,
        resume,
        output,
        shutdown,
    };

    enum class breakpoint_type : uint8_t
    {
        software = 0,
        hardware_exec = 1,
        hardware_write = 2,
        hardware_read = 3,
        hardware_read_write = 4,
        END,
    };

    struct library_info
    {
        std::string name;
        uint64_t segment_address;
    };

    struct thread_info
    {
        uint32_t id;
        std::string name;
    };

    class binding_error : public std::runtime_error
    {
      public:
        binding_error(const std::string& address)
            : std::runtime_error(address)
        {
        }
    };

    struct debugging_handler
    {
        virtual ~debugging_handler() = default;

        virtual action run() = 0;
        virtual action singlestep() = 0;

        virtual size_t get_register_count() = 0;
        virtual size_t get_max_register_size() = 0;

        virtual size_t read_register(size_t reg, void* data, size_t max_length) = 0;
        virtual size_t write_register(size_t reg, const void* data, size_t size) = 0;

        virtual bool read_memory(uint64_t address, void* data, size_t length) = 0;
        virtual bool write_memory(uint64_t address, const void* data, size_t length) = 0;

        virtual bool set_breakpoint(breakpoint_type type, uint64_t address, size_t size) = 0;
        virtual bool delete_breakpoint(breakpoint_type type, uint64_t address, size_t size) = 0;

        virtual void on_interrupt() = 0;

        virtual std::string get_target_description(std::string_view file) = 0;

        virtual bool switch_to_thread(uint32_t thread_id) = 0;

        virtual uint32_t get_current_thread_id() = 0;
        virtual std::vector<uint32_t> get_thread_ids() = 0;

        virtual std::optional<uint32_t> get_exit_code() = 0;

        virtual bool should_stop()
        {
            return false;
        }

        virtual std::vector<library_info> get_libraries()
        {
            return {};
        }

        virtual void reset_library_stop()
        {
        }

        virtual bool should_signal_library()
        {
            return false;
        }

        virtual std::string get_executable_path()
        {
            return {};
        }

        virtual std::vector<thread_info> get_thread_list() const
        {
            return {};
        }

        virtual uint64_t get_thread_teb_addr(uint32_t id) const
        {
            (void)id;
            return 0;
        }

        virtual std::string consume_debug_output()
        {
            return {};
        }

        virtual std::string get_os_abi()
        {
            return {};
        }

        virtual std::string translate_path(std::string_view emulated_path) const
        {
            (void)emulated_path;
            return {};
        }

        virtual filesystem_interface* get_filesystem()
        {
            return nullptr;
        }
    };

    bool run_gdb_stub(const network::address& bind_address, debugging_handler& handler);
}
