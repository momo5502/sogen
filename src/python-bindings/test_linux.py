import gc
import ctypes
import os
import shutil
import socket
import subprocess
import sys
import threading
import tempfile
import textwrap
from pathlib import Path

sys.path.insert(0, os.getcwd())
repo_artifacts = Path(__file__).resolve().parents[2] / "build" / "release" / "artifacts"
if repo_artifacts.exists():
    sys.path.insert(0, str(repo_artifacts))

import sogen

assert sogen.create_application is sogen.windows.create_application
assert sogen.create_empty is sogen.windows.create_empty

linux = sogen.linux
LINUX_SIGSEGV = 11
LINUX_SEGV_MAPERR = 1

assert linux.create_application is not sogen.create_application
assert hasattr(linux, "create_empty")
assert linux.create_empty is not sogen.create_empty
assert sogen.MemoryOperation is sogen.MemoryPermission
assert hasattr(linux, "ExportedSymbol")
assert hasattr(linux, "MappedSection")
assert hasattr(linux, "LinuxMappedModule")
assert hasattr(linux, "LinuxSymbolCall")
assert hasattr(linux, "SymbolHooks")
assert hasattr(linux, "symbol_call")

empty = linux.create_empty(emulation_root="/", backend=sogen.Backend.unicorn)
exec_base = empty.memory.allocate_memory(0x1000, sogen.MemoryPermission.exec)
data_base = empty.memory.allocate_memory(0x1000, sogen.MemoryPermission.read_write)
assert isinstance(exec_base, int)
assert isinstance(data_base, int)
empty.write_memory(data_base, b"linux-empty")
assert empty.read_memory(data_base, len(b"linux-empty")) == b"linux-empty"
empty.write_register(sogen.Register.rip, exec_base)
assert empty.read_register(sogen.Register.rip) == exec_base
state_payload = b"linux-empty"
assert empty.read_memory(data_base, len(state_payload)) == state_payload
empty.write_register(sogen.Register.rax, 0x123456789ABCDEF0)
serialized_state = empty.serialize_state()
empty.write_memory(data_base, b"mutated-123")
empty.write_register(sogen.Register.rax, 0xBADF00D)
empty.deserialize_state(serialized_state)
assert empty.read_memory(data_base, len(state_payload)) == state_payload
assert empty.read_register(sogen.Register.rax) == 0x123456789ABCDEF0
empty.save_snapshot()
empty.write_memory(data_base, b"snapshot-ok")
empty.write_register(sogen.Register.rax, 0xFEEDFACE)
empty.restore_snapshot()
assert empty.read_memory(data_base, len(state_payload)) == state_payload
assert empty.read_register(sogen.Register.rax) == 0x123456789ABCDEF0
assert empty.backend_name == "Unicorn Engine"
assert empty.process.thread_count == 0
assert hasattr(empty, "current_thread")
assert hasattr(empty, "current_thread_id")
assert hasattr(empty, "activate_thread")
assert hasattr(empty, "perform_thread_switch")
assert hasattr(empty, "yield_thread")
assert empty.current_thread is None
assert empty.current_thread_id is None
assert empty.activate_thread(999999) is False
assert empty.perform_thread_switch() is False
assert empty.process.threads == []
assert empty.last_stop_reason == "none"
assert isinstance(empty.last_stop_reason_code, int)
assert empty.last_stop_detail == ""
assert hasattr(empty, "debug")
for debug_method in (
    "set_breakpoint",
    "clear_breakpoint",
    "list_breakpoints",
    "step_into",
    "step_over",
    "step_out",
    "run_to",
    "continue_execution",
    "pause",
    "registers",
    "modules",
    "threads",
    "disassemble",
    "call_stack",
):
    assert hasattr(empty.debug, debug_method), f"debug facade missing {debug_method}"


assert hasattr(linux, "MemoryRegionInfo")
memory_allocate_events = []
memory_protect_events = []
memory_release_events = []
lifecycle_base = 0x300000
lifecycle_size = 0x1000


def on_memory_allocate(address, length, permissions, committed):
    assert empty.memory.get_region_info(address) is not None
    memory_allocate_events.append((address, length, permissions, committed))


def on_memory_protect(address, length, permissions):
    region = empty.memory.get_region_info(address)
    assert region is not None
    assert region.permissions == permissions
    memory_protect_events.append((address, length, permissions))


def on_memory_release(address, length):
    assert empty.memory.get_region_info(address) is None
    memory_release_events.append((address, length))


empty.callbacks.on_memory_allocate = on_memory_allocate
empty.callbacks.on_memory_protect = on_memory_protect
empty.callbacks.on_memory_release = on_memory_release
assert empty.memory.allocate_memory_at(lifecycle_base, lifecycle_size, sogen.MemoryPermission.read_write)
assert memory_allocate_events == [(lifecycle_base, lifecycle_size, sogen.MemoryPermission.read_write, True)]
assert not empty.memory.allocate_memory_at(lifecycle_base, lifecycle_size, sogen.MemoryPermission.read_write)
assert memory_allocate_events == [(lifecycle_base, lifecycle_size, sogen.MemoryPermission.read_write, True)]

mapped_regions = empty.memory.mapped_regions
assert mapped_regions
region = empty.memory.get_region_info(lifecycle_base + 0x80)
assert isinstance(region, linux.MemoryRegionInfo)
assert region.start == lifecycle_base
assert region.length == lifecycle_size
assert region.permissions == sogen.MemoryPermission.read_write
assert region.allocation_base == region.start
assert region.allocation_length == region.length
assert region.is_reserved is False
assert region.is_committed is True
assert region.initial_permissions == region.permissions
assert region.kind == sogen.MemoryRegionKind.private_allocation
assert empty.memory.get_region_info(0x90000000) is None
for mapped_region in mapped_regions:
    assert hasattr(mapped_region, "start")
    assert hasattr(mapped_region, "length")
    assert hasattr(mapped_region, "permissions")

