use icicle_cpu::ExceptionCode;
use icicle_cpu::ValueSource;
use std::collections::HashSet;
use std::{cell::RefCell, collections::HashMap, rc::Rc};

use crate::registers;

fn create_x64_vm() -> icicle_vm::Vm {
    let mut cpu_config = icicle_vm::cpu::Config::from_target_triple("x86_64-none");
    cpu_config.enable_jit = false;
    cpu_config.enable_jit_mem = true;
    cpu_config.enable_shadow_stack = false;
    cpu_config.enable_recompilation = true;
    cpu_config.track_uninitialized = false;
    cpu_config.optimize_instructions = true;
    cpu_config.optimize_block = false;

    return icicle_vm::build(&cpu_config).unwrap();
}

const FOREIGN_READ: u8 = 1 << 0;
const FOREIGN_WRITE: u8 = 1 << 1;
const FOREIGN_EXEC: u8 = 1 << 2;

fn map_permissions(foreign_permissions: u8) -> u8 {
    let mut permissions: u8 = 0;

    if (foreign_permissions & FOREIGN_READ) != 0 {
        permissions |= icicle_vm::cpu::mem::perm::READ;
    }

    if (foreign_permissions & FOREIGN_WRITE) != 0 {
        permissions |= icicle_vm::cpu::mem::perm::WRITE;
    }

    if (foreign_permissions & FOREIGN_EXEC) != 0 {
        permissions |= icicle_vm::cpu::mem::perm::EXEC;
    }

    return permissions;
}

#[repr(u8)]
#[allow(dead_code)]
#[derive(PartialEq)]
enum HookType {
    Syscall = 1,
    Read,
    Write,
    ExecuteGeneric,
    ExecuteSpecific,
    Violation,
    Interrupt,
    Block,
    Unknown,
}

fn u8_to_hook_type_unsafe(value: u8) -> HookType {
    unsafe { std::mem::transmute(value) }
}

fn split_hook_id(id: u32) -> (u32, HookType) {
    let hook_id = id & 0xFFFFFF;
    let hook_type = u8_to_hook_type_unsafe((id >> 24) as u8);

    return (hook_id, hook_type);
}

fn qualify_hook_id(hook_id: u32, hook_type: HookType) -> u32 {
    let hook_type: u32 = (hook_type as u8).into();
    let hook_type_mask: u32 = hook_type << 24;
    return (hook_id | hook_type_mask).into();
}

pub struct HookContainer<Func: ?Sized> {
    hook_id: u32,
    is_iterating: bool,
    hooks: HashMap<u32, Box<Func>>,
    hooks_to_add: HashMap<u32, Box<Func>>,
    hooks_to_remove: HashSet<u32>,
}

impl<Func: ?Sized> HookContainer<Func> {
    pub fn new() -> Self {
        Self {
            hook_id: 0,
            is_iterating: false,
            hooks: HashMap::new(),
            hooks_to_add: HashMap::new(),
            hooks_to_remove: HashSet::new(),
        }
    }

    pub fn add_hook(&mut self, callback: Box<Func>) -> u32 {
        self.hook_id += 1;
        let id = self.hook_id;

        if self.is_iterating {
            self.hooks_to_add.insert(id, callback);
        } else {
            self.hooks.insert(id, callback);
        }

        return id;
    }

    pub fn for_each_hook<F>(&mut self, mut callback: F)
    where
        F: FnMut(&Func),
    {
        let was_iterating = self.do_pre_access_work();

        for (_, func) in &self.hooks {
            callback(func.as_ref());
        }

        self.do_post_access_work(was_iterating);
    }

    pub fn access_hook<F>(&mut self, id: u32, mut callback: F)
    where
        F: FnMut(&Func),
    {
        let was_iterating = self.do_pre_access_work();

        let hook = self.hooks.get(&id);
        if hook.is_some() {
            callback(hook.unwrap().as_ref());
        }

        self.do_post_access_work(was_iterating);
    }

    pub fn is_empty(&self) -> bool {
        return self.hooks.is_empty();
    }

