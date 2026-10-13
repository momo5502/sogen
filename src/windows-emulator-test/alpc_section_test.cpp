#include "rpc_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtAlpcCreatePortSection(const syscall_context& c, handle port_handle, ULONG flags, handle section_handle,
                                            uint64_t section_size, emulator_object<handle> alpc_section_handle,
                                            emulator_object<uint64_t> actual_section_size);
    NTSTATUS handle_NtAlpcDeletePortSection(const syscall_context& c, handle port_handle, ULONG flags, handle section_handle);
    NTSTATUS handle_NtAlpcCreateSectionView(const syscall_context& c, handle port_handle, ULONG flags,
                                            emulator_object<ALPC_DATA_VIEW_ATTR<EmulatorTraits<Emu64>>> view_attributes);
    NTSTATUS handle_NtAlpcDeleteSectionView(const syscall_context& c, handle port_handle, ULONG flags, uint64_t view_base);
    NTSTATUS handle_NtClose(const syscall_context& c, handle handle);
    NTSTATUS handle_NtAlpcSendWaitReceivePort(const syscall_context& c, handle port_handle, ULONG flags,
                                              emulator_object<PORT_MESSAGE64> send_message,
                                              emulator_object<ALPC_MESSAGE_ATTRIBUTES> send_message_attributes,
                                              emulator_object<PORT_MESSAGE64> receive_message, emulator_object<uint64_t> buffer_length,
                                              emulator_object<ALPC_MESSAGE_ATTRIBUTES> receive_message_attributes,
                                              emulator_object<LARGE_INTEGER> timeout);
}

namespace sogen::test
{
    namespace
    {
        using view_attributes = ALPC_DATA_VIEW_ATTR<EmulatorTraits<Emu64>>;

        class AlpcSectionTest : public RpcProtocolTest
        {
          protected:
            void SetUp() override
            {
                RpcProtocolTest::SetUp();
                this->port_handle = this->emu->process.ports.store(port_container{u"", *this->emu, {}});
            }

            syscall_context context()
            {
                return {.win_emu = *this->emu, .emu = this->emu->emu(), .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
            }

            NTSTATUS create_section(uint64_t size, ULONG flags = 0)
            {
                return syscalls::handle_NtAlpcCreatePortSection(this->context(), this->port_handle, flags, {}, size,
                                                                {this->emu->memory, this->storage}, {this->emu->memory, this->storage + 8});
            }

            handle section_handle() const
            {
                return emulator_object<handle>{this->emu->memory, this->storage}.read();
            }

            NTSTATUS create_view(uint64_t size, view_attributes& result)
            {
                emulator_object<view_attributes> output{this->emu->memory, this->storage + 0x100};
                output.write({.Flags = 0, .SectionHandle = this->section_handle().bits, .ViewBase = 0, .ViewSize = size});
                const auto status = syscalls::handle_NtAlpcCreateSectionView(this->context(), this->port_handle, 0, output);
                result = output.read();
                return status;
            }

            NTSTATUS delete_view(uint64_t address)
            {
                return syscalls::handle_NtAlpcDeleteSectionView(this->context(), this->port_handle, 0, address);
            }

            port* inner_port() const
            {
                return this->emu->process.ports.get(this->port_handle)->get_internal_port();
            }

            handle port_handle{};

            template <typename Traits>
            NTSTATUS send_view(const ALPC_DATA_VIEW_ATTR<Traits>& view, ULONG allocated = ALPC_MESSAGE_VIEW_ATTRIBUTE,
                               ULONG valid = ALPC_MESSAGE_VIEW_ATTRIBUTE)
            {
                emulator_object<ALPC_MESSAGE_ATTRIBUTES>{this->emu->memory, this->storage + 0x200}.write(
                    {.AllocatedAttributes = allocated, .ValidAttributes = valid});
                uint64_t offset = sizeof(ALPC_MESSAGE_ATTRIBUTES);
                if ((allocated & ALPC_MESSAGE_SECURITY_ATTRIBUTE) != 0)
                {
                    offset += sizeof(typename Traits::PVOID) == 4 ? 12 : 32;
                }
                emulator_object<ALPC_DATA_VIEW_ATTR<Traits>>{this->emu->memory, this->storage + 0x200 + offset}.write(view);
                PORT_MESSAGE64 message{};
                message.u1.s1.DataLength = 8;
                message.u1.s1.TotalLength = sizeof(message) + 8;
                emulator_object<PORT_MESSAGE64>{this->emu->memory, this->storage + 0x400}.write(message);
                this->emu->memory.set_memory(this->storage + 0x400 + sizeof(message), 0, 8);
                emulator_object<uint64_t>{this->emu->memory, this->storage + 0x300}.write(0x100);
                return syscalls::handle_NtAlpcSendWaitReceivePort(
                    this->context(), this->port_handle, 0, {this->emu->memory, this->storage + 0x400},
                    {this->emu->memory, this->storage + 0x200}, {this->emu->memory, this->storage + 0x800},
                    {this->emu->memory, this->storage + 0x300}, {this->emu->memory}, {this->emu->memory});
            }
        };
    }

