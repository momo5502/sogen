#include "emulation_test_utils.hpp"

#include <registry/registry_file.hpp>
#include <syscall_utils.hpp>

#include <atomic>
#include <fstream>

namespace sogen::syscalls
{
    NTSTATUS handle_NtQueryLicenseValue(const syscall_context& c, emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> value_name,
                                        emulator_object<uint32_t> type, uint64_t data, uint64_t data_size,
                                        emulator_object<uint32_t> result_data_size);
    NTSTATUS handle_NtOpenKey(const syscall_context& c, emulator_object<handle> key_handle, ACCESS_MASK desired_access,
                              emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes);
    NTSTATUS handle_NtCreateKey(const syscall_context& c, emulator_object<handle> key_handle, ACCESS_MASK desired_access,
                                emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes, ULONG title_index,
                                emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> class_name, ULONG create_options,
                                emulator_object<ULONG> disposition);
    NTSTATUS handle_NtQueryKey(const syscall_context& c, handle key_handle, KEY_INFORMATION_CLASS key_information_class,
                               uint64_t key_information, ULONG length, emulator_object<ULONG> result_length);
    NTSTATUS handle_NtQueryValueKey(const syscall_context& c, handle key_handle,
                                    emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> value_name,
                                    KEY_VALUE_INFORMATION_CLASS key_value_information_class, uint64_t key_value_information, ULONG length,
                                    emulator_object<ULONG> result_length);
    NTSTATUS handle_NtSetValueKey(const syscall_context& c, handle key_handle,
                                  emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> value_name, ULONG title_index, ULONG type,
                                  uint64_t data, ULONG data_size);
    NTSTATUS handle_NtDeleteValueKey(const syscall_context& c, handle key_handle,
                                     emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> value_name);
    NTSTATUS handle_NtDeleteKey(const syscall_context& c, handle key_handle);
    NTSTATUS handle_NtEnumerateKey(const syscall_context& c, handle key_handle, ULONG index, KEY_INFORMATION_CLASS key_information_class,
                                   uint64_t key_information, ULONG length, emulator_object<ULONG> result_length);
    NTSTATUS handle_NtEnumerateValueKey(const syscall_context& c, handle key_handle, ULONG index,
                                        KEY_VALUE_INFORMATION_CLASS key_value_information_class, uint64_t key_value_information,
                                        ULONG length, emulator_object<ULONG> result_length);
    NTSTATUS handle_NtQueryMultipleValueKey(const syscall_context& c, handle key_handle, emulator_object<KEY_VALUE_ENTRY> value_entries,
                                            ULONG entry_count, uint64_t value_buffer, emulator_object<ULONG> buffer_length,
                                            emulator_object<ULONG> required_buffer_length);
    NTSTATUS handle_NtSetInformationKey(const syscall_context& c, handle key_handle, KEY_SET_INFORMATION_CLASS key_information_class,
                                        uint64_t key_information, ULONG length);
    NTSTATUS handle_NtQueryObject(const syscall_context& c, handle handle, OBJECT_INFORMATION_CLASS object_information_class,
                                  emulator_pointer object_information, ULONG object_information_length,
                                  emulator_object<ULONG> return_length);
}

namespace sogen::test
{
    namespace
    {
        constexpr size_t hive_data_offset = 0x1000;
        constexpr size_t root_key_offset = hive_data_offset + 0x20;

        void write_uint16(std::vector<std::byte>& data, const size_t offset, const uint16_t value)
        {
            data[offset] = static_cast<std::byte>(value & 0xFF);
            data[offset + 1] = static_cast<std::byte>(value >> 8);
        }

        void write_uint32(std::vector<std::byte>& data, const size_t offset, const uint32_t value)
        {
            data[offset] = static_cast<std::byte>(value & 0xFF);
            data[offset + 1] = static_cast<std::byte>((value >> 8) & 0xFF);
            data[offset + 2] = static_cast<std::byte>((value >> 16) & 0xFF);
            data[offset + 3] = static_cast<std::byte>((value >> 24) & 0xFF);
        }

        void write_key(std::vector<std::byte>& data, const size_t offset, const std::string_view name, const uint32_t subkey_list_offset)
        {
            write_uint32(data, offset + 32, subkey_list_offset);
            write_uint16(data, offset + 76, static_cast<uint16_t>(name.size()));
            for (size_t i = 0; i < name.size(); ++i)
            {
                data[offset + 80 + i] = static_cast<std::byte>(name[i]);
            }
        }

        void write_subkey_list(std::vector<std::byte>& data, const uint32_t list_offset, const uint32_t key_offset)
        {
            const auto offset = hive_data_offset + list_offset;
            data[offset + 4] = static_cast<std::byte>('l');
            data[offset + 5] = static_cast<std::byte>('f');
            write_uint16(data, offset + 6, 1);
            write_uint32(data, offset + 8, key_offset);
        }

        constexpr size_t big_data_segment_size = 0x3FD8;
        constexpr uint32_t big_data_segment_stride = 0x4000;

        void write_big_value(std::vector<std::byte>& data, const uint32_t value_offset, const std::string_view name,
                             const std::span<const std::byte> payload, const uint32_t db_offset, const uint32_t first_segment_offset)
        {
            const auto value = hive_data_offset + value_offset;
            data[value + 4] = static_cast<std::byte>('v');
            data[value + 5] = static_cast<std::byte>('k');
            write_uint16(data, value + 6, static_cast<uint16_t>(name.size()));
            write_uint32(data, value + 8, static_cast<uint32_t>(payload.size()));
            write_uint32(data, value + 12, db_offset);
            write_uint32(data, value + 16, REG_BINARY);
            for (size_t i = 0; i < name.size(); ++i)
            {
                data[value + 24 + i] = static_cast<std::byte>(name[i]);
            }

            const auto segment_count = (payload.size() + big_data_segment_size - 1) / big_data_segment_size;
            const uint32_t segment_list_offset = db_offset + 0x10;

            const auto db = hive_data_offset + db_offset;
            data[db + 4] = static_cast<std::byte>('d');
            data[db + 5] = static_cast<std::byte>('b');
            write_uint16(data, db + 6, static_cast<uint16_t>(segment_count));
            write_uint32(data, db + 8, segment_list_offset);

            for (size_t segment = 0; segment < segment_count; ++segment)
            {
                const auto segment_offset = first_segment_offset + static_cast<uint32_t>(segment * big_data_segment_stride);
                write_uint32(data, hive_data_offset + segment_list_offset + 4 + (segment * 4), segment_offset);

                const auto chunk = payload.subspan(segment * big_data_segment_size,
                                                   std::min(big_data_segment_size, payload.size() - (segment * big_data_segment_size)));
                std::ranges::copy(chunk, data.begin() + static_cast<std::ptrdiff_t>(hive_data_offset + segment_offset + 4));
            }
        }