    pub fn remove_hook(&mut self, id: u32) {
        if self.is_iterating {
            self.hooks_to_remove.insert(id);
        } else {
            self.hooks.remove(&id);
        }
    }

    fn do_pre_access_work(&mut self) -> bool {
        let was_iterating = self.is_iterating;
        self.is_iterating = true;
        return was_iterating;
    }

    fn do_post_access_work(&mut self, was_iterating: bool) {
        self.is_iterating = was_iterating;
        if self.is_iterating {
            return;
        }

        let to_remove = std::mem::take(&mut self.hooks_to_remove);
        for id in &to_remove {
            self.hooks.remove(&id);
        }

        let to_add = std::mem::take(&mut self.hooks_to_add);
        for (id, func) in to_add {
            self.hooks.insert(id, func);
        }
    }
}

struct InstructionHookInjector {
    inst_hook: pcode::HookId,
    block_hook: pcode::HookId,
}

fn count_instructions(block: &icicle_cpu::lifter::Block) -> u64 {
    return block.pcode.instructions.iter().fold(0u64, |count, &stmt| {
        if let pcode::Op::InstructionMarker = stmt.op {
            return count + 1;
        }

        return count;
    });
}

impl icicle_vm::CodeInjector for InstructionHookInjector {
    fn inject(
        &mut self,
        _cpu: &mut icicle_vm::cpu::Cpu,
        group: &icicle_vm::cpu::BlockGroup,
        code: &mut icicle_vm::BlockTable,
    ) {
        for id in group.range() {
            let block = &mut code.blocks[id];

            let mut tmp_block = pcode::Block::new();
            tmp_block.next_tmp = block.pcode.next_tmp;

            let mut is_first_inst = true;
            let inst_count = count_instructions(&block);

            for stmt in block.pcode.instructions.drain(..) {
                tmp_block.push(stmt);
                if let pcode::Op::InstructionMarker = stmt.op {
                    if is_first_inst {
                        is_first_inst = false;
                        tmp_block.push((pcode::Op::Arg(0), pcode::Inputs::one(inst_count)));
                        tmp_block.push(pcode::Op::Hook(self.block_hook));
                    }

                    tmp_block.push(pcode::Op::Hook(self.inst_hook));
                    code.modified.insert(id);
                }
            }

            std::mem::swap(&mut tmp_block.instructions, &mut block.pcode.instructions);
        }
    }
}

struct ExecutionHooks {
    stop: Rc<RefCell<bool>>,
    generic_hooks: HookContainer<dyn Fn(u64)>,
    specific_hooks: HookContainer<dyn Fn(u64)>,
    block_hooks: HookContainer<dyn Fn(u64, u64)>,
    address_mapping: HashMap<u64, Vec<u32>>,
    one_time_callbacks: Vec<Box<dyn Fn()>>,
}

impl ExecutionHooks {
    pub fn new(stop_value: Rc<RefCell<bool>>) -> Self {
        Self {
            stop: stop_value,
            generic_hooks: HookContainer::new(),
            specific_hooks: HookContainer::new(),
            block_hooks: HookContainer::new(),
            address_mapping: HashMap::new(),
            one_time_callbacks: Vec::new(),
        }
    }

    fn run_hooks(&mut self, address: u64) {
        if !self.one_time_callbacks.is_empty() {
            let callbacks = std::mem::take(&mut self.one_time_callbacks);
            for cb in callbacks {
                cb.as_ref()();
            }
        }

        self.generic_hooks.for_each_hook(|func| {
            func(address);
        });

        let mapping = self.address_mapping.get(&address);
        if mapping.is_none() {
            return;
        }

        for id in mapping.unwrap() {
            self.specific_hooks.access_hook(*id, |func| {
                func(address);
            });
        }
    }

    pub fn on_block(&mut self, address: u64, instructions: u64) {
        self.block_hooks.for_each_hook(|func| {
            func(address, instructions);
        });
    }