    TEST_F(AlpcSectionTest, RoundsSectionsToAllocationGranularityAndViewsToPages)
    {
        ASSERT_EQ(STATUS_SUCCESS, this->create_section(4097, 0x40000));
        EXPECT_EQ(0x10000u, (emulator_object<uint64_t>{this->emu->memory, this->storage + 8}.read()));
        view_attributes view{};
        ASSERT_EQ(STATUS_SUCCESS, this->create_view(4097, view));
        EXPECT_NE(0u, view.ViewBase);
        EXPECT_EQ(0x2000u, view.ViewSize);
        EXPECT_EQ(0u, this->emu->memory.read_memory<uint32_t>(view.ViewBase));
        ASSERT_EQ(STATUS_SUCCESS, this->delete_view(view.ViewBase));
    }

    TEST_F(AlpcSectionTest, PageSizedViewsConsumeDisjointAllocationGranularitySlots)
    {
        ASSERT_EQ(STATUS_SUCCESS, this->create_section(0x20000));
        view_attributes first{};
        view_attributes second{};
        view_attributes extra{};
        ASSERT_EQ(STATUS_SUCCESS, this->create_view(1, first));
        ASSERT_EQ(STATUS_SUCCESS, this->create_view(1, second));
        EXPECT_NE(first.ViewBase, second.ViewBase);
        EXPECT_EQ(0x1000u, first.ViewSize);
        this->emu->memory.write_memory(first.ViewBase, uint32_t{0xabcdef01});
        EXPECT_EQ(0u, this->emu->memory.read_memory<uint32_t>(second.ViewBase));
        EXPECT_EQ(STATUS_INSUFFICIENT_RESOURCES, this->create_view(1, extra));
        EXPECT_EQ(0u, extra.ViewBase);
        EXPECT_EQ(STATUS_SUCCESS, this->delete_view(first.ViewBase));
        EXPECT_EQ(STATUS_SUCCESS, this->delete_view(second.ViewBase));
    }

    TEST_F(AlpcSectionTest, ReusingFreedSlotPreservesDataAcrossPageSizedAndLargerViews)
    {
        ASSERT_EQ(STATUS_SUCCESS, this->create_section(0x10000));
        view_attributes first{};
        view_attributes replacement{};
        ASSERT_EQ(STATUS_SUCCESS, this->create_view(0x2000, first));
        this->emu->memory.write_memory(first.ViewBase, uint32_t{0xabcdef01});
        this->emu->memory.write_memory(first.ViewBase + 0x1000, uint32_t{0x12345678});
        ASSERT_EQ(STATUS_SUCCESS, this->delete_view(first.ViewBase));
        ASSERT_EQ(STATUS_SUCCESS, this->create_view(1, replacement));
        EXPECT_EQ(0xabcdef01u, this->emu->memory.read_memory<uint32_t>(replacement.ViewBase));
        this->emu->memory.write_memory(replacement.ViewBase, uint32_t{0x87654321});
        ASSERT_EQ(STATUS_SUCCESS, this->delete_view(replacement.ViewBase));
        ASSERT_EQ(STATUS_SUCCESS, this->create_view(0x2000, replacement));
        EXPECT_EQ(0x87654321u, this->emu->memory.read_memory<uint32_t>(replacement.ViewBase));
        EXPECT_EQ(0x12345678u, this->emu->memory.read_memory<uint32_t>(replacement.ViewBase + 0x1000));
        EXPECT_EQ(STATUS_SUCCESS, this->delete_view(replacement.ViewBase));
    }

