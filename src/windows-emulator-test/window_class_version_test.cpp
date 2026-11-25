#include "std_include.hpp"

namespace sogen::syscalls
{
    uint16_t handle_NtUserRegisterClassExWOW(const syscall_context& c, emulator_object<EMU_WNDCLASSEX> wnd_class_ex,
                                             emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> class_name,
                                             emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> class_version,
                                             emulator_object<CLSMENUNAME<EmulatorTraits<Emu64>>> class_menu_name, DWORD function_id,
                                             DWORD flags, emulator_pointer wow);
    BOOL handle_NtUserUnregisterClass(const syscall_context& c, emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> class_name,
                                      emulator_pointer instance, emulator_object<CLSMENUNAME<EmulatorTraits<Emu64>>> class_menu_name);
    uint16_t handle_NtUserGetClassInfoEx(const syscall_context& c, hinstance instance,
                                         emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> class_name,
                                         emulator_object<EMU_WNDCLASSEX> wnd_class_ex, emulator_pointer menu_name, BOOL ansi);
    hwnd handle_NtUserCreateWindowEx(const syscall_context& c, DWORD ex_style, emulator_object<LARGE_STRING> class_name,
                                     emulator_object<LARGE_STRING> class_version, emulator_object<LARGE_STRING> window_name, DWORD style,
                                     int x, int y, int width, int height, hwnd parent, hmenu menu, hinstance instance, pointer l_param,
                                     DWORD flags, pointer acbi_buffer);
    uint64_t handle_NtUserFindWindowEx(const syscall_context& c, hwnd parent, hwnd child_after,
                                       emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> class_name,
                                       emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> window_name);
    emulator_pointer handle_NtUserSetClassLongPtr(const syscall_context& c, handle hWnd, int nIndex, emulator_pointer dwNewLong, BOOL Ansi);
}

namespace sogen::test
{
    namespace
    {
        constexpr DWORD error_cannot_find_wnd_class = 1407;
        constexpr DWORD error_class_does_not_exist = 1411;
        constexpr DWORD error_class_already_exists = 1410;
        constexpr int gclp_wndproc = -24;
        constexpr uint32_t unicode_class_flags = 0xC0;
        constexpr uint32_t global_class_style = 0x408b;
        constexpr uint32_t local_class_style = 0x8b;

        class WindowClassVersionTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                this->emu.setup_process_if_necessary();
                this->storage = this->emu.memory.allocate_memory(0x4000, memory_permission::read_write);
            }

            uint64_t put_string(const std::u16string_view text)
            {
                const auto buffer = this->storage + this->used;
                this->used += (text.size() + 1) * sizeof(char16_t);
                this->emu.memory.write_memory(buffer, text.data(), text.size() * sizeof(char16_t));
                return buffer;
            }

            emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> unicode_string(const std::u16string_view text)
            {
                const auto length = static_cast<USHORT>(text.size() * sizeof(char16_t));
                const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> object{this->emu.memory, this->storage + this->used};
                this->used += sizeof(UNICODE_STRING<EmulatorTraits<Emu64>>);
                object.write({.Length = length, .MaximumLength = length, .Buffer = this->put_string(text)});
                return object;
            }

            emulator_object<LARGE_STRING> large_string(const std::u16string_view text)
            {
                const emulator_object<LARGE_STRING> object{this->emu.memory, this->storage + this->used};
                this->used += sizeof(LARGE_STRING);
                LARGE_STRING value{};
                value.Length = static_cast<ULONG>(text.size() * sizeof(char16_t));
                value.MaximumLength = value.Length;
                value.Buffer = this->put_string(text);
                object.write(value);
                return object;
            }

