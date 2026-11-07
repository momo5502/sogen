#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    uint16_t handle_NtUserRegisterClassExWOW(const syscall_context& c, emulator_object<EMU_WNDCLASSEX> wnd_class_ex,
                                             emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> class_name,
                                             emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> class_version,
                                             emulator_object<CLSMENUNAME<EmulatorTraits<Emu64>>> class_menu_name, DWORD function_id,
                                             DWORD flags, emulator_pointer wow);
    emulator_pointer handle_NtUserSetClassLongPtr(const syscall_context& c, handle hWnd, int nIndex, emulator_pointer dwNewLong, BOOL Ansi);
}

namespace sogen::test
{
    namespace
    {
        constexpr std::u16string_view class_name = u"SogenTestClass";
        constexpr std::u16string_view menu_name = u"SogenTestMenu";
        constexpr uint64_t menu_resource_id = 0x7B;
        constexpr uint32_t unicode_class_flags = 0xC0;
        constexpr int gclp_wndproc = -24;
        constexpr int gclp_hbrbackground = -10;

        constexpr EMU_WNDCLASSEX test_class{
            .cbSize = sizeof(EMU_WNDCLASSEX),
            .style = 0x3,
            .lpfnWndProc = 0x140001000,
            .cbClsExtra = 8,
            .cbWndExtra = 16,
            .hInstance = 0x140000000,
            .hIcon = 0,
            .hCursor = 0,
            .hbrBackground = 6,
            .lpszMenuName = 0,
            .lpszClassName = 0,
            .hIconSm = 0x1234,
        };

        class WindowClassTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                // A thread to take the last error the registration and SetClassLongPtr report.
                this->emu.setup_process_if_necessary();
                this->storage = this->emu.memory.allocate_memory(0x1000, memory_permission::read_write);
                this->menu_name_buffer = this->storage + 0x400;
                this->empty_menu_name_buffer = this->storage + 0x500;
                this->emu.memory.write_memory(this->menu_name_buffer, menu_name.data(), menu_name.size() * sizeof(char16_t));
            }

            // user32's InitClsMenuNameW points pusMenuName at the caller's string, or carries the bare resource ID.
            uint16_t register_class(const emulator_pointer menu_name_value, const int class_extra = test_class.cbClsExtra)
            {
                const emulator_object<EMU_WNDCLASSEX> wnd_class{this->emu.memory, this->storage};
                const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> name{this->emu.memory, this->storage + 0x100};
                const emulator_object<CLSMENUNAME<EmulatorTraits<Emu64>>> class_menu_name{this->emu.memory, this->storage + 0x200};
                const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> menu_string{this->emu.memory, this->storage + 0x280};
                const auto name_buffer = this->storage + 0x300;

                auto registered = test_class;
                registered.cbClsExtra = class_extra;
                wnd_class.write(registered);
                this->emu.memory.write_memory(name_buffer, class_name.data(), class_name.size() * sizeof(char16_t));
                name.write({.Length = static_cast<USHORT>(class_name.size() * sizeof(char16_t)),
                            .MaximumLength = static_cast<USHORT>(class_name.size() * sizeof(char16_t)),
                            .Buffer = name_buffer});
                menu_string.write({.Length = 0, .MaximumLength = 0, .Buffer = menu_name_value});
                class_menu_name.write(
                    {.pszClientAnsiMenuName = 0x5000, .pwszClientUnicodeMenuName = 0x6000, .pusMenuName = this->storage + 0x280});

                return syscalls::handle_NtUserRegisterClassExWOW(this->context, wnd_class, name, {this->emu.memory}, class_menu_name, 0,
                                                                 unicode_class_flags, 0);
            }

            USER_CLASS class_object()
            {
                const auto entry = this->emu.process.classes.find(std::u16string{class_name});
                EXPECT_NE(entry, this->emu.process.classes.end());
                return this->emu.memory.read_memory<USER_CLASS>(entry->second.guest_obj_addr);
            }

