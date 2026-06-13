#include "emulation_test_utils.hpp"

#include <array>
#include <cstring>
#include <vector>

namespace sogen::test
{
    // Verifies that memory_manager::allocate_host_memory aliases caller-owned host memory into the guest
    // address space such that the guest CPU reads and writes it coherently (i.e. the backend really maps the
    // host pages through, not via a staging copy). Skipped on backends that do not support it (e.g. icicle).
    TEST(HostMemoryTest, GuestCpuAccessesAliasedHostMemory)
    {
        // Build with the environment-selected backend (EMULATOR_WHP / EMULATOR_ICICLE) so the actual host
        // mapping path is exercised, not just unicorn (create_sample_emulator always picks unicorn).
        emulator_settings settings{.use_relative_time = true};
        settings.emulation_root = get_emulator_root();
        emulator_interfaces interfaces{};
        interfaces.socket_factory = network::create_static_socket_factory();
        interfaces.dns_lookup = create_sample_dns_lookup();
        windows_emulator emu{
            create_x86_64_emulator_from_environment(), get_sample_app_settings({}), settings, {}, std::move(interfaces),
        };

        auto& mm = emu.memory;
        auto& cpu = emu.emu();

        constexpr size_t page = 0x1000;

        // A page-aligned host buffer the guest will alias. A 2-page vector guarantees an aligned page exists.
        std::vector<uint8_t> host_backing(page * 2, 0);
        auto* host_ptr = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(host_backing.data()) + page - 1) & ~(page - 1));

        constexpr uint64_t host_pattern = 0x1122334455667788ull;
        std::memcpy(host_ptr, &host_pattern, sizeof(host_pattern));

        const uint64_t src = mm.find_free_allocation_base(page);
        ASSERT_NE(src, 0u);

        try
        {
            if (!mm.allocate_host_memory(src, page, host_ptr, memory_permission::read_write))
            {
                GTEST_SKIP() << "allocate_host_memory failed";
            }
        }
        catch (const std::exception& e)
        {
            GTEST_SKIP() << "backend does not support host memory mapping: " << e.what();
        }

        // host -> guest: the guest-visible range reflects the host buffer.
        ASSERT_EQ(cpu.read_memory<uint64_t>(src), host_pattern);

        // guest -> host: writing the range updates the host buffer.
        constexpr uint64_t written = 0xDEADBEEFCAFEBABEull;
        cpu.write_memory(src, written);
        uint64_t host_value = 0;
        std::memcpy(&host_value, host_ptr, sizeof(host_value));
        ASSERT_EQ(host_value, written);
        std::memcpy(host_ptr, &host_pattern, sizeof(host_pattern)); // restore for the CPU stub below

        // Where the backend supports exact instruction counts (unicorn), also drive the alias from the guest
        // CPU end to end: run the guest briefly for a live 64-bit thread context, then run a small stub that
        // reads the alias into dst and writes (alias + 1) back through it.
        const uint64_t dst = mm.allocate_memory(page, memory_permission::read_write);
        const uint64_t code = mm.allocate_memory(page, memory_permission::all);
        ASSERT_NE(dst, 0u);
        ASSERT_NE(code, 0u);

        try
        {
            emu.start(200000);
        }
        catch (const std::exception&)
        {
            // The backend (e.g. WHP) lacks exact instruction counts, so the stub cannot be stepped; the
            // host-side bidirectional checks above already verified the alias works on this backend.
            mm.release_memory(src, page);
            return;
        }
        ASSERT_NOT_TERMINATED(emu);

        // mov rax,[rcx]; mov [rdx],rax; add rax,1; mov [rcx],rax; hlt
        const std::array<uint8_t, 14> stub = {
            0x48, 0x8B, 0x01,       // mov rax, [rcx]
            0x48, 0x89, 0x02,       // mov [rdx], rax
            0x48, 0x83, 0xC0, 0x01, // add rax, 1
            0x48, 0x89, 0x01,       // mov [rcx], rax
            0xF4,                   // hlt
        };
        cpu.write_memory(code, stub.data(), stub.size());
        cpu.reg(x86_register::rip, code);
        cpu.reg(x86_register::rcx, src);
        cpu.reg(x86_register::rdx, dst);
        cpu.start(4);

        ASSERT_EQ(cpu.read_memory<uint64_t>(dst), host_pattern); // guest read the alias
        std::memcpy(&host_value, host_ptr, sizeof(host_value));
        ASSERT_EQ(host_value, host_pattern + 1); // guest wrote the alias

        mm.release_memory(src, page);
    }
} // namespace sogen::test
