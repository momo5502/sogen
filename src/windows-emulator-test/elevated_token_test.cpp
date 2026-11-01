#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtQueryInformationToken(const syscall_context& c, handle token_handle, TOKEN_INFORMATION_CLASS information_class,
                                            uint64_t information, ULONG information_length, emulator_object<ULONG> return_length);
    NTSTATUS handle_NtAccessCheck(const syscall_context& c, uint64_t security_descriptor, handle requested_token,
                                  ACCESS_MASK desired_access, emulator_object<EMU_GENERIC_MAPPING> generic_mapping, uint64_t privilege_set,
                                  emulator_object<ULONG> privilege_set_length, emulator_object<ACCESS_MASK> granted_access,
                                  emulator_object<NTSTATUS> access_status);
    NTSTATUS handle_NtImpersonateAnonymousToken(const syscall_context& c, handle thread_handle);
}

namespace sogen::test
{
    namespace
    {
        constexpr std::array<uint8_t, 16> administrators_sid = {
            0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x20, 0x00, 0x00, 0x00, 0x20, 0x02, 0x00, 0x00,
        };

        class ElevatedTokenTest : public testing::TestWithParam<bool>
        {
          protected:
            void SetUp() override
            {
                emulator_interfaces interfaces{};
                interfaces.ui = std::make_unique<null_ui_backend>();
                interfaces.audio = std::make_unique<null_audio_backend>();
                interfaces.console = std::make_unique<null_console_backend>();
                this->emu = std::make_unique<windows_emulator>(create_x86_64_emulator_from_environment(),
                                                               emulator_settings{.disable_logging = true,
                                                                                 .emulation_root = get_emulator_root(),
                                                                                 .load_registry = false,
                                                                                 .elevated = this->elevated()},
                                                               emulator_callbacks{}, std::move(interfaces));
                this->storage = this->emu->memory.allocate_memory(0x2000, memory_permission::read_write);
                const auto thread =
                    this->emu->process.threads.store(emulator_thread{this->emu->memory, this->emu->process, 0, 0, 0x100000, 0, 8, true});
                this->emu->vcpu(0).active_thread = this->emu->process.threads.get(thread);
            }

            bool elevated() const
            {
                return GetParam();
            }

            syscall_context context()
            {
                return {.win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
            }

            NTSTATUS query(const handle token, const TOKEN_INFORMATION_CLASS information_class, const ULONG size)
            {
                return syscalls::handle_NtQueryInformationToken(this->context(), token, information_class, this->storage + 0x100, size,
                                                                {this->emu->memory, this->storage});
            }

            ULONG required_size(const handle token, const TOKEN_INFORMATION_CLASS information_class)
            {
                EXPECT_EQ(STATUS_BUFFER_TOO_SMALL, this->query(token, information_class, 0));
                return emulator_object<ULONG>{this->emu->memory, this->storage}.read();
            }

            // A self-relative descriptor with a single allow ACE for the given SID.
            uint64_t build_descriptor(const std::span<const uint8_t> sid, const ACCESS_MASK mask)
            {
                const auto descriptor_address = this->storage + 0x1000;
                const auto dacl_offset = static_cast<uint32_t>(sizeof(SECURITY_DESCRIPTOR_RELATIVE));
                const auto ace_offset = dacl_offset + static_cast<uint32_t>(sizeof(ACL));
                const auto ace_size = static_cast<uint16_t>(offsetof(ACCESS_ALLOWED_ACE, SidStart) + sid.size());

                SECURITY_DESCRIPTOR_RELATIVE descriptor{};
                descriptor.Revision = 1;
                descriptor.Control = SE_SELF_RELATIVE | SE_DACL_PRESENT;
                descriptor.Dacl = dacl_offset;
                emulator_object<SECURITY_DESCRIPTOR_RELATIVE>{this->emu->memory, descriptor_address}.write(descriptor);

                ACL acl{};
                acl.AclRevision = 2;
                acl.AclSize = static_cast<uint16_t>(sizeof(ACL) + ace_size);
                acl.AceCount = 1;
                emulator_object<ACL>{this->emu->memory, descriptor_address + dacl_offset}.write(acl);

                ACCESS_ALLOWED_ACE ace{};
                ace.Header.AceType = 0;
                ace.Header.AceSize = ace_size;
                ace.Mask = mask;
                emulator_object<ACCESS_ALLOWED_ACE>{this->emu->memory, descriptor_address + ace_offset}.write(ace);
                this->emu->memory.write_memory(descriptor_address + ace_offset + offsetof(ACCESS_ALLOWED_ACE, SidStart), sid.data(),
                                               sid.size());

                return descriptor_address;
            }

            NTSTATUS check_access(const handle token, const std::span<const uint8_t> sid, const ACCESS_MASK mask)
            {
                const auto descriptor = this->build_descriptor(sid, mask);

                const emulator_object<EMU_GENERIC_MAPPING> mapping{this->emu->memory, this->storage + 0x800};
                mapping.write(EMU_GENERIC_MAPPING{});

                const emulator_object<ULONG> privilege_set_length{this->emu->memory, this->storage + 0x820};
                const emulator_object<ACCESS_MASK> granted{this->emu->memory, this->storage + 0x830};
                const emulator_object<NTSTATUS> status{this->emu->memory, this->storage + 0x840};

                const auto result = syscalls::handle_NtAccessCheck(this->context(), descriptor, token, mask, mapping, 0,
                                                                   privilege_set_length, granted, status);
                EXPECT_EQ(STATUS_SUCCESS, result);
                return status.read();
            }

            std::unique_ptr<windows_emulator> emu{};
            uint64_t storage{};
        };
    }