assert empty.memory.protect_memory(lifecycle_base, lifecycle_size, sogen.MemoryPermission.exec) is True
assert memory_protect_events == [(lifecycle_base, lifecycle_size, sogen.MemoryPermission.exec)]
assert empty.memory.protect_memory(0x90000000, lifecycle_size, sogen.MemoryPermission.read) is False
assert memory_protect_events == [(lifecycle_base, lifecycle_size, sogen.MemoryPermission.exec)]
region = empty.memory.get_region_info(lifecycle_base)
assert region.permissions == sogen.MemoryPermission.exec
assert region.initial_permissions == sogen.MemoryPermission.exec

stats = empty.memory.compute_memory_stats()
assert set(stats) == {"region_count", "mapped_bytes", "executable_bytes"}
assert stats["region_count"] == len(empty.memory.get_mapped_regions())
assert stats["mapped_bytes"] >= lifecycle_size
assert stats["executable_bytes"] >= lifecycle_size

assert empty.memory.release_memory(0x90000000, 0) is False
assert memory_release_events == []
assert empty.memory.release_memory(lifecycle_base, lifecycle_size)
assert memory_release_events == [(lifecycle_base, lifecycle_size)]
assert empty.memory.get_region_info(lifecycle_base) is None
empty = None
gc.collect()

assert hasattr(linux, "Hook")
assert hasattr(linux, "Hooks")

hook_emu = linux.create_empty(emulation_root="/", backend=sogen.Backend.unicorn)
assert hasattr(hook_emu, "hooks")
assert not hasattr(hook_emu.hooks, "apis")
assert hook_emu.memory.allocate_memory_at(0x200000, 0x1000, sogen.MemoryPermission.read_write)
assert hook_emu.memory.allocate_memory_at(0x100000, 0x1000, sogen.MemoryPermission.exec)
hook_emu.write_memory(0x200000, b"ABCDEFGH")
hook_emu.write_memory(
    0x100000,
    bytes.fromhex(
        "48bb0000200000000000"  # mov rbx, 0x200000
        "488b03"                # mov rax, [rbx]
        "48894308"              # mov [rbx+8], rax
        "0fa2"                  # cpuid
    ),
)
hook_emu.write_register(sogen.Register.rip, 0x100000)

execution_hits = []
read_hits = []
write_hits = []
instruction_hits = []


def on_cpuid(data):
    instruction_hits.append(data)
    hook_emu.stop()
    return sogen.HookContinuation.skip


hook_handles = [
    hook_emu.hooks.memory_execution_at(0x100000, lambda address: execution_hits.append(address)),
    hook_emu.hooks.memory_read(0x200000, 8, lambda address, data: read_hits.append((address, bytes(data)))),
    hook_emu.hooks.memory_write(0x200008, 8, lambda address, data: write_hits.append((address, bytes(data)))),
    hook_emu.hooks.instruction(sogen.Instruction.cpuid, on_cpuid),
]
assert all(handle.active is True for handle in hook_handles)
hook_emu.start(50)
assert execution_hits, "execution hook did not fire"
assert execution_hits[0] == 0x100000
assert read_hits == [(0x200000, b"ABCDEFGH")]
assert write_hits == [(0x200008, b"ABCDEFGH")]
assert instruction_hits, "cpuid instruction hook did not fire"
assert hook_emu.read_memory(0x200008, 8) == b"ABCDEFGH"
for handle in hook_handles:
    handle.remove()
    assert handle.active is False
    handle.remove()
    assert handle.active is False
hook_emu = None
gc.collect()

bb_emu = linux.create_empty(emulation_root="/", backend=sogen.Backend.unicorn)
assert bb_emu.memory.allocate_memory_at(0x100000, 0x1000, sogen.MemoryPermission.exec)
bb_emu.write_memory(0x100000, b"\xeb\xfe")
bb_emu.write_register(sogen.Register.rip, 0x100000)
basic_blocks = []


def on_basic_block(block):
    assert hasattr(block, "address")
    assert hasattr(block, "instruction_count")
    assert hasattr(block, "size")
    basic_blocks.append((block.address, block.instruction_count, block.size))
    bb_emu.stop()


bb_handle = bb_emu.hooks.basic_block(on_basic_block)
bb_emu.start(10)
assert basic_blocks, "basic-block hook did not fire"
assert any(instruction_count > 0 or size > 0 for _, instruction_count, size in basic_blocks)
bb_handle.remove()
bb_emu = None
gc.collect()

int_emu = linux.create_empty(emulation_root="/", backend=sogen.Backend.unicorn)
assert int_emu.memory.allocate_memory_at(0x100000, 0x1000, sogen.MemoryPermission.exec)
int_emu.write_memory(0x100000, b"\xcc")
int_emu.write_register(sogen.Register.rip, 0x100000)
interrupts = []


def on_interrupt(interrupt):
    interrupts.append(interrupt)
    int_emu.stop()


int_handle = int_emu.hooks.interrupt(on_interrupt)
try:
    int_emu.start(10)
except Exception as exc:
    detail = getattr(int_emu, "last_stop_detail", "")
    raise AssertionError(f"interrupt hook did not fire before backend exception; stop_detail={detail!r}") from exc
assert interrupts == [3], (
    f"interrupt hook did not fire for int3: interrupts={interrupts!r}, "
    f"stop_detail={getattr(int_emu, 'last_stop_detail', '')!r}"
)
int_handle.remove()
int_emu = None
gc.collect()
fault_address = 0xDEAD0000

