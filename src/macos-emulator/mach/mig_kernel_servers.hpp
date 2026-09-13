#pragma once

#include "mach_msg.hpp"
#include "mach_port_namespace.hpp"
#include "mach_types.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sogen
{
    class macos_emulator;
}

namespace sogen::mach
{
    struct mig_request
    {
        const msg_call& call;
        std::span<const uint8_t> body;
        kernel_object_kind destination{};
        int32_t id{};

        size_t args_offset{};

        bool has_ndr() const;
        size_t effective_args_offset() const;
        uint32_t arg_u32(size_t index) const;
        uint64_t arg_u64(size_t byte_offset) const;
        std::optional<port_descriptor> descriptor(size_t index) const;
    };

    mig_request make_mig_request(const msg_call& call, std::span<const uint8_t> body, kernel_object_kind destination);

    class mig_reply_builder
    {
      public:
        mig_reply_builder(const msg_call& call, mach_port_namespace& ports);

        void set_complex();
        void append_ndr();
        void append_u32(uint32_t value);
        void append_u64(uint64_t value);
        void append_port_descriptor(const port_descriptor& descriptor);
        void append_bytes(std::span<const uint8_t> bytes);
        std::vector<uint8_t> finish(int32_t reply_id_offset = subsystem::reply_offset);
        std::vector<uint8_t> finish_with_id(int32_t id);

      private:
        const msg_call& call_;
        mach_port_namespace& ports_;
        std::vector<uint8_t> bytes_{};
        uint32_t descriptor_count_{};
        bool complex_{};
    };

    using mig_routine = std::function<std::vector<uint8_t>(macos_emulator&, const mig_request&)>;

    class mig_server_table
    {
      public:
        void register_routine(kernel_object_kind destination, int32_t id, mig_routine routine, std::string name);
        const mig_routine* find(kernel_object_kind destination, int32_t id) const;
        std::string_view name_of(kernel_object_kind destination, int32_t id) const;

      private:
        struct entry
        {
            mig_routine routine{};
            std::string name{};
        };

        std::map<std::pair<kernel_object_kind, int32_t>, entry> routines_{};
    };

    std::vector<uint8_t> make_mig_error_bytes(const mig_request& request, kern_return_t code);

    void register_host_routines(mig_server_table& table);
    void register_task_routines(mig_server_table& table);
    void register_vm_routines(mig_server_table& table);
    void register_restartable_routines(mig_server_table& table);

    mig_server_table& kernel_mig_servers();
    std::vector<uint8_t> dispatch_kernel_message(macos_emulator& emu, const msg_call& call, std::span<const uint8_t> body,
                                                 kernel_object_kind destination);
}
