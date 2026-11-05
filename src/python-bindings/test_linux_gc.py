import ctypes
import gc
import os
from pathlib import Path
import unittest
import weakref

import sogen


VIEWS = ("emulator", "process", "memory", "callbacks", "hooks", "symbols", "debug", "thread", "module", "section")
HOOKS = (
    ("memory_execution", ()), ("memory_execution_at", (0x100000,)),
    ("memory_read", (0x100000, 1)), ("memory_write", (0x100000, 1)),
    ("instruction", (sogen.Instruction.cpuid,)), ("interrupt", ()),
    ("basic_block", ()), ("memory_violation", ()),
)


def create(application=False):
    backend = getattr(sogen.Backend, os.getenv("LINUX_PYTHON_TEST_BACKEND", "unicorn"))
    if application:
        return sogen.linux.create_application(
            os.getenv("LINUX_PYTHON_TEST_BINARY", "/bin/true"),
            emulation_root=os.getenv("LINUX_PYTHON_TEST_ROOT", "/"),
            backend=backend,
            disable_logging=True,
        )
    return sogen.linux.create_empty(emulation_root="/", backend=backend)


def view(app, name):
    if name == "emulator":
        return app
    if name == "symbols":
        return app.hooks.symbols
    if name == "thread":
        return app.current_thread
    if name in ("module", "section"):
        module = app.find_module_by_name(Path(os.getenv("LINUX_PYTHON_TEST_BINARY", "/bin/true")).name)
        return module if name == "module" else module.sections[0]
    return getattr(app, name)


def install_callback(name, hook=False, retain=False):
    app = create(name in ("thread", "module", "section"))
    target = view(app, name)

    def callback(*args):
        return target

    witness = weakref.ref(callback)
    if hook:
        app.hooks.memory_execution(callback)
    else:
        app.callbacks.on_stdout = callback
    return witness, target if retain else None


