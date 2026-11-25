#include "std_include.hpp"

namespace sogen::syscalls
{
    uint32_t handle_NtUserGetClipboardSequenceNumber(const syscall_context& c);
    BOOL handle_NtUserOpenClipboard(const syscall_context& c, hwnd window, emulator_object<BOOL> emptied);
    BOOL handle_NtUserCloseClipboard(const syscall_context& c);
    BOOL handle_NtUserEmptyClipboard(const syscall_context& c);
    BOOL handle_NtUserAddClipboardFormatListener(const syscall_context& c, hwnd window);
    BOOL handle_NtUserRemoveClipboardFormatListener(const syscall_context& c, hwnd window);
    uint64_t handle_NtUserGetClipboardData(const syscall_context& c, uint32_t format, emulator_object<GETCLIPBDATA> request);
    uint64_t handle_NtUserConvertMemHandle(const syscall_context& c, emulator_pointer data, uint32_t size);
    NTSTATUS handle_NtUserCreateLocalMemHandle(const syscall_context& c, uint64_t handle, emulator_pointer buffer, uint32_t size,
                                               emulator_object<uint32_t> needed);
    BOOL handle_NtUserSetClipboardData(const syscall_context& c, uint32_t format, uint64_t data, emulator_object<SETCLIPBDATA> request);
    BOOL handle_NtUserIsClipboardFormatAvailable(const syscall_context& c, uint32_t format);
    int32_t handle_NtUserCountClipboardFormats(const syscall_context& c);
    uint32_t handle_NtUserEnumClipboardFormats(const syscall_context& c, uint32_t format);
    int32_t handle_NtUserGetPriorityClipboardFormat(const syscall_context& c, emulator_pointer formats, int32_t count);
    int32_t handle_NtUserGetClipboardFormatName(const syscall_context& c, uint32_t format, emulator_pointer buffer, int32_t max_count);
    hwnd handle_NtUserGetClipboardOwner(const syscall_context& c);
    hwnd handle_NtUserGetClipboardViewer();
    hwnd handle_NtUserGetOpenClipboardWindow(const syscall_context& c);
    BOOL handle_NtUserGetUpdatedClipboardFormats(const syscall_context& c, emulator_pointer formats, uint32_t count,
                                                 emulator_object<uint32_t> returned);
}

namespace sogen::test
{
    namespace
    {
        constexpr uint32_t k_untouched = 0xDEAD;
        constexpr uint32_t k_error_access_denied = 5;
        constexpr uint32_t k_error_invalid_handle = 6;
        constexpr uint32_t k_error_insufficient_buffer = 122;
        constexpr uint32_t k_error_invalid_parameter = 87;
        constexpr uint32_t k_error_noaccess = 998;
        constexpr uint32_t k_error_not_found = 1168;
        constexpr uint32_t k_error_invalid_window_handle = 1400;
        constexpr uint32_t k_error_clipboard_not_open = 1418;

        constexpr uint32_t cf_text = 1;
        constexpr uint32_t cf_bitmap = 2;
        constexpr uint32_t cf_oemtext = 7;
        constexpr uint32_t cf_dib = 8;
        constexpr uint32_t cf_unicodetext = 13;
        constexpr uint32_t cf_locale = 16;
        constexpr uint32_t cf_dibv5 = 17;

        struct outcome
        {
            outcome(const uint64_t value, const uint32_t error)
                : value(value),
                  error(error)
            {
            }

            uint64_t value;
            uint32_t error;

            bool operator==(const outcome&) const = default;
        };

        struct row
        {
            row(std::string label, const outcome expected, std::function<uint64_t()> call)
                : label(std::move(label)),
                  expected(expected),
                  call(std::move(call))
            {
            }

            std::string label;
            outcome expected;
            std::function<uint64_t()> call;
        };

        class ClipboardTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                emulator_interfaces interfaces{};
                interfaces.ui = std::make_unique<null_ui_backend>();
                interfaces.audio = std::make_unique<null_audio_backend>();
                interfaces.console = std::make_unique<null_console_backend>();
                this->emu = std::make_unique<windows_emulator>(
                    create_x86_64_emulator_from_environment(),
                    emulator_settings{.disable_logging = true, .emulation_root = get_emulator_root(), .load_registry = false},
                    emulator_callbacks{}, std::move(interfaces));

                const auto base = this->emu->memory.allocate_memory(0x10000, memory_permission::read_write);
                this->emu->process.base_allocator = emulator_allocator{this->emu->memory, base, 0x10000};
                this->emu->process.user_handles.setup(false);

                this->main_thread = this->create_thread(8, true);
                this->helper_thread = this->create_thread(12, false);
                this->run_on(this->main_thread);

                this->scratch = this->emu->memory.allocate_memory(0x10000, memory_permission::read_write);
                this->window_a = this->create_window(this->emu->process.threads.get(this->main_thread)->id);
                this->window_b = this->create_window(this->emu->process.threads.get(this->main_thread)->id);
            }

            handle create_thread(const uint32_t id, const bool initial)
            {
                return this->emu->process.threads.store(
                    emulator_thread{this->emu->memory, this->emu->process, 0, 0, 0x100000, 0, id, initial});
            }

            void run_on(const handle thread)
            {
                this->emu->vcpu(0).active_thread = this->emu->process.threads.get(thread);
            }