    TEST_F(AlpcSectionTest, DeletingSectionHandleKeepsExistingViewAliveUntilViewDeletion)
    {
        ASSERT_EQ(STATUS_SUCCESS, this->create_section(1));
        const auto section = this->section_handle();
        view_attributes view{};
        view_attributes extra{};
        ASSERT_EQ(STATUS_SUCCESS, this->create_view(1, view));
        this->emu->memory.write_memory(view.ViewBase, uint32_t{123});
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtAlpcDeletePortSection(this->context(), this->port_handle, 0, section));
        EXPECT_EQ(123u, this->emu->memory.read_memory<uint32_t>(view.ViewBase));
        EXPECT_EQ(STATUS_INVALID_HANDLE, this->create_view(1, extra));
        EXPECT_EQ(STATUS_INVALID_HANDLE, syscalls::handle_NtAlpcDeletePortSection(this->context(), this->port_handle, 0, section));
        ASSERT_EQ(STATUS_SUCCESS, this->delete_view(view.ViewBase));
        EXPECT_TRUE(this->inner_port()->sections.empty());
        EXPECT_EQ(STATUS_INVALID_ADDRESS, this->delete_view(view.ViewBase));
    }

    TEST_F(AlpcSectionTest, DeletingNoAccessAndGuardedViewPreservesBackingContents)
    {
        ASSERT_EQ(STATUS_SUCCESS, this->create_section(0x10000));
        view_attributes view{};
        ASSERT_EQ(STATUS_SUCCESS, this->create_view(0x2000, view));
        this->emu->memory.write_memory(view.ViewBase, uint32_t{0xabcdef01});
        this->emu->memory.write_memory(view.ViewBase + 0x1000, uint32_t{0x12345678});
        ASSERT_TRUE(this->emu->memory.protect_memory(view.ViewBase, 0x1000, memory_permission::none));
        ASSERT_TRUE(
            this->emu->memory.protect_memory(view.ViewBase + 0x1000, 0x1000, memory_permission::read_write | memory_permission::guard));
        ASSERT_EQ(STATUS_SUCCESS, this->delete_view(view.ViewBase));
        ASSERT_EQ(STATUS_SUCCESS, this->create_view(0x2000, view));
        EXPECT_EQ(0xabcdef01u, this->emu->memory.read_memory<uint32_t>(view.ViewBase));
        EXPECT_EQ(0x12345678u, this->emu->memory.read_memory<uint32_t>(view.ViewBase + 0x1000));
        EXPECT_EQ(STATUS_SUCCESS, this->delete_view(view.ViewBase));
    }

