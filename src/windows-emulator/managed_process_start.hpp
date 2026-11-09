#pragma once

#include "emulator_process_target.hpp"

#include <memory>

namespace sogen
{
    class managed_process_connection;

    // The application, command line, ids, working directory and environment the parent gave the child process.
    void apply_managed_request(application_settings& settings, const process_create_request& request);

    // Sets the emulator up as the child process the parent asked for, reports that it started and waits until the parent
    // resumes it. The returned target serves the parent's requests and must outlive the emulation.
    std::unique_ptr<emulator_process_target> start_managed_process(windows_emulator& emulator, managed_process_connection& connection);
}
