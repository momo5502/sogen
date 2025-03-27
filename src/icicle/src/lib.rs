mod icicle;

use icicle::IcicleEmulator;
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
pub fn icicle_write_memory(ptr: *mut c_void, address: u64, data: *const c_void, size: usize) -> i32 {
    unsafe {
        let emulator = &mut *(ptr as *mut IcicleEmulator);
        let u8_ptr = data as *const u8;
        let u8_slice = std::slice::from_raw_parts(u8_ptr, size);
        let res = emulator.write_memory(address, u8_slice);
        return to_cbool(res);
    }
}

#[unsafe(no_mangle)]
pub fn icicle_read_memory(ptr: *mut c_void, address: u64, data: *mut c_void, size: usize) -> i32 {
    unsafe {
        let emulator = &mut *(ptr as *mut IcicleEmulator);
        let u8_ptr = data as *mut u8;
        let u8_slice = std::slice::from_raw_parts_mut(u8_ptr, size);
        let res = emulator.read_memory(address, u8_slice);
        return to_cbool(res);
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