            uint16_t register_class(const std::u16string_view name, const std::u16string_view version, const uint64_t wnd_proc,
                                    const DWORD function_id = 0, const uint32_t style = global_class_style,
                                    const uint64_t instance = 0x140000000)
            {
                const emulator_object<EMU_WNDCLASSEX> wnd_class{this->emu.memory, this->storage + this->used};
                this->used += sizeof(EMU_WNDCLASSEX);
                EMU_WNDCLASSEX info{};
                info.cbSize = sizeof(EMU_WNDCLASSEX);
                info.style = style;
                info.lpfnWndProc = wnd_proc;
                info.hInstance = instance;
                wnd_class.write(info);

                const emulator_object<CLSMENUNAME<EmulatorTraits<Emu64>>> menu_name{this->emu.memory, this->storage + this->used};
                this->used += sizeof(CLSMENUNAME<EmulatorTraits<Emu64>>);
                menu_name.write({});

                return syscalls::handle_NtUserRegisterClassExWOW(this->context, wnd_class, this->unicode_string(name),
                                                                 this->unicode_string(version), menu_name, function_id, unicode_class_flags,
                                                                 0);
            }

            uint16_t class_info(const std::u16string_view key, EMU_WNDCLASSEX& info)
            {
                const emulator_object<EMU_WNDCLASSEX> wnd_class{this->emu.memory, this->storage + this->used};
                this->used += sizeof(EMU_WNDCLASSEX);
                const auto atom = syscalls::handle_NtUserGetClassInfoEx(this->context, 0, this->unicode_string(key), wnd_class, 0, FALSE);
                info = wnd_class.read();
                return atom;
            }

            BOOL unregister_class(const std::u16string_view key)
            {
                return syscalls::handle_NtUserUnregisterClass(this->context, this->unicode_string(key), 0, {this->emu.memory});
            }

            window* create_window(const std::u16string_view class_name, const std::u16string_view version)
            {
                std::set<std::remove_cvref_t<decltype(this->emu.process.windows.begin()->first)>> existing{};
                for (const auto& [index, win] : this->emu.process.windows)
                {
                    existing.insert(index);
                }

                (void)syscalls::handle_NtUserCreateWindowEx(this->context, 0, this->large_string(class_name),
                                                            version.empty() ? emulator_object<LARGE_STRING>{this->emu.memory}
                                                                            : this->large_string(version),
                                                            {this->emu.memory}, 0, 0, 0, 100, 100, 0, 0, 0x140000000, 0, 0, 0);

                for (auto& [index, win] : this->emu.process.windows)
                {
                    if (!existing.contains(index))
                    {
                        return &win;
                    }
                }

                return nullptr;
            }

            uint16_t window_fnid(const window& win)
            {
                return win.guest.read().fnid;
            }

            uint16_t class_fnid(const std::u16string_view key)
            {
                const auto entry = this->emu.process.classes.find(key);
                return emulator_object<USER_CLASS>{this->emu.memory, entry->second.guest_obj_addr}.read().fnid;
            }

            DWORD last_error()
            {
                return this->emu.vcpu(0).active_thread->teb64->read().LastErrorValue;
            }

            void clear_last_error()
            {
                this->emu.vcpu(0).active_thread->teb64->access([](TEB64& teb) { teb.LastErrorValue = 0; });
            }