    TEST_P(ElevatedTokenTest, GroupsIncludeAdministratorsOnlyWhenElevated)
    {
        const auto size = this->required_size(CURRENT_PROCESS_TOKEN, TokenGroups);
        ASSERT_EQ(STATUS_SUCCESS, this->query(CURRENT_PROCESS_TOKEN, TokenGroups, size));

        const auto groups = emulator_object<TOKEN_GROUPS64>{this->emu->memory, this->storage + 0x100}.read();

        // The user, Interactive and logon groups are always there; Administrators is appended when elevated.
        constexpr ULONG unelevated_group_count = 3;
        ASSERT_EQ(this->elevated() ? unelevated_group_count + 1 : unelevated_group_count, groups.GroupCount);

        if (!this->elevated())
        {
            return;
        }

        // Groups[0] lives in the header, so the last entry sits that many SID_AND_ATTRIBUTES64 past it.
        const auto administrators =
            emulator_object<SID_AND_ATTRIBUTES64>{this->emu->memory, this->storage + 0x100 + sizeof(TOKEN_GROUPS64) +
                                                                         (unelevated_group_count - 1) * sizeof(SID_AND_ATTRIBUTES64)}
                .read();

        // SE_GROUP_MANDATORY | SE_GROUP_ENABLED_BY_DEFAULT | SE_GROUP_ENABLED | SE_GROUP_OWNER
        EXPECT_EQ(0xFu, administrators.Attributes);

        std::array<uint8_t, administrators_sid.size()> read_sid{};
        this->emu->memory.read_memory(administrators.Sid, read_sid.data(), read_sid.size());
        EXPECT_EQ(administrators_sid, read_sid);
    }

    TEST_P(ElevatedTokenTest, StatisticsAgreeWithTheGroupList)
    {
        ASSERT_EQ(STATUS_SUCCESS, this->query(CURRENT_PROCESS_TOKEN, TokenStatistics, sizeof(TOKEN_STATISTICS)));
        const auto stats = emulator_object<TOKEN_STATISTICS>{this->emu->memory, this->storage + 0x100}.read();
        EXPECT_EQ(this->elevated() ? 4u : 3u, stats.GroupCount);
    }

    TEST_P(ElevatedTokenTest, AdministratorsMembershipFollowsTheElevatedFlag)
    {
        const auto expected = this->elevated() ? STATUS_SUCCESS : STATUS_ACCESS_DENIED;
        EXPECT_EQ(expected, this->check_access(CURRENT_PROCESS_TOKEN, administrators_sid, 1));
    }

    TEST_P(ElevatedTokenTest, AnonymousImpersonationIsNeverElevated)
    {
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtImpersonateAnonymousToken(this->context(), CURRENT_THREAD));

        ASSERT_EQ(STATUS_SUCCESS, this->query(CURRENT_THREAD_EFFECTIVE_TOKEN, TokenElevation, sizeof(TOKEN_ELEVATION)));
        EXPECT_EQ(0u, (emulator_object<TOKEN_ELEVATION>{this->emu->memory, this->storage + 0x100}.read().TokenIsElevated));

        const auto size = this->required_size(CURRENT_THREAD_EFFECTIVE_TOKEN, TokenIntegrityLevel);
        ASSERT_EQ(STATUS_SUCCESS, this->query(CURRENT_THREAD_EFFECTIVE_TOKEN, TokenIntegrityLevel, size));
        const auto label = emulator_object<TOKEN_MANDATORY_LABEL64>{this->emu->memory, this->storage + 0x100}.read();

        // S-1-16-8192, medium integrity: the anonymous token must not pick up the process elevation.
        EXPECT_EQ(0x20u, this->emu->memory.read_memory<uint8_t>(label.Label.Sid + 9));

        EXPECT_EQ(STATUS_ACCESS_DENIED, this->check_access(CURRENT_THREAD_EFFECTIVE_TOKEN, administrators_sid, 1));
    }

    INSTANTIATE_TEST_SUITE_P(Elevation, ElevatedTokenTest, testing::Values(false, true),
                             [](const testing::TestParamInfo<bool>& info) { return info.param ? "Elevated" : "Default"; });
}