signal_fault_emu = linux.create_empty(emulation_root="/", backend=sogen.Backend.unicorn)
signal_fault_emu.write_register(sogen.Register.rip, fault_address)
signal_events = []


def on_signal(signum, signal_fault_addr, si_code):
    signal_events.append((signum, signal_fault_addr, si_code))


assert hasattr(signal_fault_emu.callbacks, "on_signal")
assert hasattr(signal_fault_emu.callbacks, "on_exception")
assert "on_signal/on_exception share the same signal callback slot" in repr(signal_fault_emu.callbacks)
signal_fault_emu.callbacks.on_signal = on_signal
signal_fault_emu.start(1)
assert signal_events, "Callbacks.on_signal did not fire"
assert signal_events[0] == (LINUX_SIGSEGV, fault_address, LINUX_SEGV_MAPERR)
signal_fault_emu = None
gc.collect()

step_out_diag_emu = linux.create_empty(emulation_root="/", backend=sogen.Backend.unicorn)
step_out_diag_emu.write_register(sogen.Register.rbp, 0)
try:
    step_out_diag_emu.debug.step_out()
except RuntimeError as exc:
    message = str(exc)
    assert "RBP is zero" in message and "run_to(address)" in message
else:
    raise AssertionError("step_out should diagnose a zero RBP")

step_out_diag_emu.write_register(sogen.Register.rbp, 0x300000)
try:
    step_out_diag_emu.debug.step_out()
except RuntimeError as exc:
    message = str(exc)
    assert "0x300008" in message and "run_to(address)" in message
else:
    raise AssertionError("step_out should diagnose an unreadable return-address slot")

assert step_out_diag_emu.memory.allocate_memory_at(0x300000, 0x1000, sogen.MemoryPermission.read_write)
step_out_diag_emu.write_memory(0x300000, b"\x00" * 16)
try:
    step_out_diag_emu.debug.step_out()
except RuntimeError as exc:
    message = str(exc)
    assert "zero saved return address" in message and "0x300008" in message and "run_to(address)" in message
else:
    raise AssertionError("step_out should diagnose a zero saved return address")
step_out_diag_emu = None
gc.collect()

debug_emu = linux.create_empty(emulation_root="/", backend=sogen.Backend.unicorn)
assert debug_emu.memory.allocate_memory_at(0x100000, 0x1000, sogen.MemoryPermission.exec)
debug_emu.write_memory(0x100000, b"\x0f\xa2\x0f\xa2\x0f\xa2")
debug_emu.write_register(sogen.Register.rip, 0x100000)
second_cpuid = 0x100002
third_cpuid = 0x100004
assert debug_emu.debug.set_breakpoint(second_cpuid) is True
assert debug_emu.debug.set_breakpoint(third_cpuid) is True
debug_emu.debug.continue_execution()
assert debug_emu.last_stop_reason == "breakpoint"
assert debug_emu.debug.registers()["rip"] == second_cpuid
debug_emu.debug.continue_execution()
assert debug_emu.last_stop_reason == "breakpoint"
assert debug_emu.debug.registers()["rip"] == third_cpuid
debug_emu.debug.step_into()
assert debug_emu.debug.registers()["rip"] > third_cpuid
assert debug_emu.debug.clear_breakpoint(second_cpuid) is True
assert debug_emu.debug.clear_breakpoint(third_cpuid) is True
debug_emu = None
gc.collect()

fault_emu = linux.create_empty(emulation_root="/", backend=sogen.Backend.unicorn)
fault_emu.write_register(sogen.Register.rip, fault_address)
fault_emu.start(1)
assert fault_emu.last_stop_reason == "unhandled_memory_violation"
assert hex(fault_address) in fault_emu.last_stop_detail
assert fault_emu.process.exit_status is None
fault_emu = None
gc.collect()

callback_fault_emu = linux.create_empty(emulation_root="/", backend=sogen.Backend.unicorn)
callback_fault_emu.write_register(sogen.Register.rip, fault_address)
callback_events = []
operation_type = type(sogen.MemoryOperation.exec)
violation_type_type = type(sogen.MemoryViolationType.unmapped)


def on_memory_violate(address, size, operation, violation_type):
    assert isinstance(address, int), f"address is not int: {address!r}"
    assert isinstance(size, int), f"size is not int: {size!r}"
    assert isinstance(operation, operation_type), f"operation is not MemoryOperation: {operation!r}"
    assert isinstance(violation_type, violation_type_type), f"type is not MemoryViolationType: {violation_type!r}"
    callback_events.append((address, size, operation, violation_type))
    callback_fault_emu.stop()
    return sogen.MemoryViolationContinuation.stop


callback_fault_emu.callbacks.on_memory_violate = on_memory_violate
callback_fault_emu.start(1)
assert callback_events, "Callbacks.on_memory_violate did not fire"
assert callback_events[0][0] == fault_address
assert callback_fault_emu.last_stop_reason == "unhandled_memory_violation"
callback_fault_emu = None
gc.collect()

hook_fault_emu = linux.create_empty(emulation_root="/", backend=sogen.Backend.unicorn)
hook_fault_emu.write_register(sogen.Register.rip, fault_address)
hook_events = []


def on_hook_memory_violate(address, size, operation, violation_type):
    hook_events.append((address, size, operation, violation_type))
    return sogen.MemoryViolationContinuation.stop


memory_violation_hook = hook_fault_emu.hooks.memory_violation(on_hook_memory_violate)
assert memory_violation_hook.active is True
hook_fault_emu.start(1)
assert hook_events, "Hooks.memory_violation did not fire"
assert hook_fault_emu.last_stop_reason == "unhandled_memory_violation"
memory_violation_hook.remove()
assert memory_violation_hook.active is False
hook_fault_emu = None

