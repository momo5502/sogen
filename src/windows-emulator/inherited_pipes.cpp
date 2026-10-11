#include "inherited_pipes.hpp"
#include "windows_emulator.hpp"
#include "devices/named_pipe.hpp"

#include <stdexcept>
#include <unordered_map>

namespace sogen
{
    void install_inherited_pipes(windows_emulator& emulator, const process_create_request& request,
                                 std::vector<process_inherited_pipe> pipes)
    {
        std::unordered_map<uint64_t, handle> files;
        for (auto& inherited : pipes)
        {
            const auto file_id = inherited.file_id ? inherited.file_id : inherited.guest_handle;
            const auto existing = files.find(file_id);
            io_device_container container;
            if (existing != files.end())
            {
                container = emulator.process.devices.get(existing->second)->duplicate(inherited.access, inherited.inherit);
            }
            else
            {
                container = io_device_container{u"NamedPipe", emulator, {}};
                auto* pipe = container.get_internal_device<named_pipe>();
                pipe->name = u8_to_u16(inherited.name);
                pipe->endpoint = std::move(inherited.endpoint);
                container.granted_access = inherited.access;
                pipe->pipe_type = inherited.pipe_type;
                pipe->read_mode = inherited.read_mode;
                pipe->completion_mode = inherited.completion_mode;
                pipe->create_options = inherited.create_options;
                container.inherit = inherited.inherit;
                container.set_completion_notification_flags(inherited.completion_notification_flags);
                files.emplace(file_id, make_handle(inherited.guest_handle));
            }
            if (!emulator.process.devices.store_at(make_handle(inherited.guest_handle), std::move(container)))
            {
                throw std::runtime_error("Inherited guest pipe handle conflicts with child setup");
            }
        }
        emulator.process.process_params64.access([&](RTL_USER_PROCESS_PARAMETERS64& parameters) {
            parameters.StandardInput = request.standard_input;
            parameters.StandardOutput = request.standard_output;
            parameters.StandardError = request.standard_error;
        });
        if (emulator.process.process_params32)
        {
            emulator.process.process_params32->access([&](RTL_USER_PROCESS_PARAMETERS32& parameters) {
                parameters.StandardInput = static_cast<uint32_t>(request.standard_input);
                parameters.StandardOutput = static_cast<uint32_t>(request.standard_output);
                parameters.StandardError = static_cast<uint32_t>(request.standard_error);
            });
        }
    }
}