    pub fn execute(&mut self, cpu: &mut icicle_cpu::Cpu, address: u64) {
        self.run_hooks(address);

        if *self.stop.borrow() {
            cpu.exception.code = ExceptionCode::InstructionLimit as u32;
            cpu.exception.value = address;
        }
    }

    pub fn add_block_hook(&mut self, callback: Box<dyn Fn(u64, u64)>) -> u32 {
        self.block_hooks.add_hook(callback)
    }

    pub fn remove_block_hook(&mut self, id: u32) {
        self.block_hooks.remove_hook(id);
    }

    pub fn add_generic_hook(&mut self, callback: Box<dyn Fn(u64)>) -> u32 {
        self.generic_hooks.add_hook(callback)
    }

    pub fn add_specific_hook(&mut self, address: u64, callback: Box<dyn Fn(u64)>) -> u32 {
        let id = self.specific_hooks.add_hook(callback);

        let mapping = self.address_mapping.entry(address).or_insert_with(Vec::new);
        mapping.push(id);

        return id;
    }

    pub fn schedule(&mut self, callback: Box<dyn Fn()>) {
        self.one_time_callbacks.push(callback);
    }

    pub fn remove_generic_hook(&mut self, id: u32) {
        self.generic_hooks.remove_hook(id);
    }

    pub fn remove_specific_hook(&mut self, id: u32) {
        self.address_mapping.retain(|_, vec| {
            vec.retain(|&x| x != id);
            !vec.is_empty()
        });

        self.specific_hooks.remove_hook(id);
    }
}

pub struct IcicleEmulator {
    executing_thread: std::thread::ThreadId,
    vm: icicle_vm::Vm,
    reg: registers::X86RegisterNodes,
    syscall_hooks: HookContainer<dyn Fn()>,
    interrupt_hooks: HookContainer<dyn Fn(i32)>,
    violation_hooks: HookContainer<dyn Fn(u64, u8, bool) -> bool>,
    execution_hooks: Rc<RefCell<ExecutionHooks>>,
    stop: Rc<RefCell<bool>>,
    snapshots: Vec<Box<icicle_vm::Snapshot>>,
}

struct MemoryHook {
    callback: Box<dyn Fn(u64, &[u8])>,
}

impl icicle_cpu::mem::WriteHook for MemoryHook {
    fn write(&mut self, _mem: &mut icicle_cpu::Mmu, addr: u64, value: &[u8]) {
        (self.callback)(addr, value);
    }
}

impl icicle_cpu::mem::ReadAfterHook for MemoryHook {
    fn read(&mut self, _mem: &mut icicle_cpu::Mmu, addr: u64, value: &[u8]) {
        (self.callback)(addr, value);
    }
}

pub struct MmioHandler {
    read_handler: Box<dyn Fn(u64, &mut [u8])>,
    write_handler: Box<dyn Fn(u64, &[u8])>,
}

impl MmioHandler {
    pub fn new(
        read_function: Box<dyn Fn(u64, &mut [u8])>,
        write_function: Box<dyn Fn(u64, &[u8])>,
    ) -> Self {
        Self {
            read_handler: read_function,
            write_handler: write_function,
        }
    }
}

impl icicle_cpu::mem::IoMemory for MmioHandler {
    fn read(&mut self, addr: u64, buf: &mut [u8]) -> icicle_cpu::mem::MemResult<()> {
        (self.read_handler)(addr, buf);
        return Ok(());
    }

    fn write(&mut self, addr: u64, value: &[u8]) -> icicle_cpu::mem::MemResult<()> {
        (self.write_handler)(addr, value);
        return Ok(());
    }
}