self_removing_fault_emu = linux.create_empty(emulation_root="/", backend=sogen.Backend.unicorn)
self_removing_fault_emu.write_register(sogen.Register.rip, fault_address)
self_removing_events = []
self_removing_handle = None


def on_self_removing_memory_violate(address, size, operation, violation_type):
    self_removing_events.append((address, size, operation, violation_type))
    self_removing_handle.remove()
    return sogen.MemoryViolationContinuation.stop


self_removing_handle = self_removing_fault_emu.hooks.memory_violation(on_self_removing_memory_violate)
self_removing_fault_emu.start(1)
assert self_removing_events and self_removing_events[0][0] == fault_address
assert self_removing_handle.active is False
self_removing_fault_emu = None
self_removing_handle = None
gc.collect()


resume_map_fault_emu = linux.create_empty(emulation_root="/", backend=sogen.Backend.unicorn)
resume_map_base = 0x410000
resume_map_fault_emu.write_register(sogen.Register.rip, resume_map_base)
resume_map_events = []
resume_cpuid_hits = []


def on_resume_map_memory_violate(address, size, operation, violation_type):
    resume_map_events.append((address, size, operation, violation_type))
    assert resume_map_fault_emu.memory.allocate_memory_at(resume_map_base, 0x1000, sogen.MemoryPermission.exec)
    resume_map_fault_emu.write_memory(resume_map_base, b"\x0f\xa2")
    return sogen.MemoryViolationContinuation.resume


def on_resume_cpuid(data):
    resume_cpuid_hits.append(data)
    resume_map_fault_emu.stop()
    return sogen.HookContinuation.skip


resume_map_fault_emu.callbacks.on_memory_violate = on_resume_map_memory_violate
resume_cpuid_handle = resume_map_fault_emu.hooks.instruction(sogen.Instruction.cpuid, on_resume_cpuid)
resume_map_fault_emu.start(10)
assert resume_map_events and resume_map_events[0][0] == resume_map_base
assert resume_cpuid_hits, "resume did not retry the newly mapped fetch page"
resume_cpuid_handle.remove()
resume_map_fault_emu = None
gc.collect()

restart_fault_emu = linux.create_empty(emulation_root="/", backend=sogen.Backend.unicorn)
restart_base = 0x400000
restart_fault_emu.write_register(sogen.Register.rip, restart_base)
restart_events = []
restart_cpuid_hits = []


def on_restart_memory_violate(address, size, operation, violation_type):
    restart_events.append((address, size, operation, violation_type))
    assert restart_fault_emu.memory.allocate_memory_at(restart_base, 0x1000, sogen.MemoryPermission.exec)
    restart_fault_emu.write_memory(restart_base, b"\x0f\xa2")
    return sogen.MemoryViolationContinuation.restart


def on_restart_cpuid(data):
    restart_cpuid_hits.append(data)
    restart_fault_emu.stop()
    return sogen.HookContinuation.skip


restart_fault_emu.callbacks.on_memory_violate = on_restart_memory_violate
restart_cpuid_handle = restart_fault_emu.hooks.instruction(sogen.Instruction.cpuid, on_restart_cpuid)
restart_fault_emu.start(10)
assert restart_events and restart_events[0][0] == restart_base
assert restart_cpuid_hits, "restart did not retry the newly mapped fetch page"
restart_cpuid_handle.remove()
restart_fault_emu = None
gc.collect()
gc.collect()



binary = os.getenv("LINUX_PYTHON_TEST_BINARY", "/bin/true")
root = os.getenv("LINUX_PYTHON_TEST_ROOT", "/")
requested_backend = os.getenv("LINUX_PYTHON_TEST_BACKEND", "unicorn")

backends = {
    "unicorn": (sogen.Backend.unicorn, "Unicorn Engine"),
    "kvm": (sogen.Backend.kvm, "Linux KVM"),
}
assert requested_backend in backends, f"unsupported backend: {requested_backend!r}"
backend, expected_backend_name = backends[requested_backend]

stdout_chunks = []
stderr_chunks = []
syscall_events = []


def capture_output(chunks):
    def capture(data):
        if isinstance(data, str):
            chunks.append(data.encode())
        else:
            chunks.append(bytes(data))

    return capture

def capture_syscall(syscall_id, syscall_name):
    assert isinstance(syscall_id, int), f"syscall id is not int: {syscall_id!r}"
    assert syscall_id >= 0, f"negative syscall id: {syscall_id!r}"
    assert isinstance(syscall_name, str), f"syscall name is not str: {syscall_name!r}"
    assert syscall_name, "empty syscall name"
    syscall_events.append((syscall_id, syscall_name))
    if len(syscall_events) == 1:
        return sogen.HookContinuation.run
    return None


app = linux.create_application(
    binary,
    emulation_root=root,
    backend=backend,
    disable_logging=True,
)
assert hasattr(app, "current_thread")
assert hasattr(app, "current_thread_id")
assert hasattr(app, "activate_thread")
assert hasattr(app, "perform_thread_switch")
assert hasattr(app, "yield_thread")
assert hasattr(app.callbacks, "on_thread_create")
assert hasattr(app.callbacks, "on_thread_terminated")
assert hasattr(app.callbacks, "on_thread_switch")
for debug_method in (
    "set_breakpoint",
    "clear_breakpoint",
    "list_breakpoints",
    "step_into",
    "step_over",
    "step_out",
    "run_to",
    "continue_execution",
    "pause",
    "registers",
    "modules",
    "threads",
    "disassemble",
    "call_stack",
):
    assert hasattr(app.debug, debug_method), f"debug facade missing {debug_method}"
