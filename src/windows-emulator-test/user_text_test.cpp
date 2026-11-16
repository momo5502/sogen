#include <gtest/gtest.h>
#include <backend_selection.hpp>
#include <syscall_dispatcher.hpp>
#include <syscall_utils.hpp>

#include "code_page_test_utils.hpp"

namespace sogen::syscalls
{
    uint64_t handle_NtUserMessageCall(const syscall_context& c, hwnd window, UINT message, uint64_t w_param, uint64_t l_param,
                                      uint64_t result_info, DWORD type, BOOL ansi);
    uint64_t completion_NtUserMessageCall(const syscall_context& c, hwnd window, UINT message, uint64_t w_param, uint64_t l_param,
                                          uint64_t result_info, DWORD type, BOOL ansi);
    BOOL handle_NtUserDefSetText(const syscall_context& c, hwnd window, emulator_object<LARGE_STRING> text);
    NTSTATUS handle_NtGetNlsSectionPtr(const syscall_context& c, ULONG section_type, ULONG section_data, emulator_pointer context_data,
                                       emulator_object<uint64_t> section_pointer, emulator_object<ULONG> section_size);
}

namespace sogen::test
{
    namespace
    {
        struct text_callback_args
        {
            uint64_t window;
            uint32_t message;
            uint64_t w_param;
            uint64_t l_param;
            uint64_t extra_param;
            uint64_t procedure;
        };

        class UserTextTest : public testing::Test
        {
          protected:
            std::unique_ptr<windows_emulator> make_emulator()
            {
                emulator_interfaces interfaces{};
                interfaces.ui = std::make_unique<null_ui_backend>();
                interfaces.audio = std::make_unique<null_audio_backend>();
                interfaces.console = std::make_unique<null_console_backend>();
                return std::make_unique<windows_emulator>(
                    create_x86_64_emulator_from_environment(),
                    emulator_settings{.disable_logging = true, .use_relative_time = true, .load_registry = false}, emulator_callbacks{},
                    std::move(interfaces));
            }

            void SetUp() override
            {
                this->emu = this->make_emulator();
                const auto base = this->emu->memory.allocate_memory(0x10000, memory_permission::read_write);
                this->emu->process.base_allocator = emulator_allocator{this->emu->memory, base, 0x10000};
                this->emu->process.ansi_code_page = code_page{make_dbcs_code_page()};
                this->emu->process.oem_code_page = this->emu->process.ansi_code_page;
                this->emu->process.user_handles.setup(false);
                const auto thread =
                    this->emu->process.threads.store(emulator_thread{this->emu->memory, this->emu->process, 0, 0, 0x100000, 0, 8, true});
                this->emu->vcpu(0).active_thread = this->emu->process.threads.get(thread);
                const auto stack = this->emu->memory.allocate_memory(0x10000, memory_permission::read_write);
                this->context().x64_cpu().reg(x86_register::rsp, stack + 0x10000);
                this->code = this->emu->memory.allocate_memory(0x1000, memory_permission::read_exec);
                this->context().x64_cpu().reg(x86_register::rip, this->code);
                auto [handle, win] = this->emu->process.windows.create(this->emu->memory);
                this->window_handle = handle.bits;
                win.handle = handle.bits;
                win.thread_id = this->emu->vcpu(0).active_thread->id;
                win.width = 100;
                win.height = 50;
                win.name = u"\x4E2D\x6587";
            }

            syscall_context context()
            {
                return {.win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
            }

            window& guest_window()
            {
                return *this->emu->process.windows.get(this->window_handle);
            }

            uint64_t allocate_buffer()
            {
                return this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);
            }

            template <typename Char>
            uint64_t store_text(const std::basic_string_view<Char> text)
            {
                const auto address = this->allocate_buffer();
                this->emu->memory.write_memory(address, text.data(), text.size() * sizeof(Char));
                this->emu->memory.write_memory(address + text.size() * sizeof(Char), Char{});
                return address;
            }