impl IcicleEmulator {
    pub fn new() -> Self {
        let mut virtual_machine = create_x64_vm();
        let capacity_400mb = 50_000;

        let mut capacity = 8 * 2 * capacity_400mb; // ~8gb
        if cfg!(target_pointer_width = "32") {
            capacity = 2 * capacity_400mb; // ~1gb
        }

        virtual_machine.cpu.mem.set_capacity(capacity);

        let stop_value = Rc::new(RefCell::new(false));
        let exec_hooks = Rc::new(RefCell::new(ExecutionHooks::new(stop_value.clone())));

        let inst_exec_hooks = Rc::clone(&exec_hooks);

        let inst_hook = icicle_cpu::InstHook::new(move |cpu: &mut icicle_cpu::Cpu, addr: u64| {
            inst_exec_hooks.borrow_mut().execute(cpu, addr);
        });

        let block_exec_hooks = Rc::clone(&exec_hooks);

        let block_hook = icicle_cpu::InstHook::new(move |cpu: &mut icicle_cpu::Cpu, addr: u64| {
            let instructions = cpu.args[0] as u64;
            block_exec_hooks.borrow_mut().on_block(addr, instructions);
        });

        let inst_hook_id = virtual_machine.cpu.add_hook(inst_hook);
        let block_hook_id = virtual_machine.cpu.add_hook(block_hook);
        virtual_machine.add_injector(InstructionHookInjector {
            inst_hook: inst_hook_id,
            block_hook: block_hook_id,
        });

        Self {
            stop: stop_value,
            executing_thread: std::thread::current().id(),
            reg: registers::X86RegisterNodes::new(&virtual_machine.cpu.arch),
            vm: virtual_machine,
            syscall_hooks: HookContainer::new(),
            interrupt_hooks: HookContainer::new(),
            violation_hooks: HookContainer::new(),
            execution_hooks: exec_hooks,
            snapshots: Vec::new(),
        }
    }

    fn get_mem(&mut self) -> &mut icicle_vm::cpu::Mmu {
        return &mut self.vm.cpu.mem;
    }

    pub fn start(&mut self, count: u64) {
        self.executing_thread = std::thread::current().id();

        self.vm.icount_limit = match count {
            0 => u64::MAX,
            _ => self.vm.cpu.icount.saturating_add(count),
        };

        loop {
            self.vm.cpu.block_id = u64::MAX;
            self.vm.cpu.block_offset = 0;
            self.vm.cpu.pending_exception = None;
            self.vm.cpu.exception.clear();
            *self.stop.borrow_mut() = false;

            let reason = self.vm.run();

            match reason {
                icicle_vm::VmExit::InstructionLimit => break,
                icicle_vm::VmExit::UnhandledException((code, value)) => {
                    let continue_execution = self.handle_exception(code, value);
                    if !continue_execution {
                        break;
                    }
                }
                _ => break,
            };
        }
    }

    fn handle_interrupt(&mut self, code: i32) -> bool {
        self.interrupt_hooks.for_each_hook(|func| {
            func(code);
        });

        return true;
    }

    fn handle_exception(&mut self, code: ExceptionCode, value: u64) -> bool {
        let continue_execution = match code {
            ExceptionCode::Syscall => self.handle_syscall(value),
            ExceptionCode::ReadPerm => self.handle_violation(value, FOREIGN_READ, false),
            ExceptionCode::WritePerm => self.handle_violation(value, FOREIGN_WRITE, false),
            ExceptionCode::ReadUnmapped => self.handle_violation(value, FOREIGN_READ, true),
            ExceptionCode::WriteUnmapped => self.handle_violation(value, FOREIGN_WRITE, true),
            ExceptionCode::SoftwareBreakpoint => self.handle_interrupt(3),
            ExceptionCode::InvalidInstruction => self.handle_interrupt(6),
            ExceptionCode::DivisionException => self.handle_interrupt(0),
            _ => false,
        };

        return continue_execution;
    }

    fn handle_violation(&mut self, address: u64, permission: u8, unmapped: bool) -> bool {
        if self.violation_hooks.is_empty() {
            return false;
        }

        let mut continue_execution = true;

        self.violation_hooks.for_each_hook(|func| {
            continue_execution &= func(address, permission, unmapped);
        });

        return continue_execution;
    }

