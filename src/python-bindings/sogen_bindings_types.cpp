#include <nanobind/nanobind.h>

#include "sogen_internal.hpp"
#include <windows_emulator.hpp>

namespace sogen::py
{
    void register_types_bindings(nb::module_& m)
    {
        nb::enum_<backend_type>(m, "Backend")
            .value("unicorn", backend_type::unicorn)
            .value("icicle", backend_type::icicle)
            .value("whp", backend_type::whp)
            .value("kvm", backend_type::kvm)
            .value("fex", backend_type::fex)
            .export_values();

        nb::enum_<memory_permission>(m, "MemoryPermission")
            .value("none", memory_permission::none)
            .value("read", memory_permission::read)
            .value("write", memory_permission::write)
            .value("exec", memory_permission::exec)
            .value("read_write", memory_permission::read_write)
            .value("read_exec", memory_permission::read_exec)
            .value("write_exec", memory_permission::write_exec)
            .value("all", memory_permission::all)
            .export_values();

        nb::setattr(m, "MemoryOperation", m.attr("MemoryPermission"));

        nb::enum_<memory_region_kind>(m, "MemoryRegionKind")
            .value("free", memory_region_kind::free)
            .value("private_allocation", memory_region_kind::private_allocation)
            .value("file_section_view", memory_region_kind::file_section_view)
            .value("pagefile_section_view", memory_region_kind::pagefile_section_view)
            .value("section_image", memory_region_kind::section_image)
            .value("mmio", memory_region_kind::mmio)
            .export_values();

        nb::enum_<memory_violation_type>(m, "MemoryViolationType")
            .value("unmapped", memory_violation_type::unmapped)
            .value("protection", memory_violation_type::protection)
            .export_values();

        nb::enum_<x86_register>(m, "Register")
            .value("invalid", x86_register::invalid)
            .value("rax", x86_register::rax)
            .value("rbx", x86_register::rbx)
            .value("rcx", x86_register::rcx)
            .value("rdx", x86_register::rdx)
            .value("rsi", x86_register::rsi)
            .value("rdi", x86_register::rdi)
            .value("rbp", x86_register::rbp)
            .value("rsp", x86_register::rsp)
            .value("rip", x86_register::rip)
            .value("r8", x86_register::r8)
            .value("r9", x86_register::r9)
            .value("r10", x86_register::r10)
            .value("r11", x86_register::r11)
            .value("r12", x86_register::r12)
            .value("r13", x86_register::r13)
            .value("r14", x86_register::r14)
            .value("r15", x86_register::r15)
            .value("eax", x86_register::eax)
            .value("ebx", x86_register::ebx)
            .value("ecx", x86_register::ecx)
            .value("edx", x86_register::edx)
            .value("esi", x86_register::esi)
            .value("edi", x86_register::edi)
            .value("ebp", x86_register::ebp)
            .value("esp", x86_register::esp)
            .value("eip", x86_register::eip)
            .value("eflags", x86_register::eflags)
            .value("rflags", x86_register::rflags)
            .value("cs", x86_register::cs)
            .value("ss", x86_register::ss)
            .value("ds", x86_register::ds)
            .value("es", x86_register::es)
            .value("fs", x86_register::fs)
            .value("gs", x86_register::gs)
            .value("xmm0", x86_register::xmm0)
            .value("xmm1", x86_register::xmm1)
            .value("xmm2", x86_register::xmm2)
            .value("xmm3", x86_register::xmm3)
            .value("xmm4", x86_register::xmm4)
            .value("xmm5", x86_register::xmm5)
            .value("xmm6", x86_register::xmm6)
            .value("xmm7", x86_register::xmm7)
            .value("xmm8", x86_register::xmm8)
            .value("xmm9", x86_register::xmm9)
            .value("xmm10", x86_register::xmm10)
            .value("xmm11", x86_register::xmm11)
            .value("xmm12", x86_register::xmm12)
            .value("xmm13", x86_register::xmm13)
            .value("xmm14", x86_register::xmm14)
            .value("xmm15", x86_register::xmm15)
            .export_values();

        nb::class_<memory_stats>(m, "MemoryStats")
            .def_prop_ro("reserved_memory", [](const memory_stats& self) { return self.reserved_memory; })
            .def_prop_ro("committed_memory", [](const memory_stats& self) { return self.committed_memory; });

        nb::class_<handle>(m, "Handle")
            .def(nb::init<>())
            .def_prop_rw(
                "bits", [](const handle& self) { return self.bits; }, [](handle& self, uint64_t value) { self.bits = value; })
            .def_prop_ro("id", [](const handle& self) { return self.value.id; })
            .def_prop_ro("type", [](const handle& self) { return self.value.type; })
            .def_prop_ro("is_system", [](const handle& self) { return self.value.is_system != 0; })
            .def_prop_ro("is_pseudo", [](const handle& self) { return self.value.is_pseudo != 0; })
            .def_prop_ro("high_bits", [](const handle& self) { return self.value.high_bits; });

        nb::class_<region_info>(m, "MemoryRegionInfo")
            .def_prop_ro("start", [](const region_info& self) { return self.start; })
            .def_prop_ro("length", [](const region_info& self) { return self.length; })
            .def_prop_ro("permissions", [](const region_info& self) { return self.permissions; })
            .def_prop_ro("allocation_base", [](const region_info& self) { return self.allocation_base; })
            .def_prop_ro("allocation_length", [](const region_info& self) { return self.allocation_length; })
            .def_prop_ro("is_reserved", [](const region_info& self) { return self.is_reserved; })
            .def_prop_ro("is_committed", [](const region_info& self) { return self.is_committed; })
            .def_prop_ro("initial_permissions", [](const region_info& self) { return self.initial_permissions; })
            .def_prop_ro("kind", [](const region_info& self) { return self.kind; });

        nb::enum_<instruction_hook_continuation>(m, "HookContinuation")
            .value("run", instruction_hook_continuation::run_instruction)
            .value("skip", instruction_hook_continuation::skip_instruction)
            .value("finalize_rip", instruction_hook_continuation::finalized_instruction_pointer)
            .export_values();

        nb::enum_<memory_violation_continuation>(m, "MemoryViolationContinuation")
            .value("stop", memory_violation_continuation::stop)
            .value("resume", memory_violation_continuation::resume)
            .value("restart", memory_violation_continuation::restart)
            .export_values();

        nb::enum_<x86_hookable_instructions>(m, "Instruction")
            .value("invalid", x86_hookable_instructions::invalid)
            .value("syscall", x86_hookable_instructions::syscall)
            .value("cpuid", x86_hookable_instructions::cpuid)
            .value("rdtsc", x86_hookable_instructions::rdtsc)
            .value("rdtscp", x86_hookable_instructions::rdtscp)
            .export_values();

        nb::enum_<function_calling_convention>(m, "CallingConvention")
            .value("cdecl", function_calling_convention::x86_cdecl)
            .value("stdcall", function_calling_convention::x86_stdcall)
            .value("fastcall", function_calling_convention::x64_fastcall)
            .value("syscall", function_calling_convention::x64_syscall)
            .export_values();

        nb::enum_<api_call_continuation>(m, "ApiContinuation")
            .value("run_original", api_call_continuation::run_original)
            .value("intercept", api_call_continuation::intercept)
            .value("skip", api_call_continuation::intercept)
            .export_values();

        nb::class_<api_call_info>(m, "ApiCall")
            .def_prop_ro("module", [](const api_call_info& self) { return self.module; })
            .def_prop_ro("name", [](const api_call_info& self) { return self.name; })
            .def_prop_ro("address", [](const api_call_info& self) { return self.address; })
            .def_prop_ro("return_address", [](const api_call_info& self) { return self.return_address; })
            .def_prop_rw(
                "return_value", [](const api_call_info& self) { return self.return_value; },
                [](api_call_info& self, uint64_t value) { self.return_value = value; });

        nb::class_<basic_block>(m, "BasicBlock")
            .def_prop_ro("address", [](const basic_block& self) { return self.address; })
            .def_prop_ro("instruction_count", [](const basic_block& self) { return self.instruction_count; })
            .def_prop_ro("size", [](const basic_block& self) { return self.size; });

        nb::class_<exported_symbol>(m, "ExportedSymbol")
            .def_prop_ro("name", [](const exported_symbol& self) { return self.name; })
            .def_prop_ro("ordinal", [](const exported_symbol& self) { return self.ordinal; })
            .def_prop_ro("rva", [](const exported_symbol& self) { return self.rva; })
            .def_prop_ro("address", [](const exported_symbol& self) { return self.address; });

        nb::class_<mapped_module>(m, "MappedModule")
            .def_prop_ro("name", [](const mapped_module& self) { return self.name; })
            .def_prop_ro("path", [](const mapped_module& self) { return self.path; })
            .def_prop_ro("module_path", [](const mapped_module& self) { return self.module_path.string(); })
            .def_prop_ro("image_base", [](const mapped_module& self) { return self.image_base; })
            .def_prop_ro("image_base_file", [](const mapped_module& self) { return self.image_base_file; })
            .def_prop_ro("size_of_image", [](const mapped_module& self) { return self.size_of_image; })
            .def_prop_ro("entry_point", [](const mapped_module& self) { return self.entry_point; })
            .def_prop_ro(
                "exports", [](const mapped_module& self) -> const exported_symbols& { return self.exports; },
                nb::rv_policy::reference_internal)
            .def_prop_ro("is_static", [](const mapped_module& self) { return self.is_static; });
    }
}