            uint64_t call_message(const UINT message, const uint64_t capacity, const uint64_t buffer, const BOOL ansi,
                                  const DWORD type = FNID_SENDMESSAGE, const uint64_t result_info = 0)
            {
                return syscalls::handle_NtUserMessageCall(this->context(), this->window_handle, message, capacity, buffer, result_info,
                                                          type, ansi);
            }

            text_callback_args callback_arguments()
            {
                const auto rsp = this->context().x64_cpu().read_stack_pointer();
                return this->emu->memory.read_memory<text_callback_args>(rsp + 0x30);
            }

            callback_frame finish_callback()
            {
                auto& callbacks = this->emu->vcpu(0).active_thread->callback_stack;
                auto frame = std::move(callbacks.back());
                callbacks.pop_back();
                frame.restore_registers(this->emu->vcpu(0).cpu);
                return frame;
            }

            uint64_t complete_message(callback_frame& frame, const UINT message, const uint64_t capacity, const uint64_t buffer,
                                      const BOOL ansi, const uint64_t result, const DWORD type = FNID_SENDMESSAGE,
                                      const uint64_t result_info = 0)
            {
                auto c = this->context();
                c.is_callback_completion = true;
                c.current_completion_state = frame.state.get();
                c.previous_callback_result.value = result;
                return syscalls::completion_NtUserMessageCall(c, this->window_handle, message, capacity, buffer, result_info, type, ansi);
            }

            void use_custom_procedure(const bool unicode)
            {
                this->guest_window().wnd_proc = this->code;
                this->guest_window().unicode_proc = unicode;
            }

            std::unique_ptr<windows_emulator> emu{};
            hwnd window_handle{};
            uint64_t code{};
        };
    }

    TEST_F(UserTextTest, LargeStringUsesDeclaredAnsiEncodingWithoutByteSniffing)
    {
        const auto bytes = this->store_text<char>("\xD6\xD0\xCE\xC4");
        LARGE_STRING value{};
        value.Length = 4;
        value.MaximumLength = 5;
        value.bAnsi = 1;
        value.Buffer = bytes;
        const auto address = this->allocate_buffer();
        this->emu->memory.write_memory(address, value);
        const emulator_object<LARGE_STRING> text{this->emu->memory, address};
        EXPECT_EQ(read_large_string(text, this->emu->process.ansi_code_page), u"\x4E2D\x6587");
        EXPECT_TRUE(syscalls::handle_NtUserDefSetText(this->context(), this->window_handle, text));
        EXPECT_EQ(this->guest_window().name, u"\x4E2D\x6587");
    }

    TEST_F(UserTextTest, AnsiClassNameUsesEncodedByteLength)
    {
        const auto address = this->emu->process.allocate_user_class(this->emu->memory, u"\x4E2D\x6587", 0x40);
        const auto cls = this->emu->memory.read_memory<USER_CLASS>(address);
        EXPECT_EQ(read_string<char>(this->emu->memory, cls.lpszAnsiClassName), "\xD6\xD0\xCE\xC4");
        EXPECT_GE(cls.lpszAnsiClassName, address + process_context::user_class_extra_offset + 0x40);
    }

    TEST_F(UserTextTest, DefaultAnsiOutputDoesNotSplitDoubleByteCharacters)
    {
        const auto address = this->allocate_buffer();
        for (const auto capacity : {0u, 1u, 2u, 3u, 4u, 5u})
        {
            const std::array<char, 8> initial{'#', '#', '#', '#', '#', '#', '#', '#'};
            this->emu->memory.write_memory(address, initial);
            const auto result = this->call_message(WM_GETTEXT, capacity, address, TRUE);
            const auto expected_length = capacity >= 5 ? 4u : capacity >= 3 ? 2u : 0u;
            EXPECT_EQ(result, expected_length);
            if (capacity != 0)
            {
                EXPECT_EQ(this->emu->memory.read_memory<char>(address + result), '\0');
                EXPECT_EQ(read_string<char>(this->emu->memory, address, static_cast<size_t>(result)),
                          std::string("\xD6\xD0\xCE\xC4").substr(0, static_cast<size_t>(result)));
            }
            EXPECT_EQ(this->emu->memory.read_memory<char>(address + capacity), '#');
        }
        EXPECT_EQ(this->call_message(WM_GETTEXTLENGTH, 0, 0, TRUE), 4);
        EXPECT_EQ(this->call_message(WM_GETTEXTLENGTH, 0, 0, FALSE), 2);
    }

