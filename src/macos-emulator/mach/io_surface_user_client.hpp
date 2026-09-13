#pragma once

#include "../macos_syscall_utils.hpp"
#include "mach_types.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace sogen
{
    class macos_emulator;
}

namespace sogen::mach
{
    // One value out of a binary OSSerialize stream. IOSurface's properties are scalars in every measured
    // dictionary; a collection is kept as its type alone so the parse can walk past it, and a guest that
    // stores one gets it reported rather than silently returned as something else.
    struct os_value
    {
        uint32_t type{};
        uint32_t length{};
        uint64_t number{};
        std::string text{};
        std::vector<uint8_t> data{};
    };

    struct os_dictionary_entry
    {
        std::string key{};
        os_value value{};
    };

    std::optional<std::vector<os_dictionary_entry>> parse_binary_dictionary(std::span<const uint8_t> bytes);
    std::vector<uint8_t> serialize_binary_dictionary(std::span<const os_dictionary_entry> entries);

    // What the kernel keeps for one surface. Everything here is either read straight back out of the
    // record selector 0 answers with, or reached through mach trap 100.
    struct io_surface
    {
        uint32_t id{};
        uint64_t width{};
        uint64_t height{};
        uint64_t bytes_per_row{};
        uint64_t alloc_size{};
        uint32_t pixel_format{};
        uint16_t bytes_per_element{1};
        uint8_t element_width{1};
        uint8_t element_height{1};
        uint32_t cache_mode{};
        uint64_t purgeable_state{};
        uint64_t base{};
        uint64_t base_reserved{};
        uint64_t info{};
        uint64_t info_reserved{};
        uint32_t seed{};
        uint32_t lock_depth{};
        int64_t use_count{};
        uint32_t references{1};
        std::vector<os_dictionary_entry> values{};
    };

    class io_surface_store
    {
      public:
        io_surface* create(macos_emulator& emu, std::span<const uint8_t> properties);
        io_surface* find(uint32_t id);
        bool release(macos_emulator& emu, uint32_t id);

        uint64_t open_connection()
        {
            return this->next_connection_++;
        }

        size_t live_count() const
        {
            return this->surfaces_.size();
        }

        size_t created_count() const
        {
            return this->created_;
        }

      private:
        std::map<uint32_t, io_surface> surfaces_{};
        uint32_t next_id_{1};
        uint64_t next_connection_{1};
        size_t created_{};
    };

    // The decoded io_connect_method (MIG 2865) argument block, in the order MIG lays it out.
    struct io_connect_method_call
    {
        uint32_t selector{};
        std::vector<uint64_t> scalar_input{};
        std::span<const uint8_t> inband_input{};
        uint64_t ool_input{};
        uint64_t ool_input_size{};
        uint32_t inband_output_max{};
        uint32_t scalar_output_max{};
        uint64_t ool_output{};
        uint64_t ool_output_max{};
    };

    struct io_connect_method_result
    {
        kern_return_t code{};
        std::vector<uint8_t> inband_output{};
        std::vector<uint64_t> scalar_output{};
        uint64_t ool_output_size{};
    };

    io_connect_method_result io_surface_user_client_method(macos_emulator& emu, const io_connect_method_call& call);

    // The record selector 0 answers with. Its size is the one the client asks for and the one the kernel
    // writes, so a guest built against a different IOSurface would notice a different number.
    constexpr size_t IO_SURFACE_RECORD_SIZE = 3176;

    // An io_object port whose id carries this bit is a user-client connection rather than a registry
    // node. The iterator tag in mig_routines_iokit.cpp shares the same id space.
    constexpr uint64_t IOKIT_CONNECT_TAG = 0x4000000000000000ull;
}

namespace sogen::mach_traps
{
    // IOSurfaceLock, IOSurfaceUnlock, the use-count adjustments and the surface refcount do not go
    // through MIG at all: they are iokit_user_client_trap, mach trap 100, which is why a trace of the
    // IOKit MIG surface shows none of them.
    void trap_iokit_user_client(const macos_syscall_context& c);
}
