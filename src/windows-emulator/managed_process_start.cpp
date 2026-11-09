#include "std_include.hpp"
#include "managed_process_start.hpp"
#include "inherited_pipes.hpp"

#include <subprocess_process_manager.hpp>

#include <stdexcept>

namespace sogen
{
    void apply_managed_request(application_settings& settings, const process_create_request& request)
    {
        settings.argument0 = u8_to_u16(request.argument0);
        settings.command_line = u8_to_u16(request.command_line);
        settings.process_id = request.process_id;
        settings.thread_id = request.thread_id;
        if (!request.working_directory.empty())
        {
            settings.working_directory = windows_path(u8_to_u16(request.working_directory));
        }
        for (const auto& [name, value] : request.environment)
        {
            settings.environment.insert_or_assign(u8_to_u16(name), u8_to_u16(value));
        }
    }

    std::unique_ptr<emulator_process_target> start_managed_process(windows_emulator& emulator, managed_process_connection& connection)
    {
        const auto& request = connection.request();
        if (!request.registry_state.empty())
        {
            utils::buffer_deserializer state{request.registry_state};
            emulator.registry.deserialize_runtime_state(state);
        }

        emulator.setup_process_if_necessary();
        emulator.process.parent_process_id = request.parent_process_id;
        if (request.debugged)
        {
            emulator.attach_debugger();
        }

        install_inherited_pipes(emulator, request, connection.take_inherited_pipes());
        auto target = std::make_unique<emulator_process_target>(emulator);
        if (!connection.wait_for_resume(*target))
        {
            throw std::runtime_error("Acknowledging managed process startup failed");
        }
        return target;
    }
}
