mod icicle;

use icicle::IcicleEmulator;
use std::os::raw::c_void;

#[unsafe(no_mangle)]
pub fn icicle_create_emulator() -> *mut c_void {
    let emulator = Box::new(IcicleEmulator::new());
    return Box::into_raw(emulator) as *mut c_void;
}

#[unsafe(no_mangle)]
pub fn icicle_map_memory(
    ptr: *mut c_void,
    address: u64,
    length: u64,
    permissions: u8,
) -> i32 {
    unsafe {
        let emulator = &mut *(ptr as *mut IcicleEmulator);
        let res = emulator.map_memory(address, length, permissions);

        if res {
            return 1;
        }

        return 0;
    }
}

#[unsafe(no_mangle)]
pub fn icicle_unmap_memory(ptr: *mut c_void, address: u64, length: u64) -> i32 {
    unsafe {
        let emulator = &mut *(ptr as *mut IcicleEmulator);
        let res = emulator.unmap_memory(address, length);

        if res {
            return 1;
        }

        return 0;
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
