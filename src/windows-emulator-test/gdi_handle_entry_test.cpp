#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

#include <bit>

namespace sogen::syscalls
{
    uint64_t handle_NtGdiCreateCompatibleDC(const syscall_context& c, hdc dc);
}

namespace sogen::test
{
    // gdi32full reads a DC's attributes (DC_ATTR) through the UserPointer of the DC's handle entry: as it is on Windows 10
    // 2004-22H2 (gdi32!GdiGetEntry copies the entry), decoded with the NtGdiInit cookie on newer builds. The test pins a
    // build of each kind, whatever the emulation root is.
    TEST(GdiHandleEntryTest, DcUserPointerLeadsToItsAttributes)
    {
        for (const uint32_t build : {19045u, 26100u})
        {
            SCOPED_TRACE(build);
            auto emu = create_sample_emulator();
            emu.setup_process_if_necessary();
            emu.version.set_windows_build_number(build);
            const syscall_context context{.win_emu = emu, .emu = emu.vcpu(0).cpu, .vcpu = emu.vcpu(0), .proc = emu.process};

            const auto dc = syscalls::handle_NtGdiCreateCompatibleDC(context, {});
            ASSERT_NE(0u, dc);

            const auto peb = emu.process.peb64.read();
            const auto entry =
                emu.memory.read_memory<GDI_HANDLE_ENTRY64>(peb.GdiSharedHandleTable + ((dc & 0xFFFF) * sizeof(GDI_HANDLE_ENTRY64)));
            ASSERT_NE(0u, entry.Object);

            auto user_pointer = entry.UserPointer;
            if (build != 19045)
            {
                const auto cookie = peb.GdiDCAttributeList;
                ASSERT_NE(0u, cookie & 0x3F); // with no rotation the encoded pointer would equal the raw one
                user_pointer = std::rotr(user_pointer, static_cast<int>(64 - (cookie & 0x3F))) ^ cookie;
            }
            EXPECT_EQ(entry.Object, user_pointer);

            // SelectObject(dc, font) reads the selected font at DC_ATTR+0x128 through it.
            uint64_t font{};
            EXPECT_TRUE(emu.memory.try_read_memory(user_pointer + 0x128, &font, sizeof(font)));
        }
    }
}