            DWORD last_error()
            {
                return this->emu.vcpu(0).active_thread->teb64->read().LastErrorValue;
            }

            handle create_window()
            {
                auto [handle, window] = this->emu.process.windows.create(this->emu.memory);
                window.class_name = class_name;
                return handle;
            }

            windows_emulator emu = create_sample_emulator();
            syscall_context context{
                .win_emu = this->emu, .emu = this->emu.vcpu(0).cpu, .vcpu = this->emu.vcpu(0), .proc = this->emu.process};
            uint64_t storage{};
            uint64_t menu_name_buffer{};
            uint64_t empty_menu_name_buffer{};
        };
    }

    TEST_F(WindowClassTest, ClassObjectCarriesTheRegisteredValues)
    {
        const auto atom = this->register_class(this->menu_name_buffer);
        ASSERT_NE(0, atom);

        const auto cls = this->class_object();
        EXPECT_EQ(atom, cls.atom);
        EXPECT_EQ(unicode_class_flags, cls.CSF_flags);
        EXPECT_EQ(test_class.style, cls.style);
        EXPECT_EQ(8u, cls.cbclsExtra);
        EXPECT_EQ(16u, cls.cbwndExtra);
        EXPECT_EQ(test_class.lpfnWndProc, cls.lpfnWndProc);
        EXPECT_EQ(~0ull, cls.lpszMenuName);
        EXPECT_EQ(0x5000u, cls.lpszClientAnsiMenuName);
        EXPECT_EQ(0x6000u, cls.lpszClientUnicodeMenuName);
        EXPECT_EQ(test_class.hIconSm, cls.hIconSm);
        EXPECT_EQ(test_class.hInstance, cls.hModule);
        EXPECT_EQ(test_class.hbrBackground, cls.hbrBackground);
    }

    TEST_F(WindowClassTest, ResourceIdMenuNameIsNotMarked)
    {
        this->register_class(menu_resource_id);
        EXPECT_EQ(0u, this->class_object().lpszMenuName);
    }

    TEST_F(WindowClassTest, EmptyMenuNameIsNotMarked)
    {
        this->register_class(this->empty_menu_name_buffer);
        EXPECT_EQ(0u, this->class_object().lpszMenuName);
    }

    TEST_F(WindowClassTest, SetClassLongPtrUpdatesTheClassObject)
    {
        this->register_class(menu_resource_id);
        const auto handle = this->create_window();

        EXPECT_EQ(test_class.lpfnWndProc, syscalls::handle_NtUserSetClassLongPtr(this->context, handle, gclp_wndproc, 0x140002000, FALSE));
        EXPECT_EQ(0x140002000u, this->class_object().lpfnWndProc);
        EXPECT_EQ(test_class.hbrBackground, syscalls::handle_NtUserSetClassLongPtr(this->context, handle, gclp_hbrbackground, 9, FALSE));
        EXPECT_EQ(9u, this->class_object().hbrBackground);
    }

    // The extra bytes follow the 0x58-byte class object, zeroed, where user32 reads positive GetClassLongPtr indexes;
    // more of them than USER_CLASS's padding holds push the ANSI class name behind them.
    TEST_F(WindowClassTest, ClassExtraBytesFollowTheClassObject)
    {
        constexpr int class_extra = 0x200;
        ASSERT_NE(0, this->register_class(menu_resource_id, class_extra));

        const auto address = this->emu.process.classes.find(std::u16string{class_name})->second.guest_obj_addr;
        const auto cls = this->class_object();
        EXPECT_EQ(static_cast<uint32_t>(class_extra), cls.cbclsExtra);
        EXPECT_GE(cls.lpszAnsiClassName, address + process_context::user_class_extra_offset + class_extra);
        std::array<char, 15> ansi_name{};
        this->emu.memory.read_memory(cls.lpszAnsiClassName, ansi_name.data(), ansi_name.size());
        EXPECT_EQ(std::string_view{"SogenTestClass"}, std::string_view(ansi_name.data(), 14));
        EXPECT_EQ('\0', ansi_name[14]);

        std::array<uint8_t, class_extra> extra{};
        this->emu.memory.read_memory(address + process_context::user_class_extra_offset, extra.data(), extra.size());
        EXPECT_TRUE(std::ranges::all_of(extra, [](const uint8_t value) { return value == 0; }));
    }

    // SetClassLongPtr with a positive index swaps a value in the extra bytes, as xxxSetClassLongPtr does, up to the last
    // index whose 8 bytes fit; past it the call fails with ERROR_INVALID_INDEX.
    TEST_F(WindowClassTest, SetClassLongPtrStoresPositiveIndexesInTheExtraBytes)
    {
        constexpr int class_extra = 16;
        this->register_class(menu_resource_id, class_extra);
        const auto handle = this->create_window();
        const auto address = this->emu.process.classes.find(std::u16string{class_name})->second.guest_obj_addr;
        constexpr int last_index = class_extra - 8;

        EXPECT_EQ(0u, syscalls::handle_NtUserSetClassLongPtr(this->context, handle, last_index, 0x1122334455667788, FALSE));
        EXPECT_EQ(0x1122334455667788u,
                  this->emu.memory.read_memory<uint64_t>(address + process_context::user_class_extra_offset + last_index));
        EXPECT_EQ(0x1122334455667788u, syscalls::handle_NtUserSetClassLongPtr(this->context, handle, last_index, 5, FALSE));

        EXPECT_EQ(0u, syscalls::handle_NtUserSetClassLongPtr(this->context, handle, last_index + 1, 7, FALSE));
        EXPECT_EQ(1413u, this->last_error()); // ERROR_INVALID_INDEX
        EXPECT_EQ(5u, this->emu.memory.read_memory<uint64_t>(address + process_context::user_class_extra_offset + last_index));
    }

    // A negative cbClsExtra fails the registration: with ERROR_INVALID_PARAMETER while 0x58 + cbClsExtra wraps in 32 bits,
    // and as a class object of about 4 GB the desktop heap cannot hold below that.
    TEST_F(WindowClassTest, NegativeClassExtraFailsTheRegistration)
    {
        for (const int class_extra : {-1, -0x58, -0x59, -1000})
        {
            this->emu.vcpu(0).active_thread->teb64->access([](TEB64& teb) { teb.LastErrorValue = 0; });
            EXPECT_EQ(0, this->register_class(menu_resource_id, class_extra)) << class_extra;
            EXPECT_EQ(class_extra >= -0x58 ? 87u : 0u, this->last_error()) << class_extra; // ERROR_INVALID_PARAMETER
            EXPECT_EQ(this->emu.process.classes.end(), this->emu.process.classes.find(std::u16string{class_name}));
        }
    }

    // A large positive cbClsExtra asks for a class object, up to about 2 GB, that the 20 MB desktop heap cannot hold. That
    // fails the registration without an error, and the class name's atom, added only once the object exists, is not added.
    TEST_F(WindowClassTest, ClassObjectTheDesktopHeapCannotHoldFailsTheRegistration)
    {
        constexpr int desktop_heap_size = 20480 * 1024;
        for (const int class_extra : {desktop_heap_size, 0x7FFFFFA7, 0x7FFFFFFF})
        {
            this->emu.vcpu(0).active_thread->teb64->access([](TEB64& teb) { teb.LastErrorValue = 0; });
            EXPECT_EQ(0, this->register_class(menu_resource_id, class_extra)) << class_extra;
            EXPECT_EQ(0u, this->last_error()) << class_extra;
            EXPECT_EQ(this->emu.process.classes.end(), this->emu.process.classes.find(std::u16string{class_name}));
            EXPECT_FALSE(this->emu.process.find_atom(class_name).has_value()) << class_extra;
        }
    }
}