assert app.current_thread_id is not None
assert app.current_thread is not None
assert app.current_thread.tid == app.current_thread_id
assert app.activate_thread(app.current_thread_id) is True
assert app.process.active_thread.tid == app.current_thread_id
assert app.process.threads
assert all(not thread.terminated for thread in app.process.threads)
for attr in ("current_ip", "start_address", "wait_state", "setup_done"):
    assert hasattr(app.current_thread, attr), f"thread missing {attr}"
assert "previous_ip" in dir(app.current_thread), "thread missing previous_ip property"
assert app.current_thread.current_ip == app.read_register(sogen.Register.rip)
try:
    _ = app.current_thread.previous_ip
except NotImplementedError as exc:
    assert "previous_ip is not tracked on Linux yet" in str(exc)
else:
    raise AssertionError("Thread.previous_ip must fail loudly until Linux tracks it")
assert hasattr(app.callbacks, "on_syscall")
app.callbacks.on_syscall = capture_syscall
app.callbacks.clear("on_syscall")
app.callbacks.set("on_syscall", capture_syscall)
app.callbacks.clear("syscall")
app.callbacks.on_stdout = capture_output(stdout_chunks)
app.callbacks.on_stderr = capture_output(stderr_chunks)
app.callbacks.set("syscall", capture_syscall)

modules = app.modules
assert modules, "Linux modules are empty before start"
main_modules = [module for module in modules if "true" in module.name or "true" in str(module.path)]
assert main_modules, f"main executable module not found in {[module.name for module in modules]!r}"
main_module = main_modules[0]
for module in modules:
    for attr in (
        "name",
        "path",
        "image_base",
        "size_of_image",
        "entry_point",
        "exports",
        "needed_libraries",
        "sections",
        "rpath",
        "runpath",
    ):
        assert hasattr(module, attr), f"module missing {attr}"
    assert isinstance(module.exports, list)
    assert isinstance(module.needed_libraries, list)
    assert isinstance(module.sections, list)
    if module.exports:
        assert isinstance(module.exports[0], linux.ExportedSymbol)
    if module.sections:
        assert isinstance(module.sections[0], linux.MappedSection)
assert isinstance(main_module, linux.LinuxMappedModule)
assert app.find_module_by_address(main_module.entry_point) is not None
assert app.find_module_by_name(main_module.name) is not None

module_load_replay = []
app.callbacks.on_module_load = module_load_replay.append
assert any(module.name == main_module.name for module in module_load_replay), "on_module_load did not replay main executable"
pre_start_state = app.serialize_state()
pre_start_mmap_base = app.memory.mmap_base
pre_start_module_count = len(app.modules)
app.memory.mmap_base = pre_start_mmap_base + 0x100000
app.deserialize_state(pre_start_state)
assert app.memory.mmap_base == pre_start_mmap_base
assert len(app.modules) == pre_start_module_count


app.start()

stdout_data = b"".join(stdout_chunks)
stderr_data = b"".join(stderr_chunks)

syscall_names = [name for _, name in syscall_events]

print(f"exit_status={app.process.exit_status}", flush=True)
print(f"backend={app.backend_name}", flush=True)
print(f"requested_backend={requested_backend}", flush=True)
print(f"executed_instructions={app.executed_instructions}", flush=True)
print(f"syscall_count={len(syscall_events)}", flush=True)
print(f"syscall_names={','.join(syscall_names[:20])}", flush=True)


assert syscall_events, "syscall callback did not fire"
assert "exit_group" in syscall_names or "exit" in syscall_names, (
    f"exit syscall not observed: {syscall_names!r}"
)
assert app.process.exit_status == 0, f"non-zero exit: {app.process.exit_status}"
assert app.last_stop_reason == "normal_exit"
assert stdout_data == b"", f"unexpected stdout: {stdout_data!r}"
assert stderr_data == b"", f"unexpected stderr: {stderr_data!r}"
assert app.backend_name == expected_backend_name, (
    f"backend name {app.backend_name!r} != {expected_backend_name!r}"
)
if requested_backend == "unicorn":
    assert app.executed_instructions > 0, "unicorn did not execute any instructions"

app = None
gc.collect()