        std::vector<std::byte> create_pattern(const size_t size)
        {
            std::vector<std::byte> result(size);
            for (size_t i = 0; i < size; ++i)
            {
                result[i] = static_cast<std::byte>(i % 251);
            }
            return result;
        }

        void append_policy_entry(std::vector<std::byte>& policy, const std::u16string_view name, const uint16_t type,
                                 const std::span<const std::byte> value)
        {
            const auto name_size = name.size() * sizeof(char16_t);
            const auto entry_size = 16 + name_size + value.size();
            const auto offset = policy.size();
            policy.resize(offset + entry_size);

            write_uint16(policy, offset, static_cast<uint16_t>(entry_size));
            write_uint16(policy, offset + 2, static_cast<uint16_t>(name_size));
            write_uint16(policy, offset + 4, type);
            write_uint16(policy, offset + 6, static_cast<uint16_t>(value.size()));
            for (size_t i = 0; i < name.size(); ++i)
            {
                write_uint16(policy, offset + 16 + (i * 2), static_cast<uint16_t>(name[i]));
            }
            std::ranges::copy(value, policy.begin() + static_cast<std::ptrdiff_t>(offset + 16 + name_size));
        }

        std::vector<std::byte> create_product_policy()
        {
            constexpr size_t header_size = 20;
            constexpr uint32_t end_marker_size = 4;

            std::vector<std::byte> policy(header_size);
            const auto filler = create_pattern(0x8000);
            for (const auto* name : {u"Filler-1", u"Filler-2", u"Filler-3"})
            {
                append_policy_entry(policy, name, REG_BINARY, filler);
            }
            append_policy_entry(policy, u"Kernel-Test", REG_DWORD,
                                std::vector{std::byte{0x78}, std::byte{0x56}, std::byte{0x34}, std::byte{0x12}});

            const auto data_size = static_cast<uint32_t>(policy.size() - header_size);
            policy.resize(policy.size() + end_marker_size);
            write_uint32(policy, 0, static_cast<uint32_t>(policy.size()));
            write_uint32(policy, 4, data_size);
            write_uint32(policy, 8, end_marker_size);
            return policy;
        }

        std::vector<std::byte> create_system_hive()
        {
            std::vector<std::byte> data(hive_data_offset + 0x28000);
            data[0] = static_cast<std::byte>('r');
            data[1] = static_cast<std::byte>('e');
            data[2] = static_cast<std::byte>('g');
            data[3] = static_cast<std::byte>('f');

            write_key(data, root_key_offset, {}, 0x200);
            write_subkey_list(data, 0x200, 0x400);
            write_key(data, hive_data_offset + 0x400, "ControlSet001", 0x600);
            write_subkey_list(data, 0x600, 0x800);
            write_key(data, hive_data_offset + 0x800, "Control", 0xA00);
            write_subkey_list(data, 0xA00, 0xC00);

            const auto product_options = hive_data_offset + 0xC00;
            write_key(data, product_options, "ProductOptions", 0);
            write_uint32(data, product_options + 40, 2);
            write_uint32(data, product_options + 44, 0xE00);
            write_uint32(data, hive_data_offset + 0xE00 + 4, 0x1000);
            write_uint32(data, hive_data_offset + 0xE00 + 8, 0x1200);

            write_big_value(data, 0x1000, "Pair", create_pattern(big_data_segment_size + 0x100), 0x1400, 0x2000);
            write_big_value(data, 0x1200, "ProductPolicy", create_product_policy(), 0x1500, 0xA000);
            return data;
        }

        void write_dword_value(std::vector<std::byte>& data, const uint32_t value_offset, const std::string_view name, const uint32_t value)
        {
            const auto offset = hive_data_offset + value_offset;
            data[offset + 4] = static_cast<std::byte>('v');
            data[offset + 5] = static_cast<std::byte>('k');
            write_uint16(data, offset + 6, static_cast<uint16_t>(name.size()));
            write_uint32(data, offset + 8, 0x80000000u | static_cast<uint32_t>(sizeof(value)));
            write_uint32(data, offset + 12, value);
            write_uint32(data, offset + 16, REG_DWORD);
            for (size_t i = 0; i < name.size(); ++i)
            {
                data[offset + 24 + i] = static_cast<std::byte>(name[i]);
            }
        }

        void write_value_list(std::vector<std::byte>& data, const size_t key_offset, const uint32_t list_offset,
                              const std::span<const uint32_t> value_offsets)
        {
            write_uint32(data, key_offset + 40, static_cast<uint32_t>(value_offsets.size()));
            write_uint32(data, key_offset + 44, list_offset);
            for (size_t i = 0; i < value_offsets.size(); ++i)
            {
                write_uint32(data, hive_data_offset + list_offset + 4 + (i * 4), value_offsets[i]);
            }
        }

        std::vector<std::byte> create_software_hive()
        {
            std::vector<std::byte> data(hive_data_offset + 0x1800);
            data[0] = static_cast<std::byte>('r');
            data[1] = static_cast<std::byte>('e');
            data[2] = static_cast<std::byte>('g');
            data[3] = static_cast<std::byte>('f');

            write_key(data, root_key_offset, {}, 0x200);
            write_subkey_list(data, 0x200, 0x400);
            write_key(data, hive_data_offset + 0x400, "Microsoft", 0x600);
            write_subkey_list(data, 0x600, 0x800);
            write_key(data, hive_data_offset + 0x800, "Windows NT", 0xA00);
            write_subkey_list(data, 0xA00, 0xC00);
            write_key(data, hive_data_offset + 0xC00, "CurrentVersion", 0xE00);
            write_subkey_list(data, 0xE00, 0x1000);
            write_key(data, hive_data_offset + 0x1000, "ProfileList", 0x1200);
            write_subkey_list(data, 0x1200, 0x1300);
            write_key(data, hive_data_offset + 0x1300, "Profile", 0);

            constexpr std::array<uint32_t, 2> profile_list_values{0x1500, 0x1680};
            write_value_list(data, hive_data_offset + 0x1000, 0x1480, profile_list_values);
            write_dword_value(data, profile_list_values[0], "First", 1);
            write_dword_value(data, profile_list_values[1], "Second", 2);
            return data;
        }

        bool write_hive(const std::filesystem::path& path, const std::span<const std::byte> data)
        {
            std::ofstream file(path, std::ios::binary);
            file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
            return file.good();
        }

        std::vector<std::byte> encode_utf8_with_bom(const std::string_view text)
        {
            std::vector<std::byte> result{std::byte{0xEF}, std::byte{0xBB}, std::byte{0xBF}};
            result.reserve(result.size() + text.size());
            for (const auto character : text)
            {
                result.push_back(static_cast<std::byte>(static_cast<uint8_t>(character)));
            }
            return result;
        }

        std::vector<std::byte> encode_utf16(const std::u16string_view text, const bool big_endian)
        {
            std::vector<std::byte> result{};
            result.reserve(2 + text.size() * 2);
            result.push_back(big_endian ? std::byte{0xFE} : std::byte{0xFF});
            result.push_back(big_endian ? std::byte{0xFF} : std::byte{0xFE});
            for (const auto character : text)
            {
                const auto low = static_cast<std::byte>(character & 0xFF);
                const auto high = static_cast<std::byte>(character >> 8);
                result.push_back(big_endian ? high : low);
                result.push_back(big_endian ? low : high);
            }
            return result;
        }

        class RegistryFileTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                static std::atomic_uint64_t counter{};
                directory_ = std::filesystem::temp_directory_path() /
                             ("sogen-registry-file-test-" + std::to_string(getpid()) + "-" + std::to_string(counter++));
                ASSERT_TRUE(std::filesystem::create_directories(directory_));

                const std::array<std::byte, 4> empty_hive{
                    static_cast<std::byte>('r'),
                    static_cast<std::byte>('e'),
                    static_cast<std::byte>('g'),
                    static_cast<std::byte>('f'),
                };
                for (const auto* name : {"SECURITY", "SAM", "NTUSER.DAT"})
                {
                    ASSERT_TRUE(write_hive(directory_ / name, empty_hive));
                }

                const auto system_hive = create_system_hive();
                ASSERT_TRUE(write_hive(directory_ / "SYSTEM", system_hive));

                const auto software_hive = create_software_hive();
                ASSERT_TRUE(write_hive(directory_ / "SOFTWARE", software_hive));
                registry_.emplace(directory_);
            }

            void TearDown() override
            {
                registry_.reset();
                std::error_code error{};
                std::filesystem::remove_all(directory_, error);
            }

            registry_manager& registry()
            {
                return *registry_;
            }

            registry_key get_key(const std::filesystem::path& path)
            {
                auto key = registry().get_key(utils::path_key{path});
                EXPECT_TRUE(key.has_value());
                return key.value_or(registry_key{});
            }

            const std::filesystem::path& directory() const
            {
                return directory_;
            }