    fn handle_syscall(&mut self, value: u64) -> bool {
        if value != 0 {
            return self.handle_interrupt(value as i32);
        }

        self.syscall_hooks.for_each_hook(|func| {
            func();
        });

        self.vm.cpu.write_pc(self.vm.cpu.read_pc() + 2);
        return true;
    }

    pub fn stop(&mut self) {
        self.vm.icount_limit = 0;

        if self.executing_thread == std::thread::current().id() {
            *self.stop.borrow_mut() = true;
        }
    }

    pub fn add_block_hook(&mut self, callback: Box<dyn Fn(u64, u64)>) -> u32 {
        let hook_id = self.execution_hooks.borrow_mut().add_block_hook(callback);
        return qualify_hook_id(hook_id, HookType::Block);
    }

    pub fn add_violation_hook(&mut self, callback: Box<dyn Fn(u64, u8, bool) -> bool>) -> u32 {
        let hook_id = self.violation_hooks.add_hook(callback);
        return qualify_hook_id(hook_id, HookType::Violation);
    }

    pub fn add_execution_hook(&mut self, address: u64, callback: Box<dyn Fn(u64)>) -> u32 {
        let hook_id = self
            .execution_hooks
            .borrow_mut()
            .add_specific_hook(address, callback);
        return qualify_hook_id(hook_id, HookType::ExecuteSpecific);
    }

    pub fn add_generic_execution_hook(&mut self, callback: Box<dyn Fn(u64)>) -> u32 {
        let hook_id = self.execution_hooks.borrow_mut().add_generic_hook(callback);
        return qualify_hook_id(hook_id, HookType::ExecuteGeneric);
    }

    pub fn add_syscall_hook(&mut self, callback: Box<dyn Fn()>) -> u32 {
        let hook_id = self.syscall_hooks.add_hook(callback);
        return qualify_hook_id(hook_id, HookType::Syscall);
    }

    pub fn add_interrupt_hook(&mut self, callback: Box<dyn Fn(i32)>) -> u32 {
        let hook_id = self.interrupt_hooks.add_hook(callback);
        return qualify_hook_id(hook_id, HookType::Interrupt);
    }

    pub fn add_read_hook(
        &mut self,
        start: u64,
        end: u64,
        callback: Box<dyn Fn(u64, &[u8])>,
    ) -> u32 {
        let id = self
            .get_mem()
            .add_read_after_hook(start, end, Box::new(MemoryHook { callback }));
        if id.is_none() {
            return 0;
        }

        return qualify_hook_id(id.unwrap(), HookType::Read);
    }

    pub fn add_write_hook(
        &mut self,
        start: u64,
        end: u64,
        callback: Box<dyn Fn(u64, &[u8])>,
    ) -> u32 {
        let id = self
            .get_mem()
            .add_write_hook(start, end, Box::new(MemoryHook { callback }));
        if id.is_none() {
            return 0;
        }

        return qualify_hook_id(id.unwrap(), HookType::Write);
    }

    pub fn remove_hook(&mut self, id: u32) {
        let (hook_id, hook_type) = split_hook_id(id);

        match hook_type {
            HookType::Syscall => self.syscall_hooks.remove_hook(hook_id),
            HookType::Violation => self.violation_hooks.remove_hook(hook_id),
            HookType::Interrupt => self.interrupt_hooks.remove_hook(hook_id),
            HookType::ExecuteGeneric => self
                .execution_hooks
                .borrow_mut()
                .remove_generic_hook(hook_id),
            HookType::ExecuteSpecific => self
                .execution_hooks
                .borrow_mut()
                .remove_specific_hook(hook_id),
            HookType::Block => self.execution_hooks.borrow_mut().remove_block_hook(hook_id),
            HookType::Read => {
                self.get_mem().remove_read_after_hook(hook_id);
                ()
            }
            HookType::Write => {
                self.get_mem().remove_write_hook(hook_id);
                ()
            }
            _ => {}
        }
    }

    pub fn run_on_next_instruction(&mut self, callback: Box<dyn Fn()>) {
        self.execution_hooks.borrow_mut().schedule(callback);
    }

