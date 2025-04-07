mod icicle;
mod registers;

use icicle::IcicleEmulator;
use registers::X64Register;
use std::os::raw::c_void;

fn to_cbool(value: bool) -> i32 {
    if value {
        return 1;
    }

    return 0;
}

#[unsafe(no_mangle)]
pub fn icicle_create_emulator() -> *mut c_void {
    let emulator = Box::new(IcicleEmulator::new());
    return Box::into_raw(emulator) as *mut c_void;
}

#[unsafe(no_mangle)]
pub fn icicle_start(ptr: *mut c_void, count: usize) {
    unsafe {
        let emulator = &mut *(ptr as *mut IcicleEmulator);
        emulator.start(count as u64);
    }
}

#[unsafe(no_mangle)]
pub fn icicle_stop(ptr: *mut c_void) {
    unsafe {
        let emulator = &mut *(ptr as *mut IcicleEmulator);
        emulator.stop();
    }
}

type RawFunction = extern "C" fn(*mut c_void);
type PtrFunction = extern "C" fn(*mut c_void, u64);
type DataFunction = extern "C" fn(*mut c_void, *const c_void, usize);
type MmioReadFunction = extern "C" fn(*mut c_void, u64, *mut c_void, usize);
type MmioWriteFunction = extern "C" fn(*mut c_void, u64, *const c_void, usize);
type ViolationFunction = extern "C" fn(*mut c_void, u64, u8, i32) -> i32;
type InterruptFunction = extern "C" fn(*mut c_void, i32);
type MemoryAccessFunction = MmioWriteFunction;

#[unsafe(no_mangle)]
pub fn icicle_map_mmio(
    ptr: *mut c_void,
    address: u64,
    length: u64,
    read_cb: MmioReadFunction,
    read_data: *mut c_void,
    write_cb: MmioWriteFunction,
    write_data: *mut c_void,
) -> i32 {
    unsafe {
        let emulator = &mut *(ptr as *mut IcicleEmulator);

        let read_wrapper = Box::new(move |addr: u64, data: &mut [u8]| {
            let raw_pointer: *mut u8 = data.as_mut_ptr();
            read_cb(read_data, addr, raw_pointer as *mut c_void, data.len());
        });

        let write_wrapper = Box::new(move |addr: u64, data: &[u8]| {
            let raw_pointer: *const u8 = data.as_ptr();
            write_cb(write_data, addr, raw_pointer as *const c_void, data.len());
        });

        let res = emulator.map_mmio(address, length, read_wrapper, write_wrapper);
        return to_cbool(res);
    }
}

#[unsafe(no_mangle)]
pub fn icicle_map_memory(ptr: *mut c_void, address: u64, length: u64, permissions: u8) -> i32 {
    unsafe {
        let emulator = &mut *(ptr as *mut IcicleEmulator);
        let res = emulator.map_memory(address, length, permissions);
        return to_cbool(res);
    }
}

#[unsafe(no_mangle)]
pub fn icicle_unmap_memory(ptr: *mut c_void, address: u64, length: u64) -> i32 {
    unsafe {
        let emulator = &mut *(ptr as *mut IcicleEmulator);
        let res = emulator.unmap_memory(address, length);
        return to_cbool(res);
    }
}

#[unsafe(no_mangle)]
pub fn icicle_protect_memory(ptr: *mut c_void, address: u64, length: u64, permissions: u8) -> i32 {
    unsafe {
        let emulator = &mut *(ptr as *mut IcicleEmulator);
        let res = emulator.protect_memory(address, length, permissions);
        return to_cbool(res);
    }
}

#[unsafe(no_mangle)]
pub fn icicle_write_memory(
    ptr: *mut c_void,
    address: u64,
    data: *const c_void,
    size: usize,
) -> i32 {
    if size == 0 {
        return 1;
    }

    if data.is_null() {
        return 0;
    }

    unsafe {
        let emulator = &mut *(ptr as *mut IcicleEmulator);
        let u8_slice = std::slice::from_raw_parts(data as *const u8, size);
        let res = emulator.write_memory(address, u8_slice);
        return to_cbool(res);
    }
}

#[unsafe(no_mangle)]
pub fn icicle_save_registers(ptr: *mut c_void, accessor: DataFunction, accessor_data: *mut c_void) {
    unsafe {
        let emulator = &mut *(ptr as *mut IcicleEmulator);
        let registers = emulator.save_registers();
        accessor(
            accessor_data,
            registers.as_ptr() as *const c_void,
            registers.len(),
        );
    }
}

#[unsafe(no_mangle)]
pub fn icicle_restore_registers(ptr: *mut c_void, data: *const c_void, size: usize) {
    if size == 0 || data.is_null() {
        return;
    }

    unsafe {
        let emulator = &mut *(ptr as *mut IcicleEmulator);
        let u8_slice = std::slice::from_raw_parts(data as *const u8, size);
        emulator.restore_registers(u8_slice);
    }
}

