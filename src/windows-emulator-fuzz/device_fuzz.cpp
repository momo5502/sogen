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
#include <devices/afd_endpoint.hpp>
#include <devices/console.hpp>
#include <devices/gpu_bridge.hpp>
#include <devices/mount_point_manager.hpp>
#include <devices/network_store_interface.hpp>
#include <devices/security_support_provider.hpp>

#include <vector>

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

        // One persistent instance of every fuzzable io_device. Constructing a device that reaches out
        // to host resources (e.g. gpu_bridge's vulkan_host) may fail on a headless box, so guard each.
        std::vector<std::unique_ptr<io_device>> make_devices()
        {
            std::vector<std::unique_ptr<io_device>> devices;
            const auto add = [&](auto factory) {
                try
                {
                    if (auto d = factory())
                    {
                        devices.push_back(std::move(d));
                    }
                }
                catch (...)
                {
                }
            };

            add([] { return create_security_support_provider(); });
            add([] { return create_console_device(); });
            add([] { return create_mount_point_manager(); });
            add([] { return create_network_store_interface(); });
            add([] { return create_afd_endpoint(false); });
            add([] { return create_afd_async_connect_hlp(false); });
            add([] { return create_gpu_bridge(); });
            return devices;
        }

        // Persistent across iterations: building the emulator is expensive, so do it once.
        struct fuzz_state
        {
            static constexpr size_t scratch_size = 0x2000;

            windows_emulator emu = make_bare_emulator();
            std::vector<std::unique_ptr<io_device>> devices = make_devices();
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
        // Header: [u32 device selector][u32 ioctl code], then the request/input buffer.
        if (data.size() < 2 * sizeof(uint32_t))
        {
            return;
        }

        auto& s = state();
        if (s.devices.empty())
        {
            return;
        }

        uint32_t selector = 0;
        uint32_t code = 0;
        std::memcpy(&selector, data.data(), sizeof(selector));
        std::memcpy(&code, data.data() + sizeof(selector), sizeof(code));

        io_device& device = *s.devices[selector % s.devices.size()];
        const auto payload = data.subspan(2 * sizeof(uint32_t));

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
            (void)device.io_control(s.emu, ctx);
        }
        catch (...)
        {
        }
    }
}