    pub fn map_memory(&mut self, address: u64, length: u64, permissions: u8) -> bool {
        const MAPPING_PERMISSIONS: u8 = icicle_vm::cpu::mem::perm::MAP
            | icicle_vm::cpu::mem::perm::INIT
            | icicle_vm::cpu::mem::perm::IN_CODE_CACHE;

        let native_permissions = map_permissions(permissions);

        let mapping = icicle_vm::cpu::mem::Mapping {
            perm: native_permissions | MAPPING_PERMISSIONS,
            value: 0x0,
        };

        return self.get_mem().map_memory_len(address, length, mapping);
    }

    pub fn map_mmio(
        &mut self,
        address: u64,
        length: u64,
        read_function: Box<dyn Fn(u64, &mut [u8])>,
        write_function: Box<dyn Fn(u64, &[u8])>,
    ) -> bool {
        let mem = self.get_mem();

        let handler = MmioHandler::new(read_function, write_function);
        let handler_id = mem.register_io_handler(handler);

        return self.get_mem().map_memory_len(address, length, handler_id);
    }

    pub fn unmap_memory(&mut self, address: u64, length: u64) -> bool {
        return self.get_mem().unmap_memory_len(address, length);
    }

    pub fn protect_memory(&mut self, address: u64, length: u64, permissions: u8) -> bool {
        let native_permissions = map_permissions(permissions);
        let res = self
            .get_mem()
            .update_perm(address, length, native_permissions);
        return res.is_ok();
    }

    pub fn write_memory(&mut self, address: u64, data: &[u8]) -> bool {
        let res = self
            .get_mem()
            .write_bytes(address, data, icicle_vm::cpu::mem::perm::NONE);
        return res.is_ok();
    }

    pub fn read_memory(&mut self, address: u64, data: &mut [u8]) -> bool {
        let res = self
            .get_mem()
            .read_bytes(address, data, icicle_vm::cpu::mem::perm::NONE);
        return res.is_ok();
    }

    pub fn save_registers(&self) -> Vec<u8> {
        const REG_SIZE: usize = std::mem::size_of::<icicle_cpu::Regs>();
        unsafe {
            let data: [u8; REG_SIZE] = self.vm.cpu.regs.read_at(0);
            return data.to_vec();
        }
    }

    pub fn restore_registers(&mut self, data: &[u8]) {
        const REG_SIZE: usize = std::mem::size_of::<icicle_cpu::Regs>();

        let mut buffer: [u8; REG_SIZE] = [0; REG_SIZE];
        let size = std::cmp::min(REG_SIZE, data.len());
        buffer.copy_from_slice(&data[..size]);

        unsafe {
            self.vm.cpu.regs.write_at(0, buffer);
        };
    }

    fn read_generic_register(&mut self, reg: registers::X86Register, buffer: &mut [u8]) -> usize {
        let reg_node = self.reg.get_node(reg);

        let res = self.vm.cpu.read_dynamic(pcode::Value::Var(reg_node));
        let bytes: [u8; 32] = res.zxt();

        let len = std::cmp::min(bytes.len(), buffer.len());
        buffer[..len].copy_from_slice(&bytes[..len]);

        return reg_node.size.into();
    }

    fn read_flags<T>(&mut self, data: &mut [u8]) -> usize {
        const REAL_SIZE: usize = std::mem::size_of::<u64>();
        let limit: usize = std::mem::size_of::<T>();
        let size = std::cmp::min(REAL_SIZE, limit);

        let flags: u64 = self.reg.get_flags(&mut self.vm.cpu);

        let copy_size = std::cmp::min(data.len(), size);
        data[..copy_size].copy_from_slice(&flags.to_ne_bytes()[..copy_size]);

        return limit;
    }

    pub fn read_register(&mut self, reg: registers::X86Register, data: &mut [u8]) -> usize {
        match reg {
            registers::X86Register::Rflags => self.read_flags::<u64>(data),
            registers::X86Register::Eflags => self.read_flags::<u32>(data),
            registers::X86Register::Flags => self.read_flags::<u16>(data),
            _ => self.read_generic_register(reg, data),
        }
    }