#[unsafe(no_mangle)]
pub fn icicle_read_memory(ptr: *mut c_void, address: u64, data: *mut c_void, size: usize) -> i32 {
    if size == 0 {
        return 1;
    }

    if data.is_null() {
        return 0;
    }

    unsafe {
        let emulator = &mut *(ptr as *mut IcicleEmulator);
        let u8_slice = std::slice::from_raw_parts_mut(data as *mut u8, size);
        let res = emulator.read_memory(address, u8_slice);
        return to_cbool(res);
    }
}

#[unsafe(no_mangle)]
pub fn icicle_add_interrupt_hook(ptr: *mut c_void, callback: InterruptFunction, data: *mut c_void) -> u32 {
    unsafe {
        let emulator = &mut *(ptr as *mut IcicleEmulator);
        return emulator.add_interrupt_hook(Box::new(
            move |code: i32| callback(data, code),
        ));
    }
}

#[unsafe(no_mangle)]
pub fn icicle_add_violation_hook(ptr: *mut c_void, callback: ViolationFunction, data: *mut c_void) -> u32 {
    unsafe {
        let emulator = &mut *(ptr as *mut IcicleEmulator);
        return emulator.add_violation_hook(Box::new(
            move |address: u64, permission: u8, unmapped: bool| {
                let result = callback(data, address, permission, to_cbool(unmapped));
                if result == 0 {
                    return false;
                }

                return true;
            },
        ));
    }
}

#[unsafe(no_mangle)]
pub fn icicle_add_read_hook(ptr: *mut c_void, start: u64, end: u64, callback: MemoryAccessFunction, user: *mut c_void) -> u32 {
    unsafe {
        let emulator = &mut *(ptr as *mut IcicleEmulator);
        return emulator.add_read_hook(start, end, Box::new(
            move |address: u64, data: &[u8]| {
                callback(user, address, data.as_ptr() as *const c_void, data.len());
            },
        ));
    }
}

#[unsafe(no_mangle)]
pub fn icicle_add_write_hook(ptr: *mut c_void, start: u64, end: u64, callback: MemoryAccessFunction, user: *mut c_void) -> u32 {
    unsafe {
        let emulator = &mut *(ptr as *mut IcicleEmulator);
        return emulator.add_write_hook(start, end, Box::new(
            move |address: u64, data: &[u8]| {
                callback(user, address, data.as_ptr() as *const c_void, data.len());
            },
        ));
    }
}

#[unsafe(no_mangle)]
pub fn icicle_add_syscall_hook(ptr: *mut c_void, callback: RawFunction, data: *mut c_void) -> u32 {
    unsafe {
        let emulator = &mut *(ptr as *mut IcicleEmulator);
        return emulator.add_syscall_hook(Box::new(move || callback(data)));
    }
}

#[unsafe(no_mangle)]
pub fn icicle_add_generic_execution_hook(ptr: *mut c_void, callback: PtrFunction, data: *mut c_void) -> u32 {
    unsafe {
        let emulator = &mut *(ptr as *mut IcicleEmulator);
        return emulator.add_generic_execution_hook(Box::new(move |ptr: u64| callback(data, ptr)));
    }
}

#[unsafe(no_mangle)]
pub fn icicle_add_execution_hook(ptr: *mut c_void, address: u64, callback: PtrFunction, data: *mut c_void) -> u32 {
    unsafe {
        let emulator = &mut *(ptr as *mut IcicleEmulator);
        return emulator.add_execution_hook(address, Box::new(move |ptr: u64| callback(data, ptr)));
    }
}

#[unsafe(no_mangle)]
pub fn icicle_remove_hook(ptr: *mut c_void, id: u32) {
    unsafe {
        let emulator = &mut *(ptr as *mut IcicleEmulator);
        emulator.remove_hook(id);
    }
}

#[unsafe(no_mangle)]
pub fn icicle_read_register(
    ptr: *mut c_void,
    reg: X64Register,
    data: *mut c_void,
    size: usize,
) -> usize {
    if size == 0 {
        return 1;
    }

    if data.is_null() {
        return 0;
    }

    unsafe {
        let emulator = &mut *(ptr as *mut IcicleEmulator);
        let u8_slice = std::slice::from_raw_parts_mut(data as *mut u8, size);
        return emulator.read_register(reg, u8_slice);
    }
}

#[unsafe(no_mangle)]
pub fn icicle_write_register(
    ptr: *mut c_void,
    reg: X64Register,
    data: *const c_void,
    size: usize,
) -> usize {
    if size == 0 {
        return 1;
    }

    if data.is_null() {
        return 0;
    }

    unsafe {
        let emulator = &mut *(ptr as *mut IcicleEmulator);
        let u8_slice = std::slice::from_raw_parts(data as *const u8, size);
        return emulator.write_register(reg, u8_slice);
    }
}

#[unsafe(no_mangle)]
pub fn icicle_destroy_emulator(ptr: *mut c_void) {
    if ptr.is_null() {
        return;
    }

    unsafe {
        let _ = Box::from_raw(ptr as *mut IcicleEmulator);
    }
}