    TEST_F(UserTextTest, ConvertsSetTextToTargetProcedureEncoding)
    {
        this->use_custom_procedure(true);
        const auto ansi = this->store_text<char>("\xD6\xD0\xCE\xC4");
        (void)this->call_message(WM_SETTEXT, 0, ansi, TRUE);
        auto args = this->callback_arguments();
        EXPECT_EQ(read_string<char16_t>(this->emu->memory, args.l_param), u"\x4E2D\x6587");
        auto frame = this->finish_callback();
        EXPECT_EQ(this->complete_message(frame, WM_SETTEXT, 0, ansi, TRUE, TRUE), TRUE);

        this->use_custom_procedure(false);
        const auto wide = this->store_text<char16_t>(u"\x4E2D\x6587");
        (void)this->call_message(WM_SETTEXT, 0, wide, FALSE);
        args = this->callback_arguments();
        EXPECT_EQ(read_string<char>(this->emu->memory, args.l_param), "\xD6\xD0\xCE\xC4");
        frame = this->finish_callback();
        EXPECT_EQ(this->complete_message(frame, WM_SETTEXT, 0, wide, FALSE, TRUE), TRUE);
    }

    TEST_F(UserTextTest, WideCallerGetsEnoughAnsiScratchCapacity)
    {
        this->use_custom_procedure(false);
        const auto output = this->allocate_buffer();
        this->emu->memory.write_memory(output, std::array<char16_t, 4>{u'#', u'#', u'#', u'#'});
        (void)this->call_message(WM_GETTEXT, 3, output, FALSE);
        const auto args = this->callback_arguments();
        EXPECT_EQ(args.w_param, 5);
        this->emu->memory.write_memory(args.l_param, "\xD6\xD0\xCE\xC4", 5);
        auto frame = this->finish_callback();
        EXPECT_EQ(this->complete_message(frame, WM_GETTEXT, 3, output, FALSE, 4), 2);
        EXPECT_EQ(read_string<char16_t>(this->emu->memory, output), u"\x4E2D\x6587");
        EXPECT_EQ(this->emu->memory.read_memory<char16_t>(output + 6), u'#');
    }

    TEST_F(UserTextTest, AnsiCallerReceivesCompleteCharactersAndByteCount)
    {
        this->use_custom_procedure(true);
        const auto output = this->allocate_buffer();
        this->emu->memory.write_memory(output, std::array<char, 5>{'#', '#', '#', '#', '#'});
        (void)this->call_message(WM_GETTEXT, 4, output, TRUE);
        const auto args = this->callback_arguments();
        EXPECT_EQ(args.w_param, 4);
        this->emu->memory.write_memory(args.l_param, u"\x4E2D\x6587", 6);
        auto frame = this->finish_callback();
        EXPECT_EQ(this->complete_message(frame, WM_GETTEXT, 4, output, TRUE, 2), 2);
        EXPECT_EQ(read_string<char>(this->emu->memory, output), "\xD6\xD0");
        EXPECT_EQ(this->emu->memory.read_memory<char>(output + 4), '#');
    }

    TEST_F(UserTextTest, CustomLengthConversionDoesNotUnderestimateCallerCapacity)
    {
        this->use_custom_procedure(true);
        (void)this->call_message(WM_GETTEXTLENGTH, 0, 0, TRUE);
        auto frame = this->finish_callback();
        EXPECT_EQ(this->complete_message(frame, WM_GETTEXTLENGTH, 0, 0, TRUE, 2), 4);

        this->use_custom_procedure(false);
        (void)this->call_message(WM_GETTEXTLENGTH, 0, 0, FALSE);
        frame = this->finish_callback();
        EXPECT_EQ(this->complete_message(frame, WM_GETTEXTLENGTH, 0, 0, FALSE, 4), 4);
    }