cc = shutil.which("cc")
assert cc is not None, "cc is required for Linux cwd and socket fixtures"
with tempfile.TemporaryDirectory() as tmpdir:
    tmp_path = Path(tmpdir)
    source_path = tmp_path / "getcwd_fixture.c"
    fixture_path = tmp_path / "getcwd_fixture"
    source_path.write_text(
        textwrap.dedent(
            r"""
            #include <limits.h>
            #include <stdio.h>
            #include <unistd.h>

            int main(void) {
                char buffer[4096];
                if (getcwd(buffer, sizeof(buffer)) == NULL) {
                    return 1;
                }
                printf("%s\n", buffer);
                return 0;
            }
            """
        )
    )
    subprocess.run([cc, "-O0", str(source_path), "-o", str(fixture_path)], check=True)

    cwd_stdout = []
    cwd_app = linux.create_application(
        str(fixture_path),
        emulation_root=root,
        backend=backend,
        working_directory="/tmp",
        disable_logging=True,
    )
    cwd_app.callbacks.on_stdout = capture_output(cwd_stdout)
    cwd_app.start()
    assert b"".join(cwd_stdout) == b"/tmp\n"
    assert cwd_app.process.exit_status == 0

    host_work = tmp_path / "host-work"
    host_work.mkdir()
    (host_work / "mapped.txt").write_text("mapped\n")
    path_source = tmp_path / "path_fixture.c"
    path_fixture = tmp_path / "path_fixture"
    path_source.write_text(
        textwrap.dedent(
            r"""
            #include <fcntl.h>
            #include <unistd.h>

            int main(void) {
                char buffer[32] = {0};
                int fd = open("mapped.txt", O_RDONLY);
                if (fd < 0) {
                    return 1;
                }
                ssize_t count = read(fd, buffer, sizeof(buffer));
                close(fd);
                if (count <= 0) {
                    return 2;
                }
                write(1, buffer, (size_t)count);
                return 0;
            }
            """
        )
    )
    subprocess.run([cc, "-O0", str(path_source), "-o", str(path_fixture)], check=True)
    path_stdout = []
    path_app = linux.create_application(
        "/guest-bin/path_fixture",
        emulation_root=root,
        backend=backend,
        working_directory="/work",
        path_mappings={"/guest-bin": tmp_path, "/work": host_work},
        disable_logging=True,
    )
    path_app.callbacks.on_stdout = capture_output(path_stdout)
    path_app.start()
    assert b"".join(path_stdout) == b"mapped\n"
    assert path_app.process.exit_status == 0

    readonly_host = tmp_path / "readonly"
    readonly_host.mkdir()
    (readonly_host / "locked.txt").write_text("locked")
    readonly_source = tmp_path / "readonly_fixture.c"
    readonly_fixture = tmp_path / "readonly_fixture"
    readonly_source.write_text(
        textwrap.dedent(
            r"""
            #include <fcntl.h>
            #include <stdio.h>
            #include <string.h>
            #include <unistd.h>

            int main(void) {
                char buffer[16] = {0};
                int fd = open("/ro/locked.txt", O_RDONLY);
                if (fd < 0) {
                    return 1;
                }
                ssize_t count = read(fd, buffer, sizeof(buffer));
                close(fd);
                if (count != 6 || memcmp(buffer, "locked", 6) != 0) {
                    return 2;
                }
                fd = open("/ro/locked.txt", O_WRONLY | O_TRUNC);
                if (fd >= 0) {
                    close(fd);
                    return 3;
                }
                puts("readonly");
                return 0;
            }
            """
        )
    )
    subprocess.run([cc, "-O0", str(readonly_source), "-o", str(readonly_fixture)], check=True)
    readonly_stdout = []
    readonly_app = linux.create_application(
        str(readonly_fixture),
        emulation_root=root,
        backend=backend,
        read_only_path_mappings={"/ro": readonly_host},
        disable_logging=True,
    )
    readonly_app.callbacks.on_stdout = capture_output(readonly_stdout)
    readonly_app.start()
    assert b"".join(readonly_stdout) == b"readonly\n"
    assert readonly_app.process.exit_status == 0
    assert (readonly_host / "locked.txt").read_text() == "locked"

    mprotect_source = tmp_path / "mprotect_fixture.c"
    mprotect_fixture = tmp_path / "mprotect_fixture"
    mprotect_source.write_text(
        textwrap.dedent(
            r"""
            #include <errno.h>
            #include <stdio.h>
            #include <sys/mman.h>
            #include <unistd.h>

            int main(void) {
                errno = 0;
                if (mprotect((void *)0x700000000000ULL, 4096, PROT_READ) != -1) {
                    return 1;
                }
                if (errno != ENOMEM) {
                    return 2;
                }
                puts("enomem");
                return 0;
            }
            """
        )
    )
    subprocess.run([cc, "-O0", str(mprotect_source), "-o", str(mprotect_fixture)], check=True)
    mprotect_stdout = []
    mprotect_app = linux.create_application(
        str(mprotect_fixture),
        emulation_root=root,
        backend=backend,
        disable_logging=True,
    )
    mprotect_app.callbacks.on_stdout = capture_output(mprotect_stdout)
    mprotect_app.start()
    assert b"".join(mprotect_stdout) == b"enomem\n"
    assert mprotect_app.process.exit_status == 0

    writeonly_state_path = tmp_path / "writeonly-state.txt"
    writeonly_state_path.write_text("initial")
    writeonly_source = tmp_path / "writeonly_restore_fixture.c"
    writeonly_fixture = tmp_path / "writeonly_restore_fixture"
    writeonly_source.write_text(
        textwrap.dedent(
            r"""
            #include <fcntl.h>
            #include <stdio.h>
            #include <unistd.h>

            int main(void) {
                char buffer[1] = {0};
                int fd = open("/state/writeonly-state.txt", O_WRONLY);
                if (fd < 0) {
                    return 1;
                }
                if (write(fd, "updated", 7) != 7) {
                    close(fd);
                    return 2;
                }
                getpid();
                if (read(fd, buffer, sizeof(buffer)) >= 0) {
                    close(fd);
                    return 3;
                }
                close(fd);
                puts("writeonly");
                return 0;
            }
            """
        )
    )
    subprocess.run([cc, "-O0", str(writeonly_source), "-o", str(writeonly_fixture)], check=True)
    writeonly_stdout = []
    writeonly_restored = {"value": False}
    writeonly_app = linux.create_application(
        str(writeonly_fixture),
        emulation_root=root,
        backend=backend,
        path_mappings={"/state": tmp_path},
        disable_logging=True,
    )
    writeonly_app.callbacks.on_stdout = capture_output(writeonly_stdout)

    def restore_before_writeonly_read(syscall_id, syscall_name):
        if syscall_name == "getpid" and not writeonly_restored["value"]:
            writeonly_restored["value"] = True
            state = writeonly_app.serialize_state()
            writeonly_app.deserialize_state(state)
        return sogen.HookContinuation.run

    writeonly_app.callbacks.on_syscall = restore_before_writeonly_read
    writeonly_app.start()
    assert writeonly_restored["value"], "write-only fd restore hook did not run"
    assert b"".join(writeonly_stdout) == b"writeonly\n"
    assert writeonly_app.process.exit_status == 0

    fatal_signal_source = tmp_path / "fatal_signal_fixture.c"
    fatal_signal_fixture = tmp_path / "fatal_signal_fixture"
    fatal_signal_source.write_text(
        textwrap.dedent(
            r"""
            #include <signal.h>
            #include <unistd.h>

            int main(void) {
                kill(getpid(), SIGTERM);
                return 1;
            }
            """
        )
    )
    subprocess.run([cc, "-O0", str(fatal_signal_source), "-o", str(fatal_signal_fixture)], check=True)
    fatal_signal_events = []
    fatal_exception_events = []
    fatal_signal_app = linux.create_application(
        str(fatal_signal_fixture),
        emulation_root=root,
        backend=backend,
        disable_logging=True,
    )
    fatal_signal_app.callbacks.on_signal = lambda signum, fault_addr, si_code: fatal_signal_events.append(
        (signum, fault_addr, si_code)
    )
    fatal_signal_app.start()
    assert fatal_signal_events and fatal_signal_events[0][0] == 15

    fatal_exception_app = linux.create_application(
        str(fatal_signal_fixture),
        emulation_root=root,
        backend=backend,
        disable_logging=True,
    )
    fatal_exception_app.callbacks.on_exception = lambda signum, fault_addr, si_code: fatal_exception_events.append(
        (signum, fault_addr, si_code)
    )
    fatal_exception_app.start()
    assert fatal_exception_events and fatal_exception_events[0][0] == 15
    assert fatal_exception_app.last_stop_reason == "signal_termination"
    assert fatal_exception_app.process.exit_status == 143
    assert fatal_signal_app.last_stop_reason == "signal_termination"
    assert fatal_signal_app.process.exit_status == 143

