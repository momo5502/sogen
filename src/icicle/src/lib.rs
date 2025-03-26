#[unsafe(no_mangle)]
pub fn test_rust() {
 // Setup the CPU state for the target triple
 let cpu_config = icicle_vm::cpu::Config::from_target_triple("x86_64-none");
 let mut vm = icicle_vm::build(&cpu_config).unwrap();

 // Setup an environment to run inside of.
 //let mut env = icicle_vm::env::build_auto(&mut vm).unwrap();
 // Load a binary into the environment.
 //env.load(&mut vm.cpu, b"./test.elf").unwrap();
 //vm.env = env;

 let mapping = icicle_vm::cpu::mem::Mapping { perm: icicle_vm::cpu::mem::perm::ALL, value: 0x0 };

 let alloc1 =
     vm.cpu.mem.alloc_memory(icicle_vm::cpu::mem::AllocLayout { addr: Some(0x10000), size: 0x100, align: 0x100 }, mapping).unwrap();

 // Add instrumentation
 let counter = vm.cpu.trace.register_store(vec![0_u64]);
 vm.add_injector(BlockCounter { counter });

 // Run until the VM exits.
 let exit = vm.run();
 println!("{exit:?}\n{}", icicle_vm::debug::current_disasm(&mut vm));


 // Read instrumentation data.
 let blocks_hit = vm.cpu.trace[counter].as_any().downcast_ref::<Vec<u64>>().unwrap()[0];
 let blocks_executed = blocks_hit.saturating_sub(1);
 println!("{blocks_executed} blocks were executed");
}

struct BlockCounter {
 counter: icicle_vm::cpu::StoreRef,
}

impl icicle_vm::CodeInjector for BlockCounter {
 fn inject(
     &mut self,
     _cpu: &mut icicle_vm::cpu::Cpu,
     group: &icicle_vm::cpu::BlockGroup,
     code: &mut icicle_vm::BlockTable,
 ) {
     let store_id = self.counter.get_store_id();
     for block in &mut code.blocks[group.range()] {
         // counter += 1
         let counter = block.pcode.alloc_tmp(8);
         let instrumentation = [
             (counter, pcode::Op::Load(store_id), 0_u64).into(),
             (counter, pcode::Op::IntAdd, (counter, 1_u64)).into(),
             (pcode::Op::Store(store_id), (0_u64, counter)).into(),
         ];

         // Inject the instrumentation at the start of the block.
         block.pcode.instructions.splice(..0, instrumentation);
     }
 }
}