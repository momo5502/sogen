#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtQueryObject(const syscall_context& c, handle handle, OBJECT_INFORMATION_CLASS object_information_class,
                                  emulator_pointer object_information, ULONG object_information_length,
                                  emulator_object<ULONG> return_length);
    NTSTATUS handle_NtOpenSymbolicLinkObject(const syscall_context& c, emulator_object<handle> link_handle, ACCESS_MASK desired_access,
                                             emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes);
}

namespace sogen::test
{
    namespace
    {
        constexpr ULONG untouched = 0xAAAAAAAA;
        constexpr ULONG header_size = sizeof(UNICODE_STRING<EmulatorTraits<Emu64>>);
        constexpr size_t poisoned_size = 0x100;

        class ObjectNameInformationTest : public testing::Test
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
                this->storage = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);
            }

            syscall_context context()
            {
                return {.win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
            }

            uint64_t information() const
            {
                return this->storage + 0x100;
            }

            emulator_object<ULONG> return_length() const
            {
                return {this->emu->memory, this->storage};
            }

            NTSTATUS query(const handle h, const ULONG length, const bool with_return_length = true)
            {
                std::array<uint8_t, poisoned_size> poison{};
                poison.fill(0xAA);
                this->emu->memory.write_memory(this->information(), poison.data(), poison.size());
                this->return_length().write(untouched);

                const auto returned = with_return_length ? this->return_length() : emulator_object<ULONG>{this->emu->memory};
                return syscalls::handle_NtQueryObject(this->context(), h, ObjectNameInformation, this->information(), length, returned);
            }

            bool information_untouched_from(const size_t offset) const
            {
                std::array<uint8_t, poisoned_size> bytes{};
                this->emu->memory.read_memory(this->information(), bytes.data(), bytes.size());
                return std::ranges::all_of(bytes.begin() + static_cast<ptrdiff_t>(offset), bytes.end(),
                                           [](const uint8_t b) { return b == 0xAA; });
            }

            void expect_mismatch(const handle h, const ULONG length, const ULONG required, const bool with_return_length = true)
            {
                EXPECT_EQ(STATUS_INFO_LENGTH_MISMATCH, this->query(h, length, with_return_length)) << length;
                EXPECT_EQ(with_return_length ? required : untouched, this->return_length().read()) << length;
                EXPECT_TRUE(this->information_untouched_from(0)) << length;
            }

            void expect_invalid_handle(const handle h, const ULONG length)
            {
                EXPECT_EQ(STATUS_INVALID_HANDLE, this->query(h, length)) << length;
                EXPECT_EQ(untouched, this->return_length().read()) << length;
                EXPECT_TRUE(this->information_untouched_from(0)) << length;
            }

            void expect_empty_name(const handle h, const ULONG length)
            {
                ASSERT_EQ(STATUS_SUCCESS, this->query(h, length)) << length;
                EXPECT_EQ(header_size, this->return_length().read()) << length;

                const auto result = emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>>{this->emu->memory, this->information()}.read();
                EXPECT_EQ(0u, result.Length) << length;
                EXPECT_EQ(0u, result.MaximumLength) << length;
                EXPECT_EQ(0u, result.Buffer) << length;
                EXPECT_TRUE(this->information_untouched_from(header_size)) << length;
            }

            void expect_name(const handle h, const ULONG length, const std::u16string_view name, const ULONG required,
                             const bool with_return_length = true)
            {
                ASSERT_EQ(STATUS_SUCCESS, this->query(h, length, with_return_length)) << length;
                EXPECT_EQ(with_return_length ? required : untouched, this->return_length().read()) << length;

                const auto result = emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>>{this->emu->memory, this->information()}.read();
                EXPECT_EQ(name.size() * sizeof(char16_t), result.Length) << length;
                EXPECT_EQ(result.Length + sizeof(char16_t), result.MaximumLength) << length;
                EXPECT_EQ(this->information() + header_size, result.Buffer) << length;
                EXPECT_EQ(required, header_size + result.MaximumLength) << length;

                std::u16string written(name.size() + 1, u'?');
                this->emu->memory.read_memory(result.Buffer, written.data(), written.size() * sizeof(char16_t));
                EXPECT_EQ(name, std::u16string_view(written).substr(0, name.size())) << length;
                EXPECT_EQ(u'\0', written.back()) << length;
            }

            handle open_symbolic_link(const std::u16string_view name)
            {
                emulator_allocator allocator{this->emu->memory, this->storage + 0x400, 0x400};
                const auto string = allocator.make_unicode_string(name);
                const auto attributes = allocator.reserve<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>>();
                OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>> values{};
                values.Length = sizeof(values);
                values.ObjectName = string.value();
                attributes.write(values);

                const emulator_object<handle> link{this->emu->memory, this->storage + 0x800};
                EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtOpenSymbolicLinkObject(this->context(), link, 0, attributes));
                return link.read();
            }

            std::unique_ptr<windows_emulator> emu{};
            uint64_t storage{};
        };

        template <typename T>
        T named(std::u16string name)
        {
            T object{};
            object.name = std::move(name);
            return object;
        }

        section file_section(std::u16string name)
        {
            auto object = named<section>(std::move(name));
            object.file_name = u"C:\\scratch\\section.bin";
            return object;
        }
    }

    TEST_F(ObjectNameInformationTest, UnnamedObjectsAnswerAnEmptyName)
    {
        auto& proc = this->emu->process;
        for (const auto h : {proc.events.store(event{}), proc.mutants.store(mutant{}), proc.semaphores.store(semaphore{}),
                             proc.timers.store(timer{}), proc.sections.store(section{}), proc.sections.store(file_section({})),
                             CURRENT_PROCESS_TOKEN, DUMMY_IMPERSONATION_TOKEN, ANONYMOUS_IMPERSONATION_TOKEN})
        {
            SCOPED_TRACE(h.bits);
            for (const ULONG length : {0x0u, 0x4u, 0xFu})
            {
                this->expect_mismatch(h, length, header_size);
            }

            this->expect_mismatch(h, 0x4, header_size, false);
            this->expect_empty_name(h, header_size);
            this->expect_empty_name(h, header_size + 0x10);
        }
    }

    TEST_F(ObjectNameInformationTest, NamedObjectsAnswerTheirNameWithTheExactRequiredLength)
    {
        auto& proc = this->emu->process;
        const std::array<std::tuple<handle, std::u16string_view, ULONG>, 8> cases{{
            {proc.events.store(named<event>(u"\\BaseNamedObjects\\SogenV4ObjNameProbe\\Event")),
             u"\\BaseNamedObjects\\SogenV4ObjNameProbe\\Event", 0x68},
            {proc.events.store(named<event>(u"\\BaseNamedObjects\\SogenV4ObjNameEvent")), u"\\BaseNamedObjects\\SogenV4ObjNameEvent", 0x5C},
            {proc.events.store(named<event>(u"\\KernelObjects\\LowMemoryCondition")), u"\\KernelObjects\\LowMemoryCondition", 0x54},
            {proc.mutants.store(named<mutant>(u"\\BaseNamedObjects\\SogenV4ObjNameProbe\\Mutant")),
             u"\\BaseNamedObjects\\SogenV4ObjNameProbe\\Mutant", 0x6A},
            {proc.semaphores.store(named<semaphore>(u"\\BaseNamedObjects\\SogenV4ObjNameProbe\\Semaphore")),
             u"\\BaseNamedObjects\\SogenV4ObjNameProbe\\Semaphore", 0x70},
            {proc.timers.store(named<timer>(u"\\BaseNamedObjects\\SogenV4ObjNameProbe\\Timer")),
             u"\\BaseNamedObjects\\SogenV4ObjNameProbe\\Timer", 0x68},
            {proc.sections.store(named<section>(u"\\BaseNamedObjects\\SogenV4ObjNameProbe\\Section")),
             u"\\BaseNamedObjects\\SogenV4ObjNameProbe\\Section", 0x6C},
            {proc.sections.store(file_section(u"\\BaseNamedObjects\\SogenV4ObjNameProbe\\FileSection")),
             u"\\BaseNamedObjects\\SogenV4ObjNameProbe\\FileSection", 0x74},
        }};

        for (const auto& [h, name, required] : cases)
        {
            SCOPED_TRACE(u16_to_u8(name));
            for (const auto length : std::array<ULONG, 5>{0x0, 0x4, 0xF, header_size, required - 2})
            {
                this->expect_mismatch(h, length, required);
            }

            this->expect_mismatch(h, 0x4, required, false);
            this->expect_name(h, required, name, required);
            this->expect_name(h, required + 0x10, name, required);
            this->expect_name(h, required, name, required, false);
        }
    }

    TEST_F(ObjectNameInformationTest, KnownDllPathSymbolicLinksAnswerTheirObjectPath)
    {
        this->expect_mismatch(KNOWN_DLLS_SYMLINK, header_size, 0x40);
        this->expect_name(KNOWN_DLLS_SYMLINK, 0x40, u"\\KnownDlls\\KnownDllPath", 0x40);
        this->expect_mismatch(KNOWN_DLLS32_SYMLINK, header_size, 0x44);
        this->expect_name(KNOWN_DLLS32_SYMLINK, 0x44, u"\\KnownDlls32\\KnownDllPath", 0x44);
    }

    TEST_F(ObjectNameInformationTest, DriveLetterSymbolicLinksAnswerTheirGlobalPath)
    {
        constexpr ULONG required = 0x2A;
        for (const auto name : std::array<std::u16string_view, 2>{u"\\??\\C:", u"\\GLOBAL??\\C:"})
        {
            SCOPED_TRACE(u16_to_u8(name));
            const auto link = this->open_symbolic_link(name);
            for (const auto length : std::array<ULONG, 3>{0x0, 0x4, header_size})
            {
                this->expect_mismatch(link, length, required);
            }

            this->expect_name(link, 0x200, u"\\GLOBAL??\\C:", required);
        }
    }

    TEST_F(ObjectNameInformationTest, ThreadTokenPseudoHandlesAreInvalidHandles)
    {
        for (const auto h : {CURRENT_THREAD_TOKEN, CURRENT_THREAD_EFFECTIVE_TOKEN})
        {
            SCOPED_TRACE(h.bits);
            this->expect_invalid_handle(h, 0x0);
            this->expect_invalid_handle(h, header_size);
        }
    }

    TEST_F(ObjectNameInformationTest, ClosedOrUnknownHandlesAreInvalidHandles)
    {
        auto& proc = this->emu->process;
        const auto closed_event = proc.events.store(event{});
        ASSERT_TRUE(proc.events.erase(closed_event));

        for (const auto h :
             {closed_event, proc.mutants.make_handle(99), proc.semaphores.make_handle(99), proc.timers.make_handle(99),
              proc.sections.make_handle(99), make_pseudo_handle(0x7, handle_types::token), make_pseudo_handle(0x7, handle_types::symlink)})
        {
            SCOPED_TRACE(h.bits);
            this->expect_invalid_handle(h, static_cast<ULONG>(poisoned_size));
        }
    }

    TEST_F(ObjectNameInformationTest, ShortBuffersAreLengthMismatchesForTheKnownDllsDirectory)
    {
        for (const auto length : std::array<ULONG, 2>{0x0, header_size})
        {
            this->expect_mismatch(KNOWN_DLLS_DIRECTORY, length, 0x26);
        }

        this->expect_name(KNOWN_DLLS_DIRECTORY, 0x26, u"\\KnownDlls", 0x26);
    }
}