with tempfile.TemporaryDirectory() as tmpdir:
    tmp_path = Path(tmpdir)
    source_path = tmp_path / "socket_proxy_fixture.c"
    fixture_path = tmp_path / "socket_proxy_fixture"
    source_path.write_text(
        textwrap.dedent(
            r"""
            #include <arpa/inet.h>
            #include <string.h>
            #include <sys/socket.h>
            #include <unistd.h>

            int main(void) {
                static const char dup_payload[] = "dup payload";
                static const char msg_payload[] = "sendmsg payload";
                static const char expected[] = "recvmsg response";
                char buffer[64] = {0};

                int fd = socket(AF_INET, SOCK_STREAM, 0);
                if (fd < 0) {
                    return 1;
                }

                struct sockaddr_in addr = {0};
                addr.sin_family = AF_INET;
                addr.sin_port = htons(40000);
                addr.sin_addr.s_addr = htonl(0x7f000001u);
                if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
                    close(fd);
                    return 2;
                }

                int dupfd = dup(fd);
                if (dupfd < 0) {
                    close(fd);
                    return 3;
                }
                if (send(dupfd, dup_payload, sizeof(dup_payload) - 1, 0) != (ssize_t)(sizeof(dup_payload) - 1)) {
                    close(dupfd);
                    close(fd);
                    return 4;
                }

                struct iovec send_iov = {
                    .iov_base = (void *)msg_payload,
                    .iov_len = sizeof(msg_payload) - 1,
                };
                struct msghdr send_msg = {
                    .msg_iov = &send_iov,
                    .msg_iovlen = 1,
                };
                if (sendmsg(fd, &send_msg, 0) != (ssize_t)(sizeof(msg_payload) - 1)) {
                    close(dupfd);
                    close(fd);
                    return 5;
                }

                struct iovec recv_iov = {
                    .iov_base = buffer,
                    .iov_len = sizeof(buffer),
                };
                struct msghdr recv_msg = {
                    .msg_iov = &recv_iov,
                    .msg_iovlen = 1,
                };
                ssize_t received = recvmsg(fd, &recv_msg, 0);
                if (received != (ssize_t)(sizeof(expected) - 1)) {
                    close(dupfd);
                    close(fd);
                    return 6;
                }
                if (memcmp(buffer, expected, sizeof(expected) - 1) != 0) {
                    close(dupfd);
                    close(fd);
                    return 7;
                }

                write(STDOUT_FILENO, "proxied\n", 8);
                close(dupfd);
                close(fd);
                return 0;
            }
            """
        )
    )
    subprocess.run([cc, "-O0", str(source_path), "-o", str(fixture_path)], check=True)

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind(("127.0.0.1", 0))
        server.listen(1)
        host_port = server.getsockname()[1]
        server_payloads = []

        def accept_once():
            conn, _ = server.accept()
            with conn:
                expected_payload = b"dup payloadsendmsg payload"
                chunks = []
                remaining = len(expected_payload)
                while remaining:
                    chunk = conn.recv(remaining)
                    if not chunk:
                        break
                    chunks.append(chunk)
                    remaining -= len(chunk)
                server_payloads.append(b"".join(chunks))
                conn.sendall(b"recvmsg response")

        thread = threading.Thread(target=accept_once, daemon=True)
        thread.start()

        connect_stdout = []
        connect_app = linux.create_application(
            str(fixture_path),
            emulation_root=root,
            backend=backend,
            port_mappings={40000: host_port},
            disable_logging=True,
        )
        assert connect_app.get_host_port(40000) == host_port
        assert connect_app.get_emulator_port(host_port) == 40000
        connect_app.callbacks.on_stdout = capture_output(connect_stdout)
        saw_recvmsg = {"value": False}
        socket_snapshot_errors = []

        def capture_socket_syscall(syscall_id, syscall_name):
            if syscall_name == "recvmsg":
                saw_recvmsg["value"] = True
            elif syscall_name == "close" and saw_recvmsg["value"] and not socket_snapshot_errors:
                try:
                    connect_app.serialize_state()
                except RuntimeError as exc:
                    socket_snapshot_errors.append(str(exc))
                else:
                    raise AssertionError("live mapped host socket serialized without an explicit rejection")
            return sogen.HookContinuation.run

        connect_app.callbacks.on_syscall = capture_socket_syscall
        connect_app.start()
        thread.join(timeout=5)
        assert server_payloads == [b"dup payloadsendmsg payload"], "host socket did not receive mapped Linux payload"
        assert b"".join(connect_stdout) == b"proxied\n"
        assert socket_snapshot_errors, "live mapped host socket snapshot was not explicitly rejected"
        assert connect_app.process.exit_status == 0

