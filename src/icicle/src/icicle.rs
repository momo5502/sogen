fn create_x64_vm() -> icicle_vm::Vm {
    let cpu_config = icicle_vm::cpu::Config::from_target_triple("x86_64-none");
    let vm = icicle_vm::build(&cpu_config).unwrap();
    return vm;
}

fn map_permissions(foreign_permissions: u8) -> u8 {
    const FOREIGN_READ: u8 = 1 << 0;
    const FOREIGN_WRITE: u8 = 1 << 1;
    const FOREIGN_EXEC: u8 = 1 << 2;

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

pub struct IcicleEmulator {
    vm: icicle_vm::Vm,
}

impl IcicleEmulator {
    pub fn new() -> Self {
        Self {
            vm: create_x64_vm(),
        }
    }

    fn get_mem(&mut self) -> &mut icicle_vm::cpu::Mmu {
        return &mut self.vm.cpu.mem;
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

        let layout = icicle_vm::cpu::mem::AllocLayout {
            addr: Some(address),
            size: length,
            align: 0x1000,
        };

        let res = self.get_mem().alloc_memory(layout, mapping);
        return res.is_ok();
    }

    pub fn unmap_memory(&mut self, address: u64, length: u64) -> bool {
        return self.get_mem().unmap_memory_len(address, length);
    }

    pub fn protect_memory(&mut self, address: u64, length: u64, permissions: u8) -> bool {
        let native_permissions = map_permissions(permissions);
        let res = self.get_mem().update_perm(address, length, native_permissions);
        return res.is_ok();
    }

    pub fn write_memory(&mut self, address: u64, data: &[u8]) -> bool {
        let res = self.get_mem().write_bytes(address, data, icicle_vm::cpu::mem::perm::WRITE);
        return res.is_ok();
    }

    pub fn read_memory(&mut self, address: u64, data: &mut [u8]) -> bool {
        let res = self.get_mem().read_bytes(address, data, icicle_vm::cpu::mem::perm::READ);
        return res.is_ok();
    }
}