    pub fn create_snapshot(&mut self) -> u32 {
        let snap = self.vm.snapshot();

        let id = self.snapshots.len() as u32;
        self.snapshots.push(Box::new(snap));

        return id;
    }

    pub fn restore_snapshot(&mut self, id: u32) {
        let snap = self.snapshots[id as usize].as_ref();
        self.vm.restore(&snap);
    }

    fn write_flags<T>(&mut self, data: &[u8]) -> usize {
        const REAL_SIZE: usize = std::mem::size_of::<u64>();
        let limit: usize = std::mem::size_of::<T>();
        let size = std::cmp::min(REAL_SIZE, limit);
        let copy_size = std::cmp::min(data.len(), size);

        let mut buffer = [0u8; REAL_SIZE];
        self.read_flags::<u64>(&mut buffer);

        buffer[..copy_size].copy_from_slice(&data[..copy_size]);

        let flags = u64::from_ne_bytes(buffer);
        self.reg.set_flags(&mut self.vm.cpu, flags);

        return limit;
    }

    pub fn write_register(&mut self, reg: registers::X86Register, data: &[u8]) -> usize {
        match reg {
            registers::X86Register::Rflags => self.write_flags::<u64>(data),
            registers::X86Register::Eflags => self.write_flags::<u32>(data),
            registers::X86Register::Flags => self.write_flags::<u16>(data),
            _ => self.write_generic_register(reg, data),
        }
    }

    fn write_generic_register(&mut self, reg: registers::X86Register, data: &[u8]) -> usize {
        let reg_node = self.reg.get_node(reg);

        let mut buffer = [0u8; 32];
        let len = std::cmp::min(data.len(), buffer.len());
        buffer[..len].copy_from_slice(&data[..len]);

        //let value = icicle_cpu::regs::DynamicValue::new(buffer, reg_node.size.into());
        //self.vm.cpu.write_trunc(reg_node, value);

        let cpu = &mut self.vm.cpu;

        match reg_node.size {
            1 => cpu.write_var::<[u8; 1]>(reg_node, buffer[..1].try_into().unwrap()),
            2 => cpu.write_var::<[u8; 2]>(reg_node, buffer[..2].try_into().unwrap()),
            3 => cpu.write_var::<[u8; 3]>(reg_node, buffer[..3].try_into().unwrap()),
            4 => cpu.write_var::<[u8; 4]>(reg_node, buffer[..4].try_into().unwrap()),
            5 => cpu.write_var::<[u8; 5]>(reg_node, buffer[..5].try_into().unwrap()),
            6 => cpu.write_var::<[u8; 6]>(reg_node, buffer[..6].try_into().unwrap()),
            7 => cpu.write_var::<[u8; 7]>(reg_node, buffer[..7].try_into().unwrap()),
            8 => cpu.write_var::<[u8; 8]>(reg_node, buffer[..8].try_into().unwrap()),
            9 => cpu.write_var::<[u8; 9]>(reg_node, buffer[..9].try_into().unwrap()),
            10 => cpu.write_var::<[u8; 10]>(reg_node, buffer[..10].try_into().unwrap()),
            11 => cpu.write_var::<[u8; 11]>(reg_node, buffer[..11].try_into().unwrap()),
            12 => cpu.write_var::<[u8; 12]>(reg_node, buffer[..12].try_into().unwrap()),
            13 => cpu.write_var::<[u8; 13]>(reg_node, buffer[..13].try_into().unwrap()),
            14 => cpu.write_var::<[u8; 14]>(reg_node, buffer[..14].try_into().unwrap()),
            15 => cpu.write_var::<[u8; 15]>(reg_node, buffer[..15].try_into().unwrap()),
            16 => cpu.write_var::<[u8; 16]>(reg_node, buffer[..16].try_into().unwrap()),
            _ => panic!("invalid dynamic value size"),
        }

        return reg_node.size.into();
    }
}