          private:
            std::filesystem::path directory_{};
            std::optional<registry_manager> registry_{};
        };

        class LicenseValueTest : public RegistryFileTest
        {
          protected:
            void SetUp() override
            {
                RegistryFileTest::SetUp();

                emulator_interfaces interfaces{};
                interfaces.audio = std::make_unique<null_audio_backend>();
                interfaces.console = std::make_unique<null_console_backend>();
                interfaces.ui = std::make_unique<null_ui_backend>();
                this->emu = std::make_unique<windows_emulator>(
                    create_x86_64_emulator_from_environment(),
                    emulator_settings{.disable_logging = true, .emulation_root = get_emulator_root(), .load_registry = false},
                    emulator_callbacks{}, std::move(interfaces));
                this->emu->registry = registry_manager{this->directory()};
                this->storage = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);
            }

            void TearDown() override
            {
                this->emu.reset();
                RegistryFileTest::TearDown();
            }

            syscall_context context()
            {
                return {.win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
            }

            std::unique_ptr<windows_emulator> emu{};
            uint64_t storage{};
        };

        class RegistrySyscallTest : public RegistryFileTest
        {
          protected:
            void SetUp() override
            {
                RegistryFileTest::SetUp();

                emulator_interfaces interfaces{};
                interfaces.audio = std::make_unique<null_audio_backend>();
                interfaces.console = std::make_unique<null_console_backend>();
                interfaces.ui = std::make_unique<null_ui_backend>();
                this->emu = std::make_unique<windows_emulator>(
                    create_x86_64_emulator_from_environment(),
                    emulator_settings{.disable_logging = true, .emulation_root = get_emulator_root(), .load_registry = false},
                    emulator_callbacks{}, std::move(interfaces));
                this->emu->registry = registry_manager{this->directory()};

                constexpr uint64_t storage_size = 0x2000;
                const auto storage = this->emu->memory.allocate_memory(storage_size, memory_permission::read_write);
                this->allocator.emplace(this->emu->memory, storage, storage_size);
            }

            void TearDown() override
            {
                this->allocator.reset();
                this->emu.reset();
                RegistryFileTest::TearDown();
            }

            syscall_context context()
            {
                return {.win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
            }

            emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> make_string(const std::u16string_view text)
            {
                return this->allocator->make_unicode_string(text);
            }

            emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> key_attributes(const std::u16string_view path, const handle root)
            {
                const auto name = this->make_string(path);
                const auto attributes = this->allocator->reserve<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>>();
                OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>> values{};
                values.Length = sizeof(values);
                values.RootDirectory = root.bits;
                values.ObjectName = name.value();
                attributes.write(values);
                return attributes;
            }

            NTSTATUS open_key(const std::u16string_view path, handle& key_handle, const handle root = {})
            {
                const auto output = this->allocator->reserve<handle>();
                const auto status = syscalls::handle_NtOpenKey(this->context(), output, 0, this->key_attributes(path, root));
                key_handle = output.read();
                return status;
            }

            NTSTATUS create_key(const std::u16string_view path, handle& key_handle, ULONG& disposition, const handle root = {})
            {
                const auto output = this->allocator->reserve<handle>();
                const auto disposition_output = this->allocator->reserve<ULONG>();
                const auto status = syscalls::handle_NtCreateKey(this->context(), output, 0, this->key_attributes(path, root), 0,
                                                                 {this->emu->memory, uint64_t{0}}, 0, disposition_output);
                key_handle = output.read();
                disposition = disposition_output.read();
                return status;
            }

            NTSTATUS query_value(const handle key_handle, const std::u16string_view name)
            {
                const auto buffer = this->allocator->reserve(0x100, 8);
                const auto result_length = this->allocator->reserve<ULONG>();
                return syscalls::handle_NtQueryValueKey(this->context(), key_handle, this->make_string(name), KeyValuePartialInformation,
                                                        buffer, 0x100, result_length);
            }

            NTSTATUS delete_value(const handle key_handle, const std::u16string_view name)
            {
                return syscalls::handle_NtDeleteValueKey(this->context(), key_handle, this->make_string(name));
            }

            NTSTATUS enumerate_key(const handle key_handle, const ULONG index)
            {
                const auto buffer = this->allocator->reserve(0x100, 8);
                const auto result_length = this->allocator->reserve<ULONG>();
                return syscalls::handle_NtEnumerateKey(this->context(), key_handle, index, KeyBasicInformation, buffer, 0x100,
                                                       result_length);
            }

            NTSTATUS enumerate_value(const handle key_handle, const ULONG index)
            {
                const auto buffer = this->allocator->reserve(0x100, 8);
                const auto result_length = this->allocator->reserve<ULONG>();
                return syscalls::handle_NtEnumerateValueKey(this->context(), key_handle, index, KeyValueBasicInformation, buffer, 0x100,
                                                            result_length);
            }

            NTSTATUS query_key(const handle key_handle, const KEY_INFORMATION_CLASS information_class)
            {
                const auto buffer = this->allocator->reserve(0x100, 8);
                const auto result_length = this->allocator->reserve<ULONG>();
                return syscalls::handle_NtQueryKey(this->context(), key_handle, information_class, buffer, 0x100, result_length);
            }

            NTSTATUS query_multiple_values(const handle key_handle, const std::u16string_view name)
            {
                KEY_VALUE_ENTRY entry{};
                entry.ValueName = this->make_string(name).value();
                const auto entries = this->allocator->reserve<KEY_VALUE_ENTRY>();
                entries.write(entry);

                const auto buffer = this->allocator->reserve(0x100, 8);
                const auto buffer_length = this->allocator->reserve<ULONG>();
                buffer_length.write(0x100);
                return syscalls::handle_NtQueryMultipleValueKey(this->context(), key_handle, entries, 1, buffer, buffer_length,
                                                                {this->emu->memory, uint64_t{0}});
            }

            NTSTATUS query_object_name(const handle key_handle)
            {
                const auto buffer = this->allocator->reserve(0x100, 8);
                const auto return_length = this->allocator->reserve<ULONG>();
                return syscalls::handle_NtQueryObject(this->context(), key_handle, ObjectNameInformation, buffer, 0x100, return_length);
            }

            NTSTATUS set_handle_tags(const handle key_handle, const ULONG tags)
            {
                const auto info = this->allocator->reserve<KEY_HANDLE_TAGS_INFORMATION>();
                info.write(KEY_HANDLE_TAGS_INFORMATION{.HandleTags = tags});
                return syscalls::handle_NtSetInformationKey(this->context(), key_handle, KeySetHandleTagsInformation, info.value(),
                                                            static_cast<ULONG>(sizeof(KEY_HANDLE_TAGS_INFORMATION)));
            }

            std::unique_ptr<windows_emulator> emu{};
            std::optional<emulator_allocator> allocator{};
        };

        constexpr std::u16string_view profile_list_path = uR"(\Registry\Machine\Software\Microsoft\Windows NT\CurrentVersion\ProfileList)";
        constexpr std::u16string_view profile_path =
            uR"(\Registry\Machine\Software\Microsoft\Windows NT\CurrentVersion\ProfileList\Profile)";
    }

    TEST_F(RegistryFileTest, ImportsUtf8BomStringsEscapesAndDword)
    {
        const auto contents = encode_utf8_with_bom(R"(Windows Registry Editor Version 5.00

[HKLM\Software\ParserCase]
@="Default"
"Quoted\"Name"="Line\nQuote:\" Slash:\\"
"Number"=dword:1234abcd
)");
        import_registry_file_contents(registry(), contents, "utf8.reg");

        const auto key = get_key(uR"(\Registry\Machine\Software\ParserCase)");
        const auto default_value = registry().get_value(key, "");
        ASSERT_TRUE(default_value.has_value());
        EXPECT_EQ(default_value->as_string(), u"Default");

        const auto escaped_value = registry().get_value(key, "Quoted\"Name");
        ASSERT_TRUE(escaped_value.has_value());
        EXPECT_EQ(escaped_value->as_string(), u"Line\nQuote:\" Slash:\\");

        const auto number = registry().get_value(key, "Number");
        ASSERT_TRUE(number.has_value());
        EXPECT_EQ(number->as_dword(), 0x1234ABCDu);
    }

    TEST_F(RegistryFileTest, ImportsUtf16LittleAndBigEndian)
    {
        const auto little_endian = encode_utf16(uR"(Windows Registry Editor Version 5.00

[HKCU\Endian\Little]
"Text"="Endian"
)",
                                                false);
        const auto big_endian = encode_utf16(uR"(Windows Registry Editor Version 5.00

[HKCU\Endian\Big]
"Text"="Endian"
)",
                                             true);

        import_registry_file_contents(registry(), little_endian, "little.reg");
        import_registry_file_contents(registry(), big_endian, "big.reg");

        for (const auto* name : {u"Little", u"Big"})
        {
            const auto key = get_key(std::filesystem::path{uR"(\Registry\User\Endian)"} / name);
            const auto value = registry().get_value(key, "Text");
            ASSERT_TRUE(value.has_value());
            EXPECT_EQ(value->as_string(), u"Endian");
        }
    }

    TEST_F(RegistryFileTest, ImportsContinuedHexAndExplicitHexType)
    {
        const auto contents = encode_utf8_with_bom(R"(Windows Registry Editor Version 5.00

[HKLM\Software\HexCase]
"Binary"=hex:01,02,\
  03,04,\
  05
"Multi"=hex(7):41,00,00,00
)");
        import_registry_file_contents(registry(), contents, "hex.reg");

        const auto key = get_key(uR"(\Registry\Machine\Software\HexCase)");
        const auto binary = registry().get_value(key, "Binary");
        ASSERT_TRUE(binary.has_value());
        EXPECT_EQ(binary->type, REG_BINARY);
        EXPECT_EQ(std::vector(binary->data.begin(), binary->data.end()),
                  (std::vector{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}, std::byte{5}}));

        const auto multi = registry().get_value(key, "Multi");
        ASSERT_TRUE(multi.has_value());
        EXPECT_EQ(multi->type, REG_MULTI_SZ);
        EXPECT_EQ(std::vector(multi->data.begin(), multi->data.end()),
                  (std::vector{std::byte{0x41}, std::byte{0}, std::byte{0}, std::byte{0}}));
    }

    TEST_F(RegistryFileTest, NormalizesRootAliasesAndPreservesKeyDisplayCase)
    {
        const auto contents = encode_utf8_with_bom(R"(Windows Registry Editor Version 5.00

[HKLM\Software\AliasL]
[HKCU\AliasC]
[HKU\AliasU]
[HKCR\CLSID\MiXeD"Key]
[HKCC\ConfigCase]
)");
        import_registry_file_contents(registry(), contents, "aliases.reg");

        EXPECT_TRUE(registry().get_key(utils::path_key{uR"(\Registry\Machine\Software\AliasL)"}).has_value());
        EXPECT_TRUE(registry().get_key(utils::path_key{uR"(\Registry\User\AliasC)"}).has_value());
        EXPECT_TRUE(registry().get_key(utils::path_key{uR"(\Registry\User\AliasU)"}).has_value());
        EXPECT_TRUE(registry().get_key(utils::path_key{uR"(\Registry\Machine\Software\Classes\CLSID\MiXeD"Key)"}).has_value());
        EXPECT_TRUE(registry()
                        .get_key(utils::path_key{uR"(\Registry\Machine\System\ControlSet001\Hardware Profiles\Current\ConfigCase)"})
                        .has_value());

        const auto clsid = get_key(uR"(\Registry\Machine\Software\Classes\CLSID)");
        ASSERT_EQ(registry().get_sub_key_count(clsid), 1u);
        const auto display_name = registry().get_sub_key_name(clsid, 0);
        ASSERT_TRUE(display_name.has_value());
        EXPECT_EQ(*display_name, "MiXeD\"Key");
    }

    TEST_F(RegistryFileTest, RejectsMalformedQuotedStringsHexTypesAndUtf16)
    {
        const auto unterminated_string = encode_utf8_with_bom(R"(Windows Registry Editor Version 5.00
[HKLM\Software\Bad]
"Value"="unterminated
)");
        const auto unterminated_type = encode_utf8_with_bom(R"(Windows Registry Editor Version 5.00
[HKLM\Software\Bad]
"Value"=hex(7:00
)");
        const std::vector odd_utf16{std::byte{0xFF}, std::byte{0xFE}, std::byte{0}};

        EXPECT_THROW(import_registry_file_contents(registry(), unterminated_string, "string.reg"), std::runtime_error);
        EXPECT_THROW(import_registry_file_contents(registry(), unterminated_type, "type.reg"), std::runtime_error);
        EXPECT_THROW(import_registry_file_contents(registry(), odd_utf16, "utf16.reg"), std::runtime_error);
    }

    TEST_F(RegistryFileTest, CreatesMissingKeyInLoadedHive)
    {
        const std::filesystem::path path{uR"(\Registry\Machine\Software\Vendor\Product)"};
        ASSERT_FALSE(registry().get_key(utils::path_key{path}).has_value());
        ASSERT_TRUE(registry().can_create_key(path));

        const auto created = registry().create_key(path);
        ASSERT_TRUE(created.has_value());
        EXPECT_TRUE(registry().get_key(utils::path_key{path}).has_value());

        const std::vector data{std::byte{1}, std::byte{2}};
        registry().set_value(*created, "Value", REG_BINARY, data);

        const auto value = registry().get_value(*created, "Value");
        ASSERT_TRUE(value.has_value());
        EXPECT_EQ(value->type, REG_BINARY);
        EXPECT_EQ(std::vector(value->data.begin(), value->data.end()), data);
    }

    TEST_F(RegistryFileTest, ResolvesOverlayOnlyKeysForExactPaths)
    {
        const std::filesystem::path path{uR"(\Registry\Machine\Software\Vendor\Overlay)"};
        ASSERT_FALSE(registry().get_key(utils::path_key{path}).has_value());
        ASSERT_TRUE(registry().create_key(path).has_value());
        EXPECT_TRUE(registry().get_key(utils::path_key{path}).has_value());

        const std::filesystem::path deeper{path / "Child"};
        EXPECT_FALSE(registry().get_key(utils::path_key{deeper}).has_value());

        const std::filesystem::path sibling{uR"(\Registry\Machine\Software\Vendor\Missing)"};
        EXPECT_FALSE(registry().get_key(utils::path_key{sibling}).has_value());
    }

    TEST_F(RegistryFileTest, EnumeratesOverlayKeysUnderParentOpenedWithTrailingSeparator)
    {
        const std::filesystem::path child{uR"(\Registry\Machine\Software\Vendor\Endpoints\{child})"};
        ASSERT_TRUE(registry().create_key(child).has_value());

        const auto parent = registry().get_key(utils::path_key{std::filesystem::path{uR"(\Registry\Machine\Software\Vendor\Endpoints\)"}});
        ASSERT_TRUE(parent.has_value());

        const auto name = registry().get_sub_key_name(*parent, 0);
        ASSERT_TRUE(name.has_value());
        EXPECT_EQ(*name, "{child}");
    }

    TEST_F(RegistryFileTest, CreateKeyIsIdempotent)
    {
        const std::filesystem::path path{uR"(\Registry\Machine\Software\Vendor\Once)"};

        const auto first = registry().create_key(path);
        ASSERT_TRUE(first.has_value());
        registry().set_value(*first, "Keep", REG_BINARY, std::vector{std::byte{7}});

        const auto second = registry().create_key(path);
        ASSERT_TRUE(second.has_value());
        EXPECT_TRUE(registry().get_value(*second, "Keep").has_value());
    }

    TEST_F(RegistryFileTest, RejectsKeyOutsideAnyLoadedHive)
    {
        const std::filesystem::path path{uR"(\Registry\NoSuchHive\Key)"};
        EXPECT_FALSE(registry().can_create_key(path));
        EXPECT_FALSE(registry().create_key(path).has_value());
    }

    TEST_F(RegistryFileTest, ReadsBigDataValuesFromEverySegment)
    {
        const auto key = get_key(uR"(\Registry\Machine\System\CurrentControlSet\Control\ProductOptions)");

        const auto pair = registry().get_value(key, "Pair");
        ASSERT_TRUE(pair.has_value());
        EXPECT_EQ(pair->type, REG_BINARY);
        EXPECT_EQ(std::vector(pair->data.begin(), pair->data.end()), create_pattern(big_data_segment_size + 0x100));

        const auto policy = registry().get_value(key, "ProductPolicy");
        ASSERT_TRUE(policy.has_value());
        const auto expected_policy = create_product_policy();
        ASSERT_GT(expected_policy.size(), 0x10000u);
        EXPECT_EQ(std::vector(policy->data.begin(), policy->data.end()), expected_policy);
    }

    TEST_F(LicenseValueTest, ReadsALicenseValueStoredInABigDataProductPolicy)
    {
        emulator_allocator allocator{this->emu->memory, this->storage, 0x1000};
        const auto name = allocator.make_unicode_string(u"Kernel-Test");
        const auto type = allocator.reserve<uint32_t>();
        const auto data = allocator.reserve<uint32_t>();
        const auto result_size = allocator.reserve<uint32_t>();

        ASSERT_EQ(STATUS_SUCCESS,
                  syscalls::handle_NtQueryLicenseValue(this->context(), name, type, data.value(), sizeof(uint32_t), result_size));
        EXPECT_EQ(type.read(), REG_DWORD);
        EXPECT_EQ(result_size.read(), sizeof(uint32_t));
        EXPECT_EQ(data.read(), 0x12345678u);
    }

    TEST_F(RegistryFileTest, DeletedHiveValuesAreHiddenFromLookupEnumerationAndCounts)
    {
        const auto key = get_key(profile_list_path);
        ASSERT_EQ(registry().get_value_count(key), 2u);

        EXPECT_TRUE(registry().delete_value(key, "first"));
        EXPECT_FALSE(registry().get_value(key, "First").has_value());
        EXPECT_FALSE(registry().delete_value(key, "First"));
        EXPECT_FALSE(registry().delete_value(key, "Missing"));
        ASSERT_EQ(registry().get_value_count(key), 1u);

        const auto remaining = registry().get_value(key, size_t{0});
        ASSERT_TRUE(remaining.has_value());
        EXPECT_EQ(remaining->name, "Second");
        EXPECT_FALSE(registry().get_value(key, size_t{1}).has_value());

        registry().set_value(key, "First", REG_BINARY, std::vector{std::byte{9}});
        EXPECT_EQ(registry().get_value_count(key), 2u);
        const auto restored = registry().get_value(key, "First");
        ASSERT_TRUE(restored.has_value());
        EXPECT_EQ(restored->type, REG_BINARY);
    }

    TEST_F(RegistryFileTest, DeletedOverlayValueIsRemoved)
    {
        const auto key = get_key(profile_list_path);
        registry().set_value(key, "Third", REG_BINARY, std::vector{std::byte{3}});
        ASSERT_EQ(registry().get_value_count(key), 3u);

        EXPECT_TRUE(registry().delete_value(key, "Third"));
        EXPECT_FALSE(registry().get_value(key, "Third").has_value());
        EXPECT_EQ(registry().get_value_count(key), 2u);
        EXPECT_FALSE(registry().get_value(key, size_t{2}).has_value());
    }

    TEST_F(RegistryFileTest, DeletedHiveKeyIsHiddenUntilRecreatedEmpty)
    {
        const auto parent = get_key(profile_list_path);
        registry().delete_key(get_key(profile_path));

        EXPECT_FALSE(registry().get_key(utils::path_key{std::filesystem::path{profile_path}}).has_value());
        EXPECT_EQ(registry().get_sub_key_count(parent), 0u);
        EXPECT_FALSE(registry().get_sub_key_name(parent, 0).has_value());

        registry().delete_key(parent);
        EXPECT_FALSE(registry().get_key(utils::path_key{std::filesystem::path{profile_list_path}}).has_value());
        EXPECT_EQ(registry().get_sub_key_count(get_key(uR"(\Registry\Machine\Software\Microsoft\Windows NT\CurrentVersion)")), 0u);

        const auto recreated = registry().create_key(std::filesystem::path{profile_list_path});
        ASSERT_TRUE(recreated.has_value());
        EXPECT_EQ(registry().get_value_count(*recreated), 0u);
        EXPECT_FALSE(registry().get_value(*recreated, "First").has_value());
        EXPECT_EQ(registry().get_sub_key_count(*recreated), 0u);

        const auto current_version = get_key(uR"(\Registry\Machine\Software\Microsoft\Windows NT\CurrentVersion)");
        ASSERT_EQ(registry().get_sub_key_count(current_version), 1u);
        const auto name = registry().get_sub_key_name(current_version, 0);
        ASSERT_TRUE(name.has_value());
        EXPECT_EQ(*name, "ProfileList");
    }

    TEST_F(RegistryFileTest, DeletedOverlayKeyIsRemovedFromItsParent)
    {
        const std::filesystem::path path{uR"(\Registry\Machine\Software\Vendor\Temporary)"};
        const auto created = registry().create_key(path);
        ASSERT_TRUE(created.has_value());
        registry().set_value(*created, "Value", REG_BINARY, std::vector{std::byte{1}});

        const auto vendor = get_key(uR"(\Registry\Machine\Software\Vendor)");
        ASSERT_EQ(registry().get_sub_key_count(vendor), 1u);

        registry().delete_key(*created);
        EXPECT_FALSE(registry().get_key(utils::path_key{path}).has_value());
        EXPECT_EQ(registry().get_sub_key_count(vendor), 0u);

        const auto recreated = registry().create_key(path);
        ASSERT_TRUE(recreated.has_value());
        EXPECT_FALSE(registry().get_value(*recreated, "Value").has_value());
    }

    TEST_F(RegistryFileTest, DeletionsSurviveARuntimeStateRoundTrip)
    {
        registry().delete_key(get_key(profile_path));
        ASSERT_TRUE(registry().delete_value(get_key(profile_list_path), "First"));

        utils::buffer_serializer snapshot{};
        registry().serialize_runtime_state(snapshot);

        registry_manager restored{directory()};
        utils::buffer_deserializer input{snapshot};
        restored.deserialize_runtime_state(input);

        EXPECT_FALSE(restored.get_key(utils::path_key{std::filesystem::path{profile_path}}).has_value());
        const auto parent = restored.get_key(utils::path_key{std::filesystem::path{profile_list_path}});
        ASSERT_TRUE(parent.has_value());
        EXPECT_EQ(restored.get_sub_key_count(*parent), 0u);
        EXPECT_FALSE(restored.get_value(*parent, "First").has_value());
        EXPECT_EQ(restored.get_value_count(*parent), 1u);
    }

    TEST_F(RegistrySyscallTest, DeletedKeyIsNotFoundByNameAndItsHandlesReportKeyDeleted)
    {
        constexpr std::u16string_view path = uR"(\Registry\Machine\Software\Vendor\Temporary)";
        handle key{};
        ULONG disposition{};
        ASSERT_EQ(STATUS_SUCCESS, this->create_key(path, key, disposition));
        ASSERT_EQ(static_cast<ULONG>(REG_CREATED_NEW_KEY), disposition);

        handle second{};
        ASSERT_EQ(STATUS_SUCCESS, this->open_key(path, second));

        EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtDeleteKey(this->context(), key));

        handle reopened{};
        EXPECT_EQ(STATUS_OBJECT_NAME_NOT_FOUND, this->open_key(path, reopened));
        EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtDeleteKey(this->context(), key));
        EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtDeleteKey(this->context(), second));
        EXPECT_EQ(STATUS_KEY_DELETED, this->query_value(second, u"Value"));
        EXPECT_EQ(STATUS_KEY_DELETED, this->enumerate_key(second, 0));

        const auto data = this->allocator->reserve<DWORD>();
        EXPECT_EQ(STATUS_KEY_DELETED, syscalls::handle_NtSetValueKey(this->context(), second, this->make_string(u"Value"), 0, REG_DWORD,
                                                                     data.value(), static_cast<ULONG>(sizeof(DWORD))));

        const auto result_length = this->allocator->reserve<ULONG>();
        EXPECT_EQ(STATUS_KEY_DELETED, syscalls::handle_NtQueryKey(this->context(), second, KeyFullInformation,
                                                                  this->allocator->reserve(0x100, 8), 0x100, result_length));

        handle vendor{};
        ASSERT_EQ(STATUS_SUCCESS, this->open_key(uR"(\Registry\Machine\Software\Vendor)", vendor));
        EXPECT_EQ(STATUS_NO_MORE_ENTRIES, this->enumerate_key(vendor, 0));
    }

    TEST_F(RegistrySyscallTest, KeyWithSubKeysAndRegistryRootCannotBeDeleted)
    {
        handle profile_list{};
        ASSERT_EQ(STATUS_SUCCESS, this->open_key(profile_list_path, profile_list));
        EXPECT_EQ(STATUS_CANNOT_DELETE, syscalls::handle_NtDeleteKey(this->context(), profile_list));

        handle machine{};
        ASSERT_EQ(STATUS_SUCCESS, this->open_key(uR"(\Registry\Machine)", machine));
        EXPECT_EQ(STATUS_CANNOT_DELETE, syscalls::handle_NtDeleteKey(this->context(), machine));

        EXPECT_EQ(STATUS_SUCCESS, this->enumerate_key(profile_list, 0));
    }

    TEST_F(RegistrySyscallTest, HiveKeyCanBeDeletedLeafFirstAndRecreatedEmpty)
    {
        handle profile{};
        ASSERT_EQ(STATUS_SUCCESS, this->open_key(profile_path, profile));
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtDeleteKey(this->context(), profile));

        handle profile_list{};
        ASSERT_EQ(STATUS_SUCCESS, this->open_key(profile_list_path, profile_list));
        EXPECT_EQ(STATUS_NO_MORE_ENTRIES, this->enumerate_key(profile_list, 0));
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtDeleteKey(this->context(), profile_list));

        handle recreated{};
        ULONG disposition{};
        ASSERT_EQ(STATUS_SUCCESS, this->create_key(profile_list_path, recreated, disposition));
        EXPECT_EQ(static_cast<ULONG>(REG_CREATED_NEW_KEY), disposition);
        EXPECT_EQ(STATUS_OBJECT_NAME_NOT_FOUND, this->query_value(recreated, u"First"));
        EXPECT_EQ(STATUS_NO_MORE_ENTRIES, this->enumerate_value(recreated, 0));
        EXPECT_EQ(STATUS_NO_MORE_ENTRIES, this->enumerate_key(recreated, 0));
    }

    TEST_F(RegistrySyscallTest, DeletedValueIsNotFoundAndNotEnumerated)
    {
        handle key{};
        ASSERT_EQ(STATUS_SUCCESS, this->open_key(profile_list_path, key));

        EXPECT_EQ(STATUS_SUCCESS, this->delete_value(key, u"First"));
        EXPECT_EQ(STATUS_OBJECT_NAME_NOT_FOUND, this->query_value(key, u"First"));
        EXPECT_EQ(STATUS_OBJECT_NAME_NOT_FOUND, this->delete_value(key, u"First"));
        EXPECT_EQ(STATUS_OBJECT_NAME_NOT_FOUND, this->delete_value(key, u"Missing"));

        EXPECT_EQ(STATUS_SUCCESS, this->query_value(key, u"Second"));
        EXPECT_EQ(STATUS_SUCCESS, this->enumerate_value(key, 0));
        EXPECT_EQ(STATUS_NO_MORE_ENTRIES, this->enumerate_value(key, 1));
    }

    TEST_F(RegistrySyscallTest, HiveRootOpenedByNameCannotBeDeleted)
    {
        handle child{};
        ULONG disposition{};
        ASSERT_EQ(STATUS_SUCCESS, this->create_key(uR"(\Registry\Machine\Software\Vendor)", child, disposition));

        handle software{};
        ASSERT_EQ(STATUS_SUCCESS, this->open_key(uR"(\Registry\Machine\Software)", software));

        const auto data = this->allocator->reserve<DWORD>();
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtSetValueKey(this->context(), software, this->make_string(u"RootValue"), 0, REG_DWORD,
                                                                 data.value(), static_cast<ULONG>(sizeof(DWORD))));
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtDeleteKey(this->context(), child));

        EXPECT_EQ(STATUS_CANNOT_DELETE, syscalls::handle_NtDeleteKey(this->context(), software));
        EXPECT_EQ(STATUS_SUCCESS, this->query_value(software, u"RootValue"));

        handle reopened{};
        EXPECT_EQ(STATUS_SUCCESS, this->open_key(uR"(\Registry\Machine\Software)", reopened));
        EXPECT_EQ(STATUS_SUCCESS, this->open_key(profile_list_path, reopened));
    }

    TEST_F(RegistrySyscallTest, DeletedKeyHandleTagsCanStillBeSetAndQueried)
    {
        handle key{};
        ASSERT_EQ(STATUS_SUCCESS, this->open_key(profile_path, key));
        handle second{};
        ASSERT_EQ(STATUS_SUCCESS, this->open_key(profile_path, second));
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtDeleteKey(this->context(), key));

        ULONG tags = 5;
        for (const auto stale : {key, second})
        {
            EXPECT_EQ(STATUS_SUCCESS, this->set_handle_tags(stale, tags));

            const auto info = this->allocator->reserve<KEY_HANDLE_TAGS_INFORMATION>();
            const auto result_length = this->allocator->reserve<ULONG>();
            EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtQueryKey(this->context(), stale, KeyHandleTagsInformation, info.value(),
                                                                  static_cast<ULONG>(sizeof(KEY_HANDLE_TAGS_INFORMATION)), result_length));
            EXPECT_EQ(tags, info.read().HandleTags);
            EXPECT_EQ(STATUS_KEY_DELETED, this->query_key(stale, KeyBasicInformation));
            ++tags;
        }
    }

    TEST_F(RegistrySyscallTest, DeletedKeyReportsKeyDeletedForCachedInformationMultipleValuesAndObjectName)
    {
        handle profile{};
        ASSERT_EQ(STATUS_SUCCESS, this->open_key(profile_path, profile));
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtDeleteKey(this->context(), profile));

        handle key{};
        ASSERT_EQ(STATUS_SUCCESS, this->open_key(profile_list_path, key));
        handle second{};
        ASSERT_EQ(STATUS_SUCCESS, this->open_key(profile_list_path, second));

        ASSERT_EQ(STATUS_SUCCESS, this->query_key(key, KeyCachedInformation));
        ASSERT_EQ(STATUS_SUCCESS, this->query_multiple_values(key, u"First"));
        ASSERT_EQ(STATUS_SUCCESS, this->query_object_name(key));
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtDeleteKey(this->context(), key));

        for (const auto stale : {key, second})
        {
            EXPECT_EQ(STATUS_KEY_DELETED, this->query_key(stale, KeyCachedInformation));
            EXPECT_EQ(STATUS_KEY_DELETED, this->query_multiple_values(stale, u"First"));
            EXPECT_EQ(STATUS_KEY_DELETED, this->query_object_name(stale));
        }
    }

    TEST_F(RegistrySyscallTest, KeysCannotBeOpenedOrCreatedRelativeToADeletedKey)
    {
        handle key{};
        ASSERT_EQ(STATUS_SUCCESS, this->open_key(profile_path, key));
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtDeleteKey(this->context(), key));

        handle relative{};
        EXPECT_EQ(STATUS_KEY_DELETED, this->open_key(u"", relative, key));

        ULONG disposition{};
        EXPECT_EQ(STATUS_KEY_DELETED, this->create_key(u"Sub", relative, disposition, key));
        EXPECT_EQ(STATUS_OBJECT_NAME_NOT_FOUND, this->open_key(std::u16string{profile_path} + u"\\Sub", relative));
    }

    TEST_F(RegistrySyscallTest, DeletingANullValueNameIsAnAccessViolation)
    {
        handle key{};
        ASSERT_EQ(STATUS_SUCCESS, this->open_key(profile_list_path, key));

        const auto data = this->allocator->reserve<DWORD>();
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtSetValueKey(this->context(), key, this->make_string(u""), 0, REG_DWORD, data.value(),
                                                                 static_cast<ULONG>(sizeof(DWORD))));

        EXPECT_EQ(STATUS_ACCESS_VIOLATION, syscalls::handle_NtDeleteValueKey(this->context(), key, {this->emu->memory, uint64_t{0}}));
        EXPECT_EQ(STATUS_SUCCESS, this->query_value(key, u""));
        EXPECT_EQ(STATUS_SUCCESS, this->delete_value(key, u""));
        EXPECT_EQ(STATUS_OBJECT_NAME_NOT_FOUND, this->query_value(key, u""));
    }
} // namespace sogen::test