class LinuxGarbageCollectionTest(unittest.TestCase):
    def test_event_callback_cycles_are_collected(self):
        for name in VIEWS:
            with self.subTest(view=name):
                witness, _ = install_callback(name)
                gc.collect()
                self.assertIsNone(witness())

    def test_hook_callback_cycles_are_collected(self):
        for name in VIEWS:
            with self.subTest(view=name):
                witness, _ = install_callback(name, hook=True)
                gc.collect()
                self.assertIsNone(witness())

    def test_live_views_keep_their_emulator_alive(self):
        witness, memory = install_callback("memory", retain=True)
        gc.collect()
        self.assertIsNotNone(witness())
        address = memory.allocate_memory(0x1000, sogen.MemoryPermission.read_write)
        memory.write_memory(address, b"alive")
        self.assertEqual(memory.read_memory(address, 5), b"alive")
        memory = None
        gc.collect()
        self.assertIsNone(witness())

    def test_live_thread_and_module_views_keep_their_emulator_alive(self):
        for name in ("thread", "module"):
            with self.subTest(view=name):
                witness, target = install_callback(name, retain=True)
                gc.collect()
                self.assertIsNotNone(witness())
                if name == "thread":
                    self.assertIsInstance(target.current_ip, int)
                else:
                    self.assertTrue(target.name)
                    self.assertTrue(target.sections)
                target = None
                gc.collect()
                self.assertIsNone(witness())

    def test_callback_still_runs_while_a_view_is_held(self):
        events = []

        def install():
            app = create()
            memory = app.memory

            def callback(address, size, permissions, committed):
                self.assertIsNotNone(memory.get_region_info(address))
                events.append(address)

            app.callbacks.on_memory_allocate = callback
            return weakref.ref(callback), memory

        witness, memory = install()
        gc.collect()
        self.assertIsNotNone(witness())
        address = memory.allocate_memory(0x1000, sogen.MemoryPermission.read_write)
        self.assertEqual(events, [address])
        memory = None
        gc.collect()
        self.assertIsNone(witness())

    def test_all_event_callback_slots_are_collected(self):
        names = (
            "stdout", "stderr", "syscall", "memory_violate", "signal", "exception",
            "memory_allocate", "memory_protect", "memory_release", "module_load",
            "thread_create", "thread_terminated", "thread_switch",
        )

        def install(name):
            app = create()

            def callback(*args):
                return app

            app.callbacks.set(name, callback)
            return weakref.ref(callback)

        for name in names:
            with self.subTest(callback=name):
                witness = install(name)
                gc.collect()
                self.assertIsNone(witness())

    def test_all_hook_kinds_are_collected(self):
        def install(name, args):
            app = create()

            def callback(*args):
                return app

            getattr(app.hooks, name)(*args, callback)
            return weakref.ref(callback)

        for name, args in HOOKS:
            with self.subTest(hook=name):
                witness = install(name, args)
                gc.collect()
                self.assertIsNone(witness())

    def test_live_hook_handle_keeps_its_emulator_alive(self):
        def install():
            app = create()

            def callback(*args):
                return app

            return weakref.ref(callback), app.hooks.memory_execution(callback)

        witness, hook = install()
        gc.collect()
        self.assertIsNotNone(witness())
        self.assertTrue(hook.active)
        hook.remove()
        self.assertFalse(hook.active)
        self.assertIsNone(witness())

    def test_callback_capturing_its_hook_is_collected(self):
        def install(name, args):
            app = create()
            state = {}

            def callback(*args):
                return state

            state["hook"] = getattr(app.hooks, name)(*args, callback)
            return weakref.ref(callback)

        for name, args in HOOKS:
            with self.subTest(hook=name):
                witness = install(name, args)
                gc.collect()
                self.assertIsNone(witness())

    def test_view_identity_is_preserved(self):
        app = create()
        for name in ("memory", "callbacks", "hooks", "symbols", "debug"):
            with self.subTest(view=name):
                self.assertIs(view(app, name), view(app, name))

    def test_module_callback_snapshots_outlive_state_restore_and_emulator(self):
        def metadata(module):
            return (
                module.name, str(module.path), module.image_base, module.entry_point,
                [(section.name, section.start, section.length) for section in module.sections],
            )

        def capture(origin, restore):
            app = create(application=True)
            modules = []
            app.callbacks.on_module_load = modules.append
            if origin == "runtime":
                modules.clear()
                app.start()
            self.assertTrue(modules)
            retained = list(modules)
            expected = [metadata(module) for module in retained]
            if restore == "snapshot":
                app.save_snapshot()
                app.restore_snapshot()
            else:
                app.deserialize_state(app.serialize_state())

            def callback(*args):
                return app

            app.callbacks.on_stdout = callback
            return weakref.ref(callback), retained, expected

        for origin in ("replay", "runtime"):
            for restore in ("snapshot", "deserialize"):
                with self.subTest(origin=origin, restore=restore):
                    witness, modules, expected = capture(origin, restore)
                    gc.collect()
                    self.assertIsNone(witness())
                    self.assertEqual([metadata(module) for module in modules], expected)

    def test_symbol_callback_cycle_is_collected(self):
        def install():
            app = create()

            @sogen.linux.symbol_call(params=[], restype=None)
            def callback(call, params):
                return app

            app.hooks.symbols["target"] = callback
            return weakref.ref(callback)

        witness = install()
        gc.collect()
        self.assertIsNone(witness())

    def test_symbol_signature_references_are_collected(self):
        def install():
            app = create()

            class Parameter(ctypes.c_uint64):
                pass

            Parameter.owner = app

            @sogen.linux.symbol_call(params=[Parameter], restype=Parameter)
            def callback(call, params):
                return sogen.ApiContinuation.run_original

            app.hooks.symbols["target"] = callback
            return weakref.ref(callback)

        witness = install()
        gc.collect()
        self.assertIsNone(witness())

    def test_uninitialized_instances_can_be_traversed(self):
        for name in ("Emulator", "Thread", "ProcessContext", "MemoryManager", "Callbacks", "Hooks", "SymbolHooks", "Debug"):
            with self.subTest(type=name):
                cls = getattr(sogen.linux, name)
                instance = cls.__new__(cls)
                gc.get_referents(instance)

    def test_resurrected_view_remains_usable(self):
        survivors = []

        def install():
            app = create()

            class Callback:
                def __init__(self):
                    self.memory = app.memory

                def __call__(self, *args):
                    pass

                def __del__(self):
                    survivors.append(self.memory)

            app.callbacks.on_stdout = Callback()

        install()
        gc.collect()
        self.assertEqual(len(survivors), 1)
        memory = survivors.pop()
        address = memory.allocate_memory(0x1000, sogen.MemoryPermission.read_write)
        memory.write_memory(address, b"resurrected")
        self.assertEqual(memory.read_memory(address, 11), b"resurrected")
        memory = None
        gc.collect()
        self.assertEqual(survivors, [])


if __name__ == "__main__":
    unittest.main()