            windows_emulator emu = create_sample_emulator();
            syscall_context context{
                .win_emu = this->emu, .emu = this->emu.vcpu(0).cpu, .vcpu = this->emu.vcpu(0), .proc = this->emu.process};
            uint64_t storage{};
            uint64_t used{};
        };
    }

    // Windows 11 26300: with comctl32 v6 active user32 sends class "Button" with version "6.0.26100.9818!Button". The
    // unversioned Button user32 registers is a different class, and comctl32 registers its own under the
    // versioned key; both report the same atom.
    TEST_F(WindowClassVersionTest, VersionedClassCoexistsWithTheUnversionedClassOfTheSameName)
    {
        constexpr std::u16string_view versioned = u"6.0.26100.9818!Button";
        constexpr uint64_t classic_proc = 0x7ff904916000;
        constexpr uint64_t themed_proc = 0x7ff8e0be2000;
        constexpr uint64_t replaced_proc = 0x7ff8e0be3000;
        EMU_WNDCLASSEX info{};

        EXPECT_EQ(nullptr, this->create_window(u"Button", versioned));
        EXPECT_EQ(error_cannot_find_wnd_class, this->last_error());

        const auto classic_atom = this->register_class(u"Button", u"Button", classic_proc);
        ASSERT_NE(0, classic_atom);
        this->clear_last_error();
        EXPECT_EQ(nullptr, this->create_window(u"Button", versioned));
        EXPECT_EQ(error_cannot_find_wnd_class, this->last_error());

        EXPECT_EQ(classic_atom, this->register_class(u"Button", versioned, themed_proc));
        EXPECT_EQ(classic_atom, this->class_info(u"Button", info));
        EXPECT_EQ(classic_proc, info.lpfnWndProc);
        EXPECT_EQ(classic_atom, this->class_info(versioned, info));
        EXPECT_EQ(themed_proc, info.lpfnWndProc);

        const auto* themed_window = this->create_window(u"Button", versioned);
        ASSERT_NE(nullptr, themed_window);
        EXPECT_EQ(u"Button", themed_window->class_name);
        EXPECT_EQ(themed_proc, themed_window->wnd_proc);

        const handle window_handle{.bits = themed_window->handle};
        EXPECT_EQ(themed_proc, syscalls::handle_NtUserSetClassLongPtr(this->context, window_handle, gclp_wndproc, replaced_proc, FALSE));
        EXPECT_EQ(classic_atom, this->class_info(versioned, info));
        EXPECT_EQ(replaced_proc, info.lpfnWndProc);
        EXPECT_EQ(classic_atom, this->class_info(u"Button", info));
        EXPECT_EQ(classic_proc, info.lpfnWndProc);
    }

    // Windows 11 26300: a class registered as "ProbeDefault" with version "1.0.0.0!ProbeDefault" is reachable only through the
    // version string. A lookup or unregistration by the plain name fails with ERROR_CLASS_DOES_NOT_EXIST, registering the
    // version again fails with ERROR_CLASS_ALREADY_EXISTS, and GetClassName still returns the plain name.
    TEST_F(WindowClassVersionTest, ClassRegisteredUnderAVersionIsOnlyReachableThroughIt)
    {
        constexpr std::u16string_view name = u"ProbeDefault";
        constexpr std::u16string_view versioned = u"1.0.0.0!ProbeDefault";
        EMU_WNDCLASSEX info{};

        const auto atom = this->register_class(name, versioned, 0x7ff904916090);
        ASSERT_NE(0, atom);
        EXPECT_EQ(atom, this->class_info(versioned, info));

        this->clear_last_error();
        EXPECT_EQ(0, this->class_info(name, info));
        EXPECT_EQ(error_class_does_not_exist, this->last_error());

        this->clear_last_error();
        EXPECT_EQ(nullptr, this->create_window(name, {}));
        EXPECT_EQ(error_cannot_find_wnd_class, this->last_error());
        EXPECT_EQ(nullptr, this->create_window(name, name));

        const auto* redirected_window = this->create_window(versioned, versioned);
        ASSERT_NE(nullptr, redirected_window);
        EXPECT_EQ(name, redirected_window->class_name);

        this->clear_last_error();
        EXPECT_EQ(0, this->register_class(name, versioned, 0x7ff904916090));
        EXPECT_EQ(error_class_already_exists, this->last_error());

        this->clear_last_error();
        EXPECT_EQ(0, this->register_class(name, versioned, 0x7ff904916090, 0, global_class_style, 0x180000000));
        EXPECT_EQ(error_class_already_exists, this->last_error());

        // win32kfull 10.0.26100.9974 InternalRegisterClassEx: a local class of the same hInstance collides with any registration,
        // a CS_GLOBALCLASS one included, while a global class only collides with another CS_GLOBALCLASS registration.
        constexpr std::u16string_view local_name = u"ProbeLocal";
        constexpr uint64_t first_module = 0x140000000;
        constexpr uint64_t second_module = 0x180000000;
        constexpr uint64_t first_proc = 0x7ff904916100;
        constexpr uint64_t second_proc = 0x7ff904916200;

        const auto local_atom = this->register_class(local_name, local_name, first_proc, 0, local_class_style, first_module);
        ASSERT_NE(0, local_atom);

        this->clear_last_error();
        EXPECT_EQ(0, this->register_class(local_name, local_name, first_proc, 0, local_class_style, first_module));
        EXPECT_EQ(error_class_already_exists, this->last_error());

        this->clear_last_error();
        EXPECT_EQ(local_atom, this->register_class(local_name, local_name, second_proc, 0, local_class_style, second_module));
        EXPECT_EQ(0, this->last_error());
        EXPECT_EQ(local_atom, this->class_info(local_name, info));
        EXPECT_EQ(second_proc, info.lpfnWndProc);

        this->clear_last_error();
        EXPECT_EQ(0, this->register_class(local_name, local_name, second_proc, 0, global_class_style, second_module));
        EXPECT_EQ(error_class_already_exists, this->last_error());

        this->clear_last_error();
        EXPECT_FALSE(this->unregister_class(name));
        EXPECT_EQ(error_class_does_not_exist, this->last_error());
        EXPECT_TRUE(this->unregister_class(versioned));

        this->clear_last_error();
        EXPECT_EQ(0, this->class_info(versioned, info));
        EXPECT_EQ(error_class_does_not_exist, this->last_error());
    }

    // user32 registers Button, Edit and Static itself, possibly after sogen has already synthesised them for a CreateWindowEx,
    // so that registration has to succeed and take over from the synthesised class.
    TEST_F(WindowClassVersionTest, RegistrationReplacesAClassSynthesisedByAnEarlierLookup)
    {
        constexpr uint64_t registered_proc = 0x7ff904916000;
        constexpr uint64_t replaced_proc = 0x7ff904916100;
        constexpr DWORD button_fnid = 0x2a1;
        EMU_WNDCLASSEX info{};

        const auto* existing_window = this->create_window(u"Button", {});
        ASSERT_NE(nullptr, existing_window);
        EXPECT_EQ(button_fnid, this->window_fnid(*existing_window));
        ASSERT_NE(registered_proc, existing_window->wnd_proc);

        const auto atom = this->register_class(u"Button", u"Button", registered_proc, button_fnid);
        ASSERT_NE(0, atom);
        EXPECT_EQ(atom, this->class_info(u"Button", info));
        EXPECT_EQ(registered_proc, info.lpfnWndProc);
        EXPECT_EQ(0x408b, info.style);

        const handle window_handle{.bits = existing_window->handle};
        (void)syscalls::handle_NtUserSetClassLongPtr(this->context, window_handle, gclp_wndproc, replaced_proc, FALSE);
        EXPECT_EQ(atom, this->class_info(u"Button", info));
        EXPECT_EQ(replaced_proc, info.lpfnWndProc);

        this->clear_last_error();
        EXPECT_EQ(0, this->register_class(u"Button", u"Button", registered_proc, button_fnid));
        EXPECT_EQ(error_class_already_exists, this->last_error());
    }

    // user32 registers its own classes with their FNID_* function id (captured: ComboBox 0x2a2, ComboLBox 0x2a3, Edit 0x2a5),
    // comctl32 registers its v6 controls with 0. The class keeps the id it was registered with, while its windows start without
    // one, because user32's window procedures set it and the list box procedure refuses a ComboLBox window with FNID_COMBOLBOX.
    TEST_F(WindowClassVersionTest, WindowOfAUserClassStartsWithoutFunctionId)
    {
        constexpr DWORD combo_lbox_fnid = 0x2a3;

        ASSERT_NE(0, this->register_class(u"ComboLBox", u"ComboLBox", 0x7ff904916130, combo_lbox_fnid));
        EXPECT_EQ(combo_lbox_fnid, this->class_fnid(u"ComboLBox"));

        const auto* window = this->create_window(u"ComboLBox", {});
        ASSERT_NE(nullptr, window);
        EXPECT_EQ(0, this->window_fnid(*window));
    }

    TEST_F(WindowClassVersionTest, FindWindowMatchesTheClassNameCaseInsensitively)
    {
        const auto* window = this->create_window(u"BUTTON", {});
        ASSERT_NE(nullptr, window);

        for (const auto* name : {u"BUTTON", u"Button", u"button"})
        {
            EXPECT_EQ(window->handle,
                      syscalls::handle_NtUserFindWindowEx(this->context, 0, 0, this->unicode_string(name), {this->emu.memory}));
        }
    }
}
