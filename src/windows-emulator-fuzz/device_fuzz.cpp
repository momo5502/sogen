// Engine-independent core for fuzzing the emulator's IO device handlers with untrusted guest bytes.
// (This fuzzes the HOST — the emulator's own device/syscall handling — NOT a guest application; the
// guest-app fuzzer lives in src/fuzzer and is a different thing.)

#include "fuzz_target.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

#include <windows_emulator.hpp>
#include <network/static_socket_factory.hpp>

#include <io_device.hpp>
#include <devices/security_support_provider.hpp>

#include "mock_emulator.hpp"

namespace sogen::fuzz
{
    namespace
    {
        // Bare emulator: mock backend (no unicorn), empty root, no registry hives, never start()'d.
        // Just a guest-memory arena to back the device handler's reads/writes.
        windows_emulator make_bare_emulator()
        {
            emulator_settings settings{};
            settings.use_relative_time = true;
            settings.load_registry = false;

            emulator_interfaces interfaces{};
            interfaces.socket_factory = network::create_static_socket_factory();

            return windows_emulator{mock::make_mock_emulator(), settings, {}, std::move(interfaces)};
        }

        // Persistent across iterations: building the emulator is expensive, so do it once.
        struct fuzz_state
        {
            static constexpr size_t scratch_size = 0x2000;

            windows_emulator emu = make_bare_emulator();
            std::unique_ptr<io_device> device = create_security_support_provider();
            uint64_t scratch = emu.memory.allocate_memory(scratch_size, memory_permission::read_write);
        };

        fuzz_state& state()
        {
            static fuzz_state s{};
            return s;
        }
    }

    void run(std::span<const uint8_t> data)
    {
        if (data.size() < sizeof(uint32_t))
        {
            return;
        }

        auto& s = state();

        // First 4 bytes select the ioctl code; the remainder is the request/input buffer.
        uint32_t code = 0;
        std::memcpy(&code, data.data(), sizeof(code));
        const auto payload = data.subspan(sizeof(code));

        constexpr size_t half = fuzz_state::scratch_size / 2;
        const uint64_t in_buf = s.scratch;
        const uint64_t out_buf = s.scratch + half;
        const size_t in_len = std::min(payload.size(), half);

        if (in_len > 0)
        {
            s.emu.emu().write_memory(in_buf, payload.data(), in_len);
        }

        io_device_context ctx{s.emu.emu()};
        ctx.io_control_code = code;
        ctx.input_buffer = in_buf;
        ctx.input_buffer_length = static_cast<ULONG>(in_len);
        ctx.output_buffer = out_buf;
        ctx.output_buffer_length = static_cast<ULONG>(half);

        try
        {
            // Guest memory access is MMU-checked and throws by design; only sanitizer aborts are real bugs.
            (void)s.device->io_control(s.emu, ctx);
        }
        catch (...)
        {
        }
    }
}