if requested_backend == "unicorn":
    cc = shutil.which("cc")
    assert cc is not None, "cc is required for Linux symbol hook fixture"
    with tempfile.TemporaryDirectory() as tmpdir:
        tmp_path = Path(tmpdir)
        source_path = tmp_path / "symbol_fixture.c"
        fixture_path = tmp_path / "symbol_fixture"
        source_path.write_text(
            textwrap.dedent(
                r"""
                __attribute__((noinline, visibility("default"))) int target_function(int value) {
                    return value + 1;
                }

                __attribute__((noinline, visibility("default")))
                int target_values(signed char a, unsigned char b, short c, unsigned short d, int e, unsigned int f) {
                    return (int)a + (int)b + (int)c + (int)d + e + (int)f;
                }

                int main(void) {
                    return target_function(41) == 42 &&
                           target_values((signed char)-2, (unsigned char)250, (short)-1234,
                                         (unsigned short)65000, -123456, 4000000000U) != 0
                               ? 0
                               : 1;
                }
                """
            )
        )
        subprocess.run([cc, "-O0", "-rdynamic", str(source_path), "-o", str(fixture_path)], check=True)

        symbol_app = linux.create_application(
            str(fixture_path),
            emulation_root=root,
            backend=backend,
            disable_logging=True,
        )
        assert isinstance(symbol_app.hooks.symbols, linux.SymbolHooks)

        @linux.symbol_call(params=[ctypes.c_double])
        def unsupported_symbol_signature(call, params):
            return None

        try:
            symbol_app.hooks.symbols["target_function"] = unsupported_symbol_signature
        except RuntimeError:
            pass
        else:
            raise AssertionError("unsupported Linux symbol hook signature did not fail at registration")

        def install_symbol_hooks(target_app, hits):
            @linux.symbol_call(params=[ctypes.c_int], restype=ctypes.c_int)
            def on_target_function(call, params):
                assert isinstance(call, linux.LinuxSymbolCall)
                assert isinstance(call.module, linux.LinuxMappedModule)
                assert call.name == "target_function"
                assert call.address != 0
                assert call.return_address != 0
                assert list(params) == [41]
                hits.append((call.module.name, call.name, call.address, call.return_address))
                return sogen.ApiContinuation.run_original

            @linux.symbol_call(
                params=[
                    ctypes.c_int8,
                    ctypes.c_uint8,
                    ctypes.c_int16,
                    ctypes.c_uint16,
                    ctypes.c_int,
                    ctypes.c_uint,
                ],
                restype=ctypes.c_int,
            )
            def on_target_values(call, params):
                assert call.name == "target_values"
                assert list(params) == [-2, 250, -1234, 65000, -123456, 4000000000]
                hits.append((call.module.name, call.name, tuple(params)))
                return sogen.ApiContinuation.run_original

            target_app.hooks.symbols["target_function"] = on_target_function
            target_app.hooks.symbols["target_values"] = on_target_values

        symbol_hits = []
        install_symbol_hooks(symbol_app, symbol_hits)
        serialized_symbol_state = symbol_app.serialize_state()
        symbol_app.deserialize_state(serialized_symbol_state)
        symbol_app.start()
        assert any(hit[1] == "target_function" for hit in symbol_hits), "Linux symbol hook did not fire after deserialize"
        assert any(hit[1] == "target_values" for hit in symbol_hits), "Linux signed/narrow symbol hook did not fire"
        assert symbol_app.process.exit_status == 0, f"symbol fixture non-zero exit: {symbol_app.process.exit_status}"

        snapshot_symbol_app = linux.create_application(
            str(fixture_path),
            emulation_root=root,
            backend=backend,
            disable_logging=True,
        )
        snapshot_symbol_hits = []
        install_symbol_hooks(snapshot_symbol_app, snapshot_symbol_hits)
        snapshot_symbol_app.save_snapshot()
        snapshot_symbol_app.restore_snapshot()
        snapshot_symbol_app.start()
        assert any(hit[1] == "target_function" for hit in snapshot_symbol_hits), (
            "Linux symbol hook did not fire after snapshot restore"
        )
        assert snapshot_symbol_app.process.exit_status == 0, (
            f"snapshot symbol fixture non-zero exit: {snapshot_symbol_app.process.exit_status}"
        )
        snapshot_symbol_app.hooks.symbols.clear()
        snapshot_symbol_app = None
        symbol_app.hooks.symbols["target_function"] = None
        symbol_app.hooks.symbols.clear()
        symbol_app = None
        gc.collect()

for _name in (
    "mapped_regions",
    "mapped_region",
    "region",
    "stats",
    "hook_handles",
    "bb_handle",
    "int_handle",
    "memory_violation_hook",
    "modules",
    "main_modules",
    "main_module",
    "module",
    "module_load_replay",
    "pre_start_state",
    "cwd_app",
    "path_app",
    "readonly_app",
    "connect_app",
    "symbol_app",
    "self_removing_handle",
    "restart_cpuid_handle",
    "resume_cpuid_handle",
    "resume_map_fault_emu",
    "mprotect_app",
    "fatal_signal_app",
    "fatal_exception_app",
    "snapshot_symbol_app",
):
    globals()[_name] = None
gc.collect()
