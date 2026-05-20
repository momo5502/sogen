#pragma once

#include <nanobind/nanobind.h>
#include <nanobind/stl/filesystem.h>
#include <nanobind/stl/map.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/shared_ptr.h>
#include <nanobind/stl/set.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/string_view.h>
#include <nanobind/stl/unordered_map.h>
#include <nanobind/stl/vector.h>

#include <disassembler.hpp>
#include <backend_selection.hpp>
#include <windows_emulator.hpp>
#include <x86_register.hpp>

namespace nb = nanobind;

namespace sogen::py
{
    using sogen::backend_type;
    using sogen::emulator_hook;
    using sogen::emulator_thread;
    using sogen::function_calling_convention;
    using sogen::instruction_hook_continuation;
    using sogen::mapped_module;
    using sogen::memory_interface;
    using sogen::memory_manager;
    using sogen::memory_permission;
    using sogen::memory_region_kind;
    using sogen::memory_violation_continuation;
    using sogen::nt_memory_permission;
    using sogen::process_context;
    using sogen::stop_reason;
    using sogen::u16_to_u8;
    using sogen::windows_emulator;
    using sogen::x86_register;

    // ----- continuation enums (custom to bindings) -----
    enum class api_call_continuation
    {
        run_original,
        intercept,
    };

    // ----- forward declarations of binding types -----
    struct hook_handle;
    struct api_call_info;
    struct api_hook_registry;
    struct hook_registry;
    struct callback_registry;
    struct sogen_process_context;
    struct sogen_windows_emulator;

    // ----- helpers -----
    std::string stop_reason_to_string(stop_reason reason);

    api_call_continuation coerce_api_continuation(nb::handle result);
    instruction_hook_continuation coerce_instruction_continuation(nb::handle result);
    memory_violation_continuation coerce_memory_violation_continuation(nb::handle result);

    nb::bytes read_memory_bytes(const memory_interface& memory, uint64_t address, size_t size);
    void write_memory_bytes(memory_interface& memory, uint64_t address, const nb::bytes& buffer);

    nb::bytes serialize_state_bytes(const windows_emulator& emulator);
    void deserialize_state_bytes(windows_emulator& emulator, const nb::bytes& buffer);

    std::unique_ptr<windows_emulator> create_empty_emulator(const nb::kwargs& kwargs);
    std::unique_ptr<windows_emulator> create_application_emulator(const nb::object& application, const nb::object& args,
                                                                  const nb::kwargs& kwargs);

    void register_types_bindings(nb::module_& m);
    void register_windows_runtime_bindings(nb::module_& m);
    void register_runtime_bindings(nb::module_& m);
}