            hwnd create_window(const uint32_t thread_id)
            {
                auto [handle, win] = this->emu->process.windows.create(this->emu->memory);
                win.handle = handle.bits;
                win.thread_id = thread_id;
                win.guest.access([&](USER_WINDOW& guest) { guest.hWnd = handle.bits; });
                return win.handle;
            }

            syscall_context context()
            {
                return {.win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
            }

            emulator_thread& thread()
            {
                return *this->emu->vcpu(0).active_thread;
            }

            outcome run(const std::function<uint64_t()>& call)
            {
                this->thread().teb64->field<ULONG>(offsetof(TEB64, LastErrorValue)).write(k_untouched);
                const auto value = call();
                return {value, this->thread().teb64->read().LastErrorValue};
            }

            void expect_rows(const std::vector<row>& rows)
            {
                for (const auto& r : rows)
                {
                    const auto actual = this->run(r.call);
                    EXPECT_EQ(r.expected, actual) << r.label;
                }
            }

            emulator_object<uint32_t> scratch_dwords(const size_t offset = 0)
            {
                return {this->emu->memory, this->scratch + offset};
            }

            uint64_t open(const hwnd window)
            {
                const emulator_object<BOOL> emptied{this->emu->memory, this->scratch + 0x8000};
                return syscalls::handle_NtUserOpenClipboard(this->context(), window, emptied);
            }

            uint64_t close()
            {
                return syscalls::handle_NtUserCloseClipboard(this->context());
            }

            uint64_t empty()
            {
                return syscalls::handle_NtUserEmptyClipboard(this->context());
            }

            uint64_t set(const uint32_t format, const uint64_t data, const bool global_handle = true)
            {
                const emulator_object<SETCLIPBDATA> request{this->emu->memory, this->scratch + 0x8100};
                request.write({.global_handle = global_handle ? 1 : 0, .increment_sequence_number = 1});
                return syscalls::handle_NtUserSetClipboardData(this->context(), format, data, request);
            }

            uint64_t sequence()
            {
                return syscalls::handle_NtUserGetClipboardSequenceNumber(this->context());
            }

            int32_t count()
            {
                return syscalls::handle_NtUserCountClipboardFormats(this->context());
            }

            std::vector<uint32_t> enumerate()
            {
                std::vector<uint32_t> formats{};
                for (auto format = syscalls::handle_NtUserEnumClipboardFormats(this->context(), 0); format != 0;
                     format = syscalls::handle_NtUserEnumClipboardFormats(this->context(), format))
                {
                    formats.push_back(format);
                }
                return formats;
            }

            int32_t priority(const std::vector<uint32_t>& formats)
            {
                for (size_t i = 0; i < formats.size(); ++i)
                {
                    this->scratch_dwords(0x8200).write(formats[i], i);
                }
                return syscalls::handle_NtUserGetPriorityClipboardFormat(this->context(), this->scratch + 0x8200,
                                                                         static_cast<int32_t>(formats.size()));
            }

            std::u16string format_name(const uint32_t format, const int32_t max_count, const emulator_pointer buffer)
            {
                std::u16string text(0x100, u'?');
                this->emu->memory.write_memory(this->scratch + 0x9000, text.data(), text.size() * sizeof(char16_t));
                this->last_name_result = syscalls::handle_NtUserGetClipboardFormatName(this->context(), format, buffer, max_count);
                this->emu->memory.read_memory(this->scratch + 0x9000, text.data(), text.size() * sizeof(char16_t));
                return text.substr(0, text.find(u'\0'));
            }

            std::unique_ptr<windows_emulator> emu{};
            handle main_thread{};
            handle helper_thread{};
            uint64_t scratch{};
            hwnd window_a{};
            hwnd window_b{};
            int32_t last_name_result{};
        };
    }

    TEST_F(ClipboardTest, AnEmptyClipboardAnswersEveryQueryAsWindowsDoes)
    {
        auto& c = this->emu->process;
        constexpr std::u16string_view long_name = u"SogenProbeFmt_11532_105255000";
        const auto registered = c.add_or_find_atom(std::u16string{long_name});
        const auto call = [this]() { return this->context(); };

        std::vector<row> rows{
            {"sequence", {0, k_untouched}, [&] { return syscalls::handle_NtUserGetClipboardSequenceNumber(call()); }},
            {"count", {0, k_untouched}, [&] { return static_cast<uint64_t>(syscalls::handle_NtUserCountClipboardFormats(call())); }},
            {"owner", {0, k_untouched}, [&] { return syscalls::handle_NtUserGetClipboardOwner(call()); }},
            {"open window", {0, k_untouched}, [&] { return syscalls::handle_NtUserGetOpenClipboardWindow(call()); }},
            {"close", {0, k_error_clipboard_not_open}, [&] { return syscalls::handle_NtUserCloseClipboard(call()); }},
            {"empty", {0, k_error_clipboard_not_open}, [&] { return syscalls::handle_NtUserEmptyClipboard(call()); }},
            {"set", {0, k_error_clipboard_not_open}, [&] { return this->set(cf_unicodetext, 0x1000); }},
            {"get",
             {0, k_error_clipboard_not_open},
             [&] { return syscalls::handle_NtUserGetClipboardData(call(), cf_unicodetext, {this->emu->memory, this->scratch}); }},
            {"priority of none",
             {0, k_untouched},
             [&] { return static_cast<uint64_t>(this->priority({cf_text, cf_unicodetext, cf_bitmap})); }},
            {"priority of an empty list", {0, k_untouched}, [&] { return static_cast<uint64_t>(this->priority({})); }},
            {"priority with a negative count",
             {0, k_error_noaccess},
             [&] { return static_cast<uint64_t>(syscalls::handle_NtUserGetPriorityClipboardFormat(call(), this->scratch, -1)); }},
            {"priority with a huge count",
             {0, k_untouched},
             [&] { return static_cast<uint64_t>(syscalls::handle_NtUserGetPriorityClipboardFormat(call(), this->scratch, 65536)); }},
            {"updated formats",
             {1, k_untouched},
             [&] { return syscalls::handle_NtUserGetUpdatedClipboardFormats(call(), this->scratch, 64, this->scratch_dwords(0x100)); }},
            {"updated formats without a buffer",
             {1, k_untouched},
             [&] { return syscalls::handle_NtUserGetUpdatedClipboardFormats(call(), 0, 5, this->scratch_dwords(0x100)); }},
            {"updated formats without a result",
             {0, k_error_noaccess},
             [&] { return syscalls::handle_NtUserGetUpdatedClipboardFormats(call(), this->scratch, 8, {this->emu->memory, 0}); }},
        };

        for (const uint32_t format : {0u, cf_text, cf_bitmap, cf_unicodetext, cf_locale, cf_dibv5, 0x200u, 0xBFFFu, 0xC000u, 0xFFFFu,
                                      0x10000u, 0x7FFFFFFFu, 0xFFFFFFFFu, static_cast<uint32_t>(registered)})
        {
            rows.push_back({"available " + std::to_string(format), {0, k_untouched}, [&, format] {
                                return static_cast<uint64_t>(syscalls::handle_NtUserIsClipboardFormatAvailable(call(), format));
                            }});
            rows.push_back({"enumerate from " + std::to_string(format), {0, k_error_clipboard_not_open}, [&, format] {
                                return static_cast<uint64_t>(syscalls::handle_NtUserEnumClipboardFormats(call(), format));
                            }});
        }

        this->expect_rows(rows);
        EXPECT_EQ(0u, this->scratch_dwords(0x100).read());

        const auto buffer = this->scratch + 0x9000;

        for (const uint32_t format :
             {0u, cf_text, cf_bitmap, cf_unicodetext, cf_locale, cf_dibv5, 0x12u, 0x7Fu, 0x80u, 0x1FFu, 0x3FFu, 0xBFFFu, 0x10000u})
        {
            EXPECT_EQ((outcome{0, k_error_invalid_parameter}), this->run([&] {
                return static_cast<uint64_t>(syscalls::handle_NtUserGetClipboardFormatName(this->context(), format, buffer, 64));
            })) << format;
        }

        for (const uint32_t format : {0xFFFFu, 0x7FFFFFFFu, 0xFFFFFFFFu})
        {
            EXPECT_EQ((outcome{0, k_error_invalid_handle}), this->run([&] {
                return static_cast<uint64_t>(syscalls::handle_NtUserGetClipboardFormatName(this->context(), format, buffer, 64));
            })) << format;
        }

        const std::u16string untouched(0x100, u'?');

        struct size_case
        {
            int32_t max_count;
            uint64_t copied;
            uint32_t error;
            std::u16string text;
        };

        const std::vector<size_case> sizes{
            {.max_count = 0, .copied = 0, .error = k_error_insufficient_buffer, .text = untouched},
            {.max_count = 1, .copied = 0, .error = k_error_insufficient_buffer, .text = untouched},
            {.max_count = 2, .copied = 1, .error = k_untouched, .text = u"S"},
            {.max_count = 28, .copied = 27, .error = k_untouched, .text = u"SogenProbeFmt_11532_1052550"},
            {.max_count = 29, .copied = 28, .error = k_untouched, .text = u"SogenProbeFmt_11532_10525500"},
            {.max_count = 30, .copied = 29, .error = k_untouched, .text = std::u16string{long_name}},
            {.max_count = 31, .copied = 29, .error = k_untouched, .text = std::u16string{long_name}},
            {.max_count = 300, .copied = 29, .error = k_untouched, .text = std::u16string{long_name}},
            {.max_count = -1, .copied = 0, .error = k_error_noaccess, .text = untouched},
            {.max_count = -100, .copied = 0, .error = k_error_noaccess, .text = untouched},
        };

        for (const auto& size : sizes)
        {
            std::u16string text{};
            const auto actual = this->run([&] {
                text = this->format_name(registered, size.max_count, buffer);
                return static_cast<uint64_t>(this->last_name_result);
            });
            EXPECT_EQ((outcome{size.copied, size.error}), actual) << size.max_count;
            EXPECT_EQ(size.text, text) << size.max_count;
        }

        EXPECT_EQ((outcome{0, k_error_noaccess}), this->run([&] {
            return static_cast<uint64_t>(syscalls::handle_NtUserGetClipboardFormatName(this->context(), registered, 0, 64));
        }));
        EXPECT_EQ((outcome{0, k_untouched}), this->run([&] {
            return static_cast<uint64_t>(syscalls::handle_NtUserGetClipboardFormatName(this->context(), registered, 0, 0));
        }));
        EXPECT_EQ((outcome{0, k_error_noaccess}), this->run([&] {
            return static_cast<uint64_t>(syscalls::handle_NtUserGetClipboardFormatName(this->context(), cf_text, 0, 64));
        }));
        EXPECT_EQ((outcome{0, k_error_noaccess}), this->run([&] {
            return static_cast<uint64_t>(syscalls::handle_NtUserGetClipboardFormatName(this->context(), registered, 0x10, 64));
        }));

        const auto add = [&](const hwnd window) {
            return this->run([&] { return syscalls::handle_NtUserAddClipboardFormatListener(this->context(), window); });
        };
        const auto remove = [&](const hwnd window) {
            return this->run([&] { return syscalls::handle_NtUserRemoveClipboardFormatListener(this->context(), window); });
        };

        EXPECT_EQ((outcome{0, k_error_invalid_window_handle}), add(0));
        EXPECT_EQ((outcome{1, k_untouched}), add(this->window_a));
        EXPECT_EQ((outcome{0, k_error_invalid_parameter}), add(this->window_a));
        EXPECT_EQ((outcome{1, k_untouched}), remove(this->window_a));
        EXPECT_EQ((outcome{0, k_error_invalid_parameter}), remove(this->window_a));
    }

    TEST_F(ClipboardTest, OpeningTheClipboardIsExclusiveToTheThreadThatOpenedIt)
    {
        const auto desktop_like = this->create_window(0);
        const auto destroyed = this->create_window(this->thread().id);
        this->emu->process.windows.erase(destroyed);

        for (const hwnd bogus : {static_cast<hwnd>(0xDEADBEEF), static_cast<hwnd>(1), static_cast<hwnd>(0xFFFFFFFF), destroyed})
        {
            EXPECT_EQ((outcome{0, k_error_invalid_window_handle}), this->run([&] { return this->open(bogus); })) << bogus;
        }

        EXPECT_EQ((outcome{0, 0}), this->run([&] { return this->open(desktop_like); }));
        EXPECT_EQ((outcome{0, k_error_clipboard_not_open}), this->run([&] { return this->close(); }));

        EXPECT_EQ((outcome{1, k_untouched}), this->run([&] { return this->open(0); }));
        EXPECT_EQ((outcome{1, k_untouched}), this->run([&] { return this->open(0); }));
        EXPECT_EQ(0u, syscalls::handle_NtUserGetOpenClipboardWindow(this->context()));
        EXPECT_EQ((outcome{1, k_untouched}), this->run([&] { return this->close(); }));
        EXPECT_EQ((outcome{0, k_error_clipboard_not_open}), this->run([&] { return this->close(); }));
        EXPECT_EQ((outcome{0, k_error_clipboard_not_open}), this->run([&] { return this->close(); }));

        EXPECT_EQ((outcome{1, k_untouched}), this->run([&] { return this->open(this->window_a); }));
        EXPECT_EQ(this->window_a, syscalls::handle_NtUserGetOpenClipboardWindow(this->context()));
        EXPECT_EQ((outcome{1, k_untouched}), this->run([&] { return this->open(this->window_a); }));
        EXPECT_EQ((outcome{0, k_error_access_denied}), this->run([&] { return this->open(0); }));
        EXPECT_EQ((outcome{0, k_error_access_denied}), this->run([&] { return this->open(this->window_b); }));
        EXPECT_EQ(0u, syscalls::handle_NtUserGetClipboardOwner(this->context()));

        this->run_on(this->helper_thread);
        const auto helper_window = this->create_window(this->thread().id);
        EXPECT_EQ((outcome{0, k_error_access_denied}), this->run([&] { return this->open(0); }));
        EXPECT_EQ((outcome{0, k_error_access_denied}), this->run([&] { return this->open(helper_window); }));
        EXPECT_EQ(this->window_a, syscalls::handle_NtUserGetOpenClipboardWindow(this->context()));
        EXPECT_EQ((outcome{0, k_error_clipboard_not_open}),
                  this->run([&] { return syscalls::handle_NtUserEnumClipboardFormats(this->context(), 0); }));
        EXPECT_EQ((outcome{0, k_error_clipboard_not_open}), this->run([&] { return this->close(); }));
        EXPECT_EQ((outcome{0, k_untouched}),
                  this->run([&] { return static_cast<uint64_t>(syscalls::handle_NtUserCountClipboardFormats(this->context())); }));

        this->run_on(this->main_thread);
        EXPECT_EQ((outcome{1, k_untouched}), this->run([&] { return this->close(); }));
        EXPECT_EQ(0u, syscalls::handle_NtUserGetOpenClipboardWindow(this->context()));

        EXPECT_EQ((outcome{1, k_untouched}), this->run([&] { return this->open(0); }));
        this->run_on(this->helper_thread);
        EXPECT_EQ((outcome{1, k_untouched}), this->run([&] { return this->open(0); }));
        EXPECT_EQ((outcome{1, k_untouched}), this->run([&] { return this->close(); }));
        this->run_on(this->main_thread);
        EXPECT_EQ((outcome{0, k_error_clipboard_not_open}), this->run([&] { return this->close(); }));

        EXPECT_EQ((outcome{1, k_untouched}), this->run([&] { return this->open(this->window_a); }));
        this->run_on(this->helper_thread);
        EXPECT_EQ((outcome{1, k_untouched}), this->run([&] { return this->open(this->window_a); }));
        EXPECT_EQ(this->window_a, syscalls::handle_NtUserGetOpenClipboardWindow(this->context()));
        this->run_on(this->main_thread);
        EXPECT_EQ((outcome{0, k_error_clipboard_not_open}), this->run([&] { return this->close(); }));
        this->run_on(this->helper_thread);
        EXPECT_EQ((outcome{1, k_untouched}), this->run([&] { return this->close(); }));

        EXPECT_EQ((outcome{1, k_untouched}), this->run([&] { return this->open(0); }));
        this->emu->process.terminate_thread(this->thread(), STATUS_SUCCESS);
        this->run_on(this->main_thread);
        EXPECT_EQ(0u, syscalls::handle_NtUserGetOpenClipboardWindow(this->context()));
        EXPECT_EQ((outcome{1, k_untouched}), this->run([&] { return this->open(0); }));
        EXPECT_EQ((outcome{1, k_untouched}), this->run([&] { return this->close(); }));
    }

    TEST_F(ClipboardTest, SettingAndEmptyingTheClipboardTracksFormatsSequenceAndSynthesizedFormats)
    {
        auto& c = this->emu->process;
        constexpr uint64_t first_handle = 0x265ea210008;
        constexpr uint64_t second_handle = 0x265ea210018;
        const uint32_t format_a = c.add_or_find_atom(u"SogenProbeFmt_23824_105290500");
        const uint32_t format_b = c.add_or_find_atom(u"SogenProbeFmtB_23824_105290500");
        const uint32_t unregistered = 0xFFFE;
        const auto get = [&](const uint32_t format) {
            return this->run([&] {
                return syscalls::handle_NtUserGetClipboardData(this->context(), format, {this->emu->memory, this->scratch + 0x8300});
            });
        };

        ASSERT_EQ(1u, this->open(this->window_a));
        EXPECT_EQ(1u, this->empty());
        EXPECT_EQ(1u, this->sequence());
        EXPECT_EQ(this->window_a, syscalls::handle_NtUserGetClipboardOwner(this->context()));

        EXPECT_EQ((outcome{1, k_untouched}), this->run([&] { return this->set(cf_unicodetext, first_handle); }));
        EXPECT_EQ(2u, this->sequence());
        EXPECT_EQ(1, this->count());
        EXPECT_EQ(1u, syscalls::handle_NtUserIsClipboardFormatAvailable(this->context(), cf_unicodetext));
        EXPECT_EQ(0u, syscalls::handle_NtUserIsClipboardFormatAvailable(this->context(), cf_text));
        for (const uint32_t missing : {cf_text, cf_oemtext, cf_locale, 0u, unregistered})
        {
            EXPECT_EQ((outcome{0, k_error_not_found}), get(missing)) << missing;
        }

        EXPECT_EQ((outcome{1, k_untouched}), this->run([&] { return this->set(cf_unicodetext, second_handle); }));
        EXPECT_EQ(3u, this->sequence());
        EXPECT_EQ(1, this->count());

        EXPECT_EQ((outcome{0, k_error_clipboard_not_open}), this->run([&] { return this->set(0, second_handle); }));
        EXPECT_EQ((outcome{0, k_error_invalid_handle}), this->run([&] { return this->set(unregistered, second_handle); }));
        EXPECT_EQ((outcome{0, k_error_invalid_handle}), this->run([&] { return this->set(0xFFFF, second_handle); }));
        EXPECT_EQ(3u, this->sequence());

        ASSERT_EQ(1u, this->close());
        EXPECT_EQ(6u, this->sequence());
        EXPECT_EQ(4, this->count());
        EXPECT_EQ(this->window_a, syscalls::handle_NtUserGetClipboardOwner(this->context()));
        for (const uint32_t present : {cf_text, cf_oemtext, cf_unicodetext, cf_locale})
        {
            EXPECT_EQ(1u, syscalls::handle_NtUserIsClipboardFormatAvailable(this->context(), present)) << present;
        }

        ASSERT_EQ(1u, this->open(0));
        EXPECT_EQ((std::vector<uint32_t>{cf_unicodetext, cf_locale, cf_text, cf_oemtext}), this->enumerate());
        EXPECT_EQ((outcome{second_handle, k_untouched}), get(cf_unicodetext));
        ASSERT_EQ(1u, this->close());
        EXPECT_EQ(6u, this->sequence());

        ASSERT_EQ(1u, this->open(this->window_b));
        EXPECT_EQ(1u, this->empty());
        EXPECT_EQ(7u, this->sequence());
        EXPECT_EQ(0, this->count());
        EXPECT_EQ(this->window_b, syscalls::handle_NtUserGetClipboardOwner(this->context()));
        EXPECT_EQ((outcome{0, k_error_not_found}), get(cf_unicodetext));

        ASSERT_EQ(1u, this->empty());
        ASSERT_EQ(1u, this->empty());
        EXPECT_EQ(9u, this->sequence());

        struct synthesis_case
        {
            uint32_t original;
            std::vector<uint32_t> formats;
        };

        const std::vector<synthesis_case> synthesis{
            {.original = cf_text, .formats = {cf_text, cf_locale, cf_oemtext, cf_unicodetext}},
            {.original = cf_oemtext, .formats = {cf_oemtext, cf_locale, cf_text, cf_unicodetext}},
            {.original = cf_unicodetext, .formats = {cf_unicodetext, cf_locale, cf_text, cf_oemtext}},
            {.original = cf_bitmap, .formats = {cf_bitmap, cf_dib, cf_dibv5}},
            {.original = cf_dib, .formats = {cf_dib, cf_bitmap, cf_dibv5}},
            {.original = cf_dibv5, .formats = {cf_dibv5, cf_bitmap, cf_dib}},
        };

        for (const auto& [original, formats] : synthesis)
        {
            ASSERT_EQ(1u, this->empty());
            const auto before = this->sequence();
            ASSERT_EQ(1u, this->set(original, second_handle, original != cf_bitmap));
            EXPECT_EQ(std::vector<uint32_t>{original}, this->enumerate()) << original;
            ASSERT_EQ(1u, this->close());
            EXPECT_EQ(before + formats.size(), this->sequence()) << original;
            ASSERT_EQ(1u, this->open(this->window_b));
            EXPECT_EQ(formats, this->enumerate()) << original;

            if (original == cf_bitmap)
            {
                EXPECT_EQ((outcome{second_handle, k_untouched}), get(cf_bitmap));
                EXPECT_EQ((outcome{0, k_error_not_found}), get(cf_dib));
                EXPECT_EQ((outcome{0, k_error_not_found}), get(cf_dibv5));
            }
            else if (original == cf_dib)
            {
                EXPECT_EQ((outcome{second_handle, k_untouched}), get(cf_bitmap));
            }
        }

        const emulator_object<GETCLIPBDATA> request{this->emu->memory, this->scratch + 0x8300};
        const auto reported_format = [&](const uint32_t format) {
            const auto result = get(format);
            return std::pair{result, request.read().format};
        };

        ASSERT_EQ(1u, this->empty());
        ASSERT_EQ(1u, this->set(cf_oemtext, first_handle));
        ASSERT_EQ(1u, this->set(cf_unicodetext, second_handle));
        ASSERT_EQ(1u, this->close());
        ASSERT_EQ(1u, this->open(this->window_b));
        EXPECT_EQ((std::pair{outcome{second_handle, k_untouched}, cf_unicodetext}), reported_format(cf_text));
        EXPECT_EQ((std::pair{outcome{first_handle, k_untouched}, cf_oemtext}), reported_format(cf_oemtext));

        ASSERT_EQ(1u, this->empty());
        ASSERT_EQ(1u, this->set(cf_dib, first_handle));
        ASSERT_EQ(1u, this->set(cf_dibv5, second_handle));
        ASSERT_EQ(1u, this->close());
        ASSERT_EQ(1u, this->open(this->window_b));
        EXPECT_EQ((std::pair{outcome{second_handle, k_untouched}, cf_dibv5}), reported_format(cf_bitmap));

        ASSERT_EQ(1u, this->empty());
        this->set(format_a, first_handle);
        this->set(cf_unicodetext, second_handle);
        this->run_on(this->helper_thread);
        EXPECT_EQ((outcome{1, k_untouched}), this->run([&] { return this->set(format_b, 0x265ea210028); }));
        this->run_on(this->main_thread);
        EXPECT_EQ((std::vector<uint32_t>{format_a, cf_unicodetext, format_b}), this->enumerate());
        EXPECT_EQ(static_cast<int32_t>(format_b), this->priority({format_b, cf_unicodetext, format_a}));
        EXPECT_EQ(static_cast<int32_t>(format_a), this->priority({format_a, format_b}));
        EXPECT_EQ(-1, this->priority({cf_text, 0xC075}));
        EXPECT_EQ((outcome{0, k_error_noaccess}), this->run([&] {
            return static_cast<uint64_t>(syscalls::handle_NtUserGetPriorityClipboardFormat(this->context(), 0x10, 1));
        }));
        ASSERT_EQ(1u, this->close());
        ASSERT_EQ(1u, this->open(0));
        EXPECT_EQ((std::vector<uint32_t>{format_a, cf_unicodetext, format_b, cf_locale, cf_text, cf_oemtext}), this->enumerate());
        ASSERT_EQ(1u, this->empty());
        EXPECT_EQ(0u, syscalls::handle_NtUserGetClipboardOwner(this->context()));

        ASSERT_EQ(1u, this->set(cf_unicodetext, 0, false));
        ASSERT_EQ(1u, this->set(format_a, 0, false));
        ASSERT_EQ(1u, this->set(cf_dib, 0x100000000, false));
        const auto before_delayed = this->sequence();
        EXPECT_EQ((std::vector<uint32_t>{cf_unicodetext, format_a, cf_dib}), this->enumerate());
        ASSERT_EQ(1u, this->close());
        EXPECT_EQ(before_delayed + 5, this->sequence());
        EXPECT_EQ(8, this->count());

        const std::vector<std::byte> contents{std::byte{'S'}, std::byte{0}, std::byte{'o'}, std::byte{0}, std::byte{0}, std::byte{0}};
        this->emu->memory.write_memory(this->scratch + 0xA000, contents.data(), contents.size());
        const auto memory_handle =
            syscalls::handle_NtUserConvertMemHandle(this->context(), this->scratch + 0xA000, static_cast<uint32_t>(contents.size()));
        ASSERT_NE(0u, memory_handle);

        ASSERT_EQ(1u, this->open(0));
        ASSERT_EQ(1u, this->empty());
        ASSERT_EQ(1u, this->set(cf_unicodetext, memory_handle));
        ASSERT_EQ(1u, this->close());
        ASSERT_EQ(1u, this->open(0));

        for (const uint32_t format : {cf_unicodetext, cf_text})
        {
            ASSERT_EQ(memory_handle, syscalls::handle_NtUserGetClipboardData(this->context(), format, request));
            EXPECT_EQ(cf_unicodetext, request.read().format) << format;
            EXPECT_NE(0, request.read().global_handle) << format;
        }

        const auto needed = this->scratch_dwords(0x8400);
        EXPECT_EQ(STATUS_BUFFER_TOO_SMALL, syscalls::handle_NtUserCreateLocalMemHandle(this->context(), memory_handle, 0, 0, needed));
        ASSERT_EQ(contents.size(), needed.read());
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtUserCreateLocalMemHandle(this->context(), memory_handle, this->scratch + 0xA100,
                                                                              needed.read(), {this->emu->memory, 0}));
        std::vector<std::byte> copy(contents.size());
        this->emu->memory.read_memory(this->scratch + 0xA100, copy.data(), copy.size());
        EXPECT_EQ(contents, copy);

        const auto no_access = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);
        const auto read_only = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);
        ASSERT_TRUE(this->emu->memory.protect_memory(no_access, 0x1000, memory_permission::none));
        ASSERT_TRUE(this->emu->memory.protect_memory(read_only, 0x1000, memory_permission::read));
        EXPECT_EQ((outcome{0, k_error_noaccess}),
                  this->run([&] { return syscalls::handle_NtUserConvertMemHandle(this->context(), no_access, 16); }));
        EXPECT_EQ(STATUS_ACCESS_VIOLATION,
                  syscalls::handle_NtUserCreateLocalMemHandle(this->context(), memory_handle, read_only, 16, {this->emu->memory, 0}));
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, syscalls::handle_NtUserCreateLocalMemHandle(
                                               this->context(), memory_handle, this->scratch + 0xA100, 16, {this->emu->memory, read_only}));
        ASSERT_EQ(1u, this->close());

        const auto convert = [&](const std::vector<std::byte>& bytes) {
            this->emu->memory.write_memory(this->scratch + 0xB000, bytes.data(), bytes.size());
            return syscalls::handle_NtUserConvertMemHandle(this->context(), this->scratch + 0xB000, static_cast<uint32_t>(bytes.size()));
        };
        const auto make_dib = [](const uint16_t bit_count, const uint32_t compression, const std::vector<std::byte>& tail,
                                 const uint32_t size_image = 0, const uint32_t colors_used = 0) {
            EMU_BITMAPINFOHEADER header{};
            header.biSize = sizeof(header);
            header.biWidth = 1;
            header.biHeight = 1;
            header.biPlanes = 1;
            header.biBitCount = bit_count;
            header.biCompression = compression;
            header.biSizeImage = size_image;
            header.biClrUsed = colors_used;
            std::vector<std::byte> bytes(sizeof(header) + tail.size());
            std::memcpy(bytes.data(), &header, sizeof(header));
            std::ranges::copy(tail, bytes.begin() + sizeof(header));
            return bytes;
        };
        const auto contents_of = [&](const uint64_t handle) {
            const auto* blob = c.clipboard.memory(handle);
            return blob ? *blob : std::vector<std::byte>{};
        };
        const auto publish = [&](const std::vector<std::byte>& bytes) {
            ASSERT_EQ(1u, this->open(0));
            ASSERT_EQ(1u, this->empty());
            ASSERT_EQ(1u, this->set(cf_dib, convert(bytes)));
            ASSERT_EQ(1u, this->close());
            ASSERT_EQ(1u, this->open(0));
        };
        const std::vector<std::byte> pixel_a(4, std::byte{0xAA});
        const std::vector<std::byte> pixel_b(4, std::byte{0xBB});

        publish(make_dib(32, 0, pixel_a));
        ASSERT_EQ(1u, this->set(cf_dib, convert(make_dib(32, 0, pixel_b))));
        const auto replaced = get(cf_dibv5);
        EXPECT_EQ(k_untouched, replaced.error);
        const auto replaced_v5 = contents_of(replaced.value);
        ASSERT_EQ(128u, replaced_v5.size());
        EXPECT_EQ(pixel_b, (std::vector<std::byte>{replaced_v5.end() - 4, replaced_v5.end()}));
        ASSERT_EQ(1u, this->set(cf_dib, convert(make_dib(32, 0, std::vector<std::byte>(4, std::byte{0xCC})))));
        const auto reread = get(cf_dibv5);
        EXPECT_EQ(replaced.value, reread.value);
        EXPECT_EQ(replaced_v5, contents_of(reread.value));
        ASSERT_EQ(1u, this->close());

        ASSERT_EQ(1u, this->open(0));
        ASSERT_EQ(1u, this->empty());
        ASSERT_EQ(1u, this->set(cf_bitmap, 0x1234, false));
        ASSERT_EQ(1u, this->close());
        ASSERT_EQ(1u, this->open(0));
        ASSERT_EQ(1u, this->set(cf_dib, convert(make_dib(32, 0, pixel_a))));
        ASSERT_EQ(1u, this->close());
        ASSERT_EQ(1u, this->open(0));
        const auto late = get(cf_dibv5);
        EXPECT_NE(0u, late.value);
        EXPECT_EQ(cf_dibv5, request.read().format);
        ASSERT_EQ(1u, this->close());

        publish(make_dib(32, 0, pixel_a));
        const auto v5_handle = get(cf_dibv5).value;
        ASSERT_NE(0u, v5_handle);
        EXPECT_EQ((std::pair{outcome{v5_handle, k_untouched}, cf_dibv5}), reported_format(cf_bitmap));
        ASSERT_EQ(1u, this->close());

        const std::vector<std::byte> masks{std::byte{0}, std::byte{0xF8}, std::byte{0},    std::byte{0}, std::byte{0xE0}, std::byte{7},
                                           std::byte{0}, std::byte{0},    std::byte{0x1F}, std::byte{0}, std::byte{0},    std::byte{0}};
        std::vector<std::byte> bitfield_tail = masks;
        bitfield_tail.insert(bitfield_tail.end(), 4, std::byte{0x5A});
        publish(make_dib(16, 3, bitfield_tail));
        const auto bitfield_v5 = contents_of(get(cf_dibv5).value);
        ASSERT_EQ(140u, bitfield_v5.size());
        EXPECT_TRUE(std::ranges::equal(masks, std::span{bitfield_v5}.subspan(40, masks.size())));
        EXPECT_TRUE(std::ranges::equal(bitfield_tail, std::span{bitfield_v5}.subspan(124)));
        ASSERT_EQ(1u, this->close());

        for (const uint32_t colors_used : {0u, 2u})
        {
            std::vector<std::byte> rle_tail(1024 + 6, std::byte{0x11});
            publish(make_dib(8, 1, rle_tail, 6, colors_used));
            EXPECT_EQ(1152u, contents_of(get(cf_dibv5).value).size()) << colors_used;
            ASSERT_EQ(1u, this->close());
        }

        for (const uint32_t compression : {4u, 5u})
        {
            publish(make_dib(0, compression, {}));
            EXPECT_EQ(124u, contents_of(get(cf_dibv5).value).size()) << compression;
            ASSERT_EQ(1u, this->close());
        }

        publish(make_dib(32, 0, {}));
        EXPECT_EQ((outcome{0, k_error_not_found}), get(cf_dibv5));
        ASSERT_EQ(1u, this->close());

        EMU_BITMAPINFOHEADER dib_header{};
        dib_header.biSize = sizeof(dib_header);
        dib_header.biWidth = 1;
        dib_header.biHeight = 1;
        dib_header.biPlanes = 1;
        dib_header.biBitCount = 32;
        dib_header.biSizeImage = 4;
        std::vector<std::byte> dib(sizeof(dib_header) + 4, std::byte{0x7F});
        std::memcpy(dib.data(), &dib_header, sizeof(dib_header));
        this->emu->memory.write_memory(this->scratch + 0xA200, dib.data(), dib.size());
        const auto dib_handle =
            syscalls::handle_NtUserConvertMemHandle(this->context(), this->scratch + 0xA200, static_cast<uint32_t>(dib.size()));
        ASSERT_NE(0u, dib_handle);

        ASSERT_EQ(1u, this->open(0));
        ASSERT_EQ(1u, this->empty());
        ASSERT_EQ(1u, this->set(cf_dib, dib_handle));
        ASSERT_EQ(1u, this->close());
        ASSERT_EQ(1u, this->open(0));
        const auto converted = syscalls::handle_NtUserGetClipboardData(this->context(), cf_dibv5, request);
        ASSERT_NE(0u, converted);
        ASSERT_NE(dib_handle, converted);
        EXPECT_EQ(cf_dibv5, request.read().format);
        EXPECT_NE(0, request.read().global_handle);

        constexpr size_t v5_header_size = 124;
        std::vector<std::byte> v5(dib.size() - sizeof(dib_header) + v5_header_size);
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtUserCreateLocalMemHandle(this->context(), converted, this->scratch + 0xA300,
                                                                              static_cast<uint32_t>(v5.size()), {this->emu->memory, 0}));
        this->emu->memory.read_memory(this->scratch + 0xA300, v5.data(), v5.size());
        const auto field = [&](const size_t offset) {
            uint32_t value{};
            std::memcpy(&value, v5.data() + offset, sizeof(value));
            return value;
        };
        EXPECT_EQ(v5_header_size, field(0));
        EXPECT_EQ(1u, field(4));
        EXPECT_EQ(1u, field(8));
        EXPECT_EQ(32u, field(12) >> 16);
        EXPECT_EQ(0x73524742u, field(56));
        EXPECT_EQ(0x7F7F7F7Fu, field(v5_header_size));
        ASSERT_EQ(1u, this->close());

        utils::buffer_serializer serializer{};
        c.clipboard.serialize(serializer);
        clipboard_state restored{};
        utils::buffer_deserializer deserializer{serializer.get_buffer()};
        restored.deserialize(deserializer);

        EXPECT_EQ(c.clipboard.sequence_number, restored.sequence_number);
        EXPECT_EQ(c.clipboard.owner, restored.owner);
        EXPECT_EQ(c.clipboard.entries.size(), restored.entries.size());
        ASSERT_NE(nullptr, restored.memory(dib_handle));
        EXPECT_EQ(dib, *restored.memory(dib_handle));
        ASSERT_NE(nullptr, restored.memory(converted));
        EXPECT_EQ(v5, *restored.memory(converted));
    }
}