    TEST_F(AlpcSectionTest, FailedPageCapturePreservesCacheViewAndOriginalPermissions)
    {
        ASSERT_EQ(STATUS_SUCCESS, this->create_section(0x10000));
        view_attributes view{};
        ASSERT_EQ(STATUS_SUCCESS, this->create_view(0x2000, view));
        this->emu->memory.write_memory(view.ViewBase, uint32_t{123});
        ASSERT_EQ(STATUS_SUCCESS, this->delete_view(view.ViewBase));
        ASSERT_EQ(STATUS_SUCCESS, this->create_view(0x2000, view));
        this->emu->memory.write_memory(view.ViewBase, uint32_t{456});
        ASSERT_TRUE(this->emu->memory.protect_memory(view.ViewBase, 0x1000, memory_permission::none));
        ASSERT_TRUE(this->emu->memory.decommit_memory(view.ViewBase + 0x1000, 0x1000));
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->delete_view(view.ViewBase));
        EXPECT_EQ(memory_permission::none, this->emu->memory.get_region_info(view.ViewBase).permissions.common);
        const auto& section = this->inner_port()->sections.begin()->second;
        EXPECT_EQ(1u, section.views.size());
        uint32_t saved_value{};
        std::memcpy(&saved_value, section.saved_pages.at(0).data(), sizeof(saved_value));
        EXPECT_EQ(123u, saved_value);
        ASSERT_TRUE(this->emu->memory.commit_memory(view.ViewBase + 0x1000, 0x1000, memory_permission::read_write));
        ASSERT_EQ(STATUS_SUCCESS, this->delete_view(view.ViewBase));
        ASSERT_EQ(STATUS_SUCCESS, this->create_view(0x2000, view));
        EXPECT_EQ(456u, this->emu->memory.read_memory<uint32_t>(view.ViewBase));
        EXPECT_EQ(STATUS_SUCCESS, this->delete_view(view.ViewBase));
    }

    TEST_F(AlpcSectionTest, RepeatedViewDeletionDoesNotGrowSavedPagesForSameSlot)
    {
        ASSERT_EQ(STATUS_SUCCESS, this->create_section(0x10000));
        view_attributes view{};
        for (uint32_t value = 0; value < 16; ++value)
        {
            ASSERT_EQ(STATUS_SUCCESS, this->create_view(0x2000, view));
            if (value != 0)
            {
                EXPECT_EQ(value - 1, this->emu->memory.read_memory<uint32_t>(view.ViewBase));
            }
            this->emu->memory.write_memory(view.ViewBase, value);
            ASSERT_EQ(STATUS_SUCCESS, this->delete_view(view.ViewBase));
            EXPECT_EQ(2u, this->inner_port()->sections.begin()->second.saved_pages.size());
        }
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtAlpcDeletePortSection(this->context(), this->port_handle, 0, this->section_handle()));
        EXPECT_TRUE(this->inner_port()->sections.empty());
    }

    TEST_F(AlpcSectionTest, RejectsInvalidInputsWithoutAllocatingViewsOrSections)
    {
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->create_section(0));
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->create_section(std::numeric_limits<uint64_t>::max()));
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->create_section(1, 1));
        EXPECT_TRUE(this->inner_port()->sections.empty());
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, syscalls::handle_NtAlpcCreatePortSection(this->context(), this->port_handle, 0, {}, 1,
                                                                                    {this->emu->memory}, {this->emu->memory}));
        ASSERT_EQ(STATUS_SUCCESS, this->create_section(1));
        view_attributes view{};
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->create_view(0, view));
        EXPECT_EQ(STATUS_INSUFFICIENT_RESOURCES, this->create_view(0x10001, view));
        EXPECT_EQ(STATUS_ACCESS_VIOLATION,
                  syscalls::handle_NtAlpcCreateSectionView(this->context(), this->port_handle, 0, {this->emu->memory}));
        emulator_object<view_attributes> output{this->emu->memory, this->storage + 0x100};
        output.write({.Flags = 0x40000, .SectionHandle = this->section_handle().bits, .ViewBase = 0, .ViewSize = 1});
        EXPECT_EQ(STATUS_INVALID_PARAMETER, syscalls::handle_NtAlpcCreateSectionView(this->context(), this->port_handle, 0, output));
        output.write({.Flags = 0, .SectionHandle = this->section_handle().bits, .ViewBase = 0x123450000, .ViewSize = 1});
        EXPECT_EQ(STATUS_INVALID_PARAMETER, syscalls::handle_NtAlpcCreateSectionView(this->context(), this->port_handle, 0, output));
        EXPECT_TRUE(this->inner_port()->sections.begin()->second.views.empty());
    }

    TEST_F(AlpcSectionTest, ClosingPortReleasesAllActiveViews)
    {
        ASSERT_EQ(STATUS_SUCCESS, this->create_section(0x20000));
        view_attributes first{};
        view_attributes second{};
        ASSERT_EQ(STATUS_SUCCESS, this->create_view(1, first));
        ASSERT_EQ(STATUS_SUCCESS, this->create_view(1, second));
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtClose(this->context(), this->port_handle));
        uint32_t value{};
        EXPECT_FALSE(this->emu->memory.try_read_memory(first.ViewBase, &value, sizeof(value)));
        EXPECT_FALSE(this->emu->memory.try_read_memory(second.ViewBase, &value, sizeof(value)));
    }

    TEST_F(AlpcSectionTest, PortSerializationPreservesFreedPageContentsAndActiveViewAllocation)
    {
        ASSERT_EQ(STATUS_SUCCESS, this->create_section(0x20000));
        view_attributes first{};
        view_attributes second{};
        view_attributes replacement{};
        ASSERT_EQ(STATUS_SUCCESS, this->create_view(1, first));
        ASSERT_EQ(STATUS_SUCCESS, this->create_view(1, second));
        this->emu->memory.write_memory(first.ViewBase, uint32_t{0xabcdef01});
        ASSERT_EQ(STATUS_SUCCESS, this->delete_view(first.ViewBase));
        auto* container = this->emu->process.ports.get(this->port_handle);
        utils::buffer_serializer snapshot;
        container->serialize_object(snapshot);
        utils::buffer_deserializer input{snapshot};
        container->deserialize_object(input);
        ASSERT_EQ(STATUS_SUCCESS, this->create_view(1, replacement));
        EXPECT_EQ(0xabcdef01u, this->emu->memory.read_memory<uint32_t>(replacement.ViewBase));
        EXPECT_NE(second.ViewBase, replacement.ViewBase);
        EXPECT_EQ(STATUS_SUCCESS, this->delete_view(second.ViewBase));
        EXPECT_EQ(STATUS_SUCCESS, this->delete_view(replacement.ViewBase));
    }

    TEST_F(AlpcSectionTest, MovingPortPreservesActiveViewsAndSavedPages)
    {
        ASSERT_EQ(STATUS_SUCCESS, this->create_section(0x20000));
        view_attributes first{};
        view_attributes second{};
        ASSERT_EQ(STATUS_SUCCESS, this->create_view(1, first));
        ASSERT_EQ(STATUS_SUCCESS, this->create_view(1, second));
        this->emu->memory.write_memory(first.ViewBase, uint32_t{0xabcdef01});
        this->emu->memory.write_memory(second.ViewBase, uint32_t{0x12345678});
        ASSERT_EQ(STATUS_SUCCESS, this->delete_view(first.ViewBase));
        auto* stored = this->emu->process.ports.get(this->port_handle);
        port_container moved{std::move(*stored)};
        EXPECT_TRUE(moved.get_internal_port()->owns_section_view(this->section_handle().bits, second.ViewBase, second.ViewSize));
        EXPECT_EQ(0x12345678u, this->emu->memory.read_memory<uint32_t>(second.ViewBase));
        *stored = std::move(moved);
        ASSERT_EQ(STATUS_SUCCESS, this->create_view(1, first));
        EXPECT_EQ(0xabcdef01u, this->emu->memory.read_memory<uint32_t>(first.ViewBase));
        EXPECT_EQ(STATUS_SUCCESS, this->delete_view(first.ViewBase));
        EXPECT_EQ(STATUS_SUCCESS, this->delete_view(second.ViewBase));
    }

    TEST_F(AlpcSectionTest, SendValidatesViewOwnershipBoundsAndAttributeMasks)
    {
        ASSERT_EQ(STATUS_SUCCESS, this->create_section(0x10000));
        view_attributes view{};
        ASSERT_EQ(STATUS_SUCCESS, this->create_view(1, view));
        view.Flags = 0x40000;
        EXPECT_EQ(STATUS_SUCCESS, this->send_view(view));
        EXPECT_EQ(STATUS_SUCCESS, this->send_view(view, ALPC_MESSAGE_VIEW_ATTRIBUTE | ALPC_MESSAGE_SECURITY_ATTRIBUTE));
        auto invalid = view;
        ++invalid.SectionHandle;
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->send_view(invalid));
        invalid = view;
        ++invalid.ViewBase;
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->send_view(invalid));
        invalid = view;
        ++invalid.ViewSize;
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->send_view(invalid));
        invalid = view;
        invalid.Flags = 1;
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->send_view(invalid));
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->send_view(view, 0));
        EXPECT_EQ(STATUS_SUCCESS, this->send_view(invalid, ALPC_MESSAGE_VIEW_ATTRIBUTE, 0));
        ASSERT_EQ(STATUS_SUCCESS, this->delete_view(view.ViewBase));
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->send_view(view));
    }

    TEST_F(AlpcSectionTest, SendReadsWow64ViewAttributesAtWow64SecurityOffset)
    {
        this->emu->process.is_wow64_process = true;
        ASSERT_EQ(STATUS_SUCCESS, this->create_section(0x10000));
        view_attributes view{};
        ASSERT_EQ(STATUS_SUCCESS, this->create_view(1, view));
        const ALPC_DATA_VIEW_ATTR<EmulatorTraits<Emu32>> narrow{.Flags = 0x40000,
                                                                .SectionHandle = static_cast<uint32_t>(view.SectionHandle),
                                                                .ViewBase = static_cast<uint32_t>(view.ViewBase),
                                                                .ViewSize = static_cast<uint32_t>(view.ViewSize)};
        EXPECT_EQ(STATUS_SUCCESS, this->send_view(narrow));
        EXPECT_EQ(STATUS_SUCCESS, this->send_view(narrow, ALPC_MESSAGE_VIEW_ATTRIBUTE | ALPC_MESSAGE_SECURITY_ATTRIBUTE));
    }
}