    TEST_F(UserTextTest, ResultCallbackReceivesCallerEncodedLength)
    {
        this->use_custom_procedure(true);
        const auto output = this->allocate_buffer();
        const auto info = this->allocate_buffer();
        this->emu->memory.write_memory(info, std::array<uint64_t, 2>{this->code, 123});
        (void)this->call_message(WM_GETTEXT, 5, output, TRUE, FNID_SENDMESSAGECALLBACK, info);
        auto args = this->callback_arguments();
        this->emu->memory.write_memory(args.l_param, u"\x4E2D\x6587", 6);
        auto frame = this->finish_callback();
        EXPECT_EQ(this->complete_message(frame, WM_GETTEXT, 5, output, TRUE, 2, FNID_SENDMESSAGECALLBACK, info), 0);
        args = this->callback_arguments();
        EXPECT_EQ(args.l_param, 4);
        EXPECT_EQ(args.w_param, 123);
        frame = this->finish_callback();
        EXPECT_TRUE(this->complete_message(frame, WM_GETTEXT, 5, output, TRUE, 0, FNID_SENDMESSAGECALLBACK, info));
    }

    TEST_F(UserTextTest, RejectsOverflowingTextCapacityBeforeDispatch)
    {
        this->use_custom_procedure(false);
        const auto output = this->allocate_buffer();
        auto c = this->context();
        EXPECT_EQ(syscalls::handle_NtUserMessageCall(c, this->window_handle, WM_GETTEXT, std::numeric_limits<uint64_t>::max(), output, 0,
                                                     FNID_SENDMESSAGE, FALSE),
                  0);
        EXPECT_FALSE(c.run_callback);
    }

    TEST_F(UserTextTest, RestoresActiveCodePageAndPendingTextConversion)
    {
        this->use_custom_procedure(false);
        const auto output = this->allocate_buffer();
        (void)this->call_message(WM_GETTEXT, 3, output, FALSE);
        const auto args = this->callback_arguments();
        this->emu->memory.write_memory(args.l_param, "\xD6\xD0\xCE\xC4", 5);
        utils::buffer_serializer saved{};
        this->emu->serialize(saved);
        this->emu.reset();
        this->emu = this->make_emulator();
        utils::buffer_deserializer input{saved};
        this->emu->deserialize(input);
        EXPECT_EQ(this->emu->process.ansi_code_page.id(), 936);
        auto frame = this->finish_callback();
        EXPECT_EQ(this->complete_message(frame, WM_GETTEXT, 3, output, FALSE, 4), 2);
        EXPECT_EQ(read_string<char16_t>(this->emu->memory, output), u"\x4E2D\x6587");
    }

    TEST_F(UserTextTest, EqualDbcsAnsiAndOemCodePagesPreserveByteTableInputs)
    {
        initialize_user_code_page_tables(*this->emu);
        const auto info = this->emu->process.user_handles.get_server_info().read();
        for (size_t i = 0; i < 256; ++i)
        {
            EXPECT_EQ(info.ansiToOem[i], i);
            EXPECT_EQ(info.oemToAnsi[i], i);
        }
    }

    TEST_F(UserTextTest, NlsSectionUsesFrozenDataAndWow64ReachableAddresses)
    {
        this->emu->process.is_wow64_process = true;
        const auto pointer = this->allocate_buffer();
        const auto size = pointer + 8;
        EXPECT_EQ(syscalls::handle_NtGetNlsSectionPtr(this->context(), 11, 936, 0, {this->emu->memory, pointer}, {this->emu->memory, size}),
                  STATUS_SUCCESS);
        const auto address = this->emu->memory.read_memory<uint64_t>(pointer);
        const auto length = this->emu->memory.read_memory<ULONG>(size);
        EXPECT_LT(address, uint64_t{0x100000000});
        EXPECT_LE(address + length, uint64_t{0x100000000});
        const auto expected = make_dbcs_code_page();
        std::vector<std::byte> actual(expected.size());
        this->emu->memory.read_memory(address, actual.data(), actual.size());
        EXPECT_EQ(actual, expected);
    }
}
