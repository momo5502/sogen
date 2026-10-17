"""ARM64 binding tests. The Windows application test needs EMULATOR_ARM64_ROOT; the others need no root."""

import ctypes
import os
import platform
import struct
import unittest

import sogen

# Windows on ARM builds only the WHP ARM64 backend; other ARM64 hosts (macOS, Linux) keep Unicorn and x86-64 guests.
WINDOWS_ARM64_HOST = platform.system() == "Windows" and platform.machine().upper() == "ARM64"
BACKEND_NAMES = {sogen.Backend.whp: "whp-arm64", sogen.Backend.hvf: "hvf-arm64", sogen.Backend.unicorn: "Unicorn Engine ARM64"}
EXPECTED_BACKEND = BACKEND_NAMES[sogen.arm64.default_backend()]


class Arm64BindingsTest(unittest.TestCase):
    def create_cpu(self, instructions):
        cpu = sogen.arm64.Emulator()
        cpu.map_memory(0x10000, 0x1000, sogen.MemoryPermission.all)
        cpu.write_memory(0x10000, struct.pack(f"<{len(instructions)}I", *instructions))
        cpu.write_register(sogen.arm64.Register.pc, 0x10000)
        return cpu

    def test_single_step_and_saved_registers(self):
        cpu = self.create_cpu([0xD2800540, 0x91000400])
        reg = sogen.arm64.Register
        self.assertEqual(cpu.backend_name, EXPECTED_BACKEND)
        self.assertTrue(cpu.supports_single_step)
        cpu.step()
        self.assertEqual(cpu.read_register(reg.pc), 0x10004)
        self.assertEqual(cpu.read_register(reg.x0), 42)
        saved = cpu.save_registers()
        cpu.start(1)
        self.assertEqual(cpu.read_register(reg.x0), 43)
        cpu.restore_registers(saved)
        self.assertEqual(cpu.read_register(reg.x0), 42)
        self.assertEqual(cpu.read_register(reg.pc), 0x10004)

    def test_python_svc_callback_returns_a_result(self):
        cpu = self.create_cpu([0xD40000E1, 0x91000400])
        reg = sogen.arm64.Register
        calls = []

        def handle(machine, identifier):
            self.assertIs(machine, cpu)
            self.assertEqual(machine.read_register(reg.pc), 0x10000)
            calls.append(identifier)
            machine.write_register(reg.x0, 99)
            return sogen.HookContinuation.skip

        cpu.on_svc = handle
        cpu.step()
        self.assertEqual(calls, [7])
        self.assertEqual(cpu.read_register(reg.x0), 99)
        cpu.step()
        self.assertEqual(cpu.read_register(reg.x0), 100)
        cpu.on_svc = None

    def test_python_callback_exception_propagates_and_can_retry(self):
        cpu = self.create_cpu([0xD4000021])

        def fail(machine, identifier):
            raise ValueError("intentional Python callback error")

        cpu.on_svc = fail
        with self.assertRaisesRegex(ValueError, "intentional Python callback error"):
            cpu.step()
        cpu.on_svc = lambda machine, identifier: sogen.HookContinuation.skip
        cpu.step()
        self.assertEqual(cpu.read_register(sogen.arm64.Register.pc), 0x10004)
        cpu.on_svc = None

    def test_invalid_arguments_and_unmapped_memory_fail(self):
        cpu = self.create_cpu([0xD4000021])
        with self.assertRaises(ValueError):
            cpu.on_svc = 123
        with self.assertRaisesRegex(RuntimeError, "on_svc"):
            cpu.step()
        with self.assertRaises(RuntimeError):
            cpu.read_memory(0x90000, 8)
        with self.assertRaises(ValueError):
            cpu.restore_registers(b"invalid")
        with self.assertRaises(ValueError):
            cpu.restore_registers(b"")
        self.assertTrue(cpu.unmap_memory(0x10000, 0x1000))

    def test_existing_namespaces_remain_but_unavailable_personalities_fail(self):
        self.assertIs(sogen.create_application, sogen.windows.create_application)
        self.assertTrue(hasattr(sogen, "linux"))
        if not WINDOWS_ARM64_HOST:
            self.skipTest("x86-64 guest personalities are available on this host")
        with self.assertRaisesRegex(RuntimeError, "unavailable"):
            sogen.windows.create_empty()
        with self.assertRaisesRegex(RuntimeError, "unavailable"):
            sogen.linux.create_empty()

    def test_windows_application_registers_use_the_arm64_guest(self):
        root = os.getenv("EMULATOR_ARM64_ROOT")
        if not root or not os.path.isfile(os.path.join(root, "filesys", "c", "hello_crt.exe")):
            self.skipTest("Set EMULATOR_ARM64_ROOT to an ARM64 root containing C:\\hello_crt.exe")
        app = sogen.windows.create_application(r"C:\hello_crt.exe", emulation_root=root)
        output = []
        app.callbacks.on_stdout = output.append
        app.setup_process_if_necessary()
        reg = sogen.arm64.Register
        self.assertNotEqual(app.read_register(reg.x18), 0)
        app.write_register(reg.x9, 0x1234)
        self.assertEqual(app.read_register(reg.x9), 0x1234)
        with self.assertRaises(RuntimeError):
            app.read_register(sogen.Register.rax)
        app.start()
        self.assertEqual(app.process.exit_status, 0)
        self.assertIn("Hello, world!", "".join(output))

    def run_hello_crt_with_write_file_hook(self, root, intercept):
        app = sogen.windows.create_application(r"C:\hello_crt.exe", emulation_root=root)
        output = []
        written = []
        app.callbacks.on_stdout = output.append

        @sogen.windows.api_call(cc=sogen.CallingConvention.stdcall,
                                params=[ctypes.c_uint64, ctypes.c_uint64, ctypes.c_uint32, ctypes.c_uint64])
        def on_write_file(call, params):
            _, buffer, length, bytes_written = params
            written.append(bytes(app.read_memory(buffer, length)).decode())
            if not intercept:
                return sogen.ApiContinuation.run_original
            app.write_memory(bytes_written, struct.pack("<I", length))
            call.return_value = 1
            return sogen.ApiContinuation.intercept

        app.hooks.apis["WriteFile"] = on_write_file
        try:
            app.start()
            return app.process.exit_status, "".join(output), written
        finally:
            # The hook refers to the application; drop it so the application (and its partition) is freed.
            del app.hooks.apis["WriteFile"]
            app.callbacks.on_stdout = None

    def test_windows_api_hooks_observe_and_intercept_arm64_calls(self):
        root = os.getenv("EMULATOR_ARM64_ROOT")
        if not root or not os.path.isfile(os.path.join(root, "filesys", "c", "hello_crt.exe")):
            self.skipTest("Set EMULATOR_ARM64_ROOT to an ARM64 root containing C:\\hello_crt.exe")

        for intercept in (False, True):
            exit_status, output, written = self.run_hello_crt_with_write_file_hook(root, intercept)
            self.assertEqual(exit_status, 0)
            self.assertTrue(any("Hello, world!" in text for text in written))
            self.assertEqual("Hello, world!" in output, not intercept)
if __name__ == "__main__":
    unittest.main()
