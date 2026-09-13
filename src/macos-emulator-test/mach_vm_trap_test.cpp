#include <gtest/gtest.h>

#include "macos_test_utils.hpp"

#include <mach/mach_traps.hpp>
#include <mach/mach_types.hpp>

#include <guest/guest_memory_object.hpp>

#include <vector>

namespace
{
    constexpr uint64_t code_base = 0x100000000ULL;
    constexpr uint64_t carry = 0x20000000ULL;

    // Every word below is the first instruction of the matching stub in this host's libsystem_kernel.
    constexpr uint32_t movn_x16_vm_allocate = 0x92800130;   // mov x16, #-10
    constexpr uint32_t movn_x16_vm_deallocate = 0x92800170; // mov x16, #-12
    constexpr uint32_t movn_x16_vm_protect = 0x928001B0;    // mov x16, #-14
    constexpr uint32_t movn_x16_vm_map = 0x928001D0;        // mov x16, #-15
    constexpr uint32_t cset_x3_cs = 0x9A9F37E3;
    constexpr uint32_t svc_80 = 0xD4001001;

    // One page clear of code_base, so a test that also writes a program by hand never shares a page with
    // the runner's.
    constexpr uint64_t trap_code_base = code_base + sogen::MACOS_PAGE_SIZE;
    constexpr uint64_t scratch = 0x340000000ULL;
    constexpr uint64_t fixed_target = 0x380000000ULL;
    constexpr uint64_t unmapped = 0x700000000ULL;

    // The first base an unhinted search hands out, once the page below has been taken.
    constexpr uint64_t first_free_base = 0x300000000ULL;
    constexpr uint64_t second_free_base = first_free_base + sogen::MACOS_PAGE_SIZE;

    constexpr uint32_t VM_FLAGS_FIXED = 0x0;
    constexpr uint32_t VM_FLAGS_ANYWHERE = 0x1;
    constexpr uint32_t VM_FLAGS_OVERWRITE = 0x4000;
    constexpr uint32_t VM_MEMORY_MALLOC_TAG = 0x01000000;

    constexpr uint32_t VM_PROT_NONE = 0x0;
    constexpr uint32_t VM_PROT_READ = 0x1;
    constexpr uint32_t VM_PROT_WRITE = 0x2;
    constexpr uint32_t VM_PROT_EXECUTE = 0x4;

    constexpr uint64_t MACH_SEND_INVALID_DEST = 0x10000003ULL;
    constexpr uint64_t alignment_mask = 0xFFFFFULL;

    constexpr uint64_t kr(const sogen::mach::kern_return_t value)
    {
        return static_cast<uint64_t>(static_cast<uint32_t>(value));
    }

    void put(sogen::macos_emulator& emu, const uint64_t address, const uint64_t value)
    {
        emu.memory.write_memory(address, &value, sizeof(value));
    }

    uint64_t get(sogen::macos_emulator& emu, const uint64_t address)
    {
        uint64_t value = 0;
        emu.memory.read_memory(address, &value, sizeof(value));
        return value;
    }

    // The handlers are reached the way the guest reaches them - a real trap word plus svc - rather than by
    // calling them with a hand-built context, so a mis-registered index shows up here too.
    //
    // Each program gets its own page: unicorn keeps the blocks it has already translated, so a second
    // program written over the first at the same address silently re-executes the first one.
    class trap_runner
    {
      public:
        explicit trap_runner(sogen::macos_emulator& emu)
            : emu_(&emu)
        {
        }

        uint64_t operator()(const uint32_t trap_word, const std::vector<uint64_t>& arguments)
        {
            std::vector<uint32_t> words{};
            for (uint32_t reg = 0; reg < arguments.size(); ++reg)
            {
                macos_test::load_x(words, reg, arguments[reg]);
            }

            words.push_back(trap_word);
            words.push_back(svc_80);

            macos_test::write_guest_code(*this->emu_, this->next_base_, words);
            this->next_base_ += sogen::MACOS_PAGE_SIZE;
            this->emu_->start(words.size());

            return this->emu_->emu().reg(sogen::arm64_register::x0);
        }

      private:
        sogen::macos_emulator* emu_;
        uint64_t next_base_{trap_code_base};
    };

    TEST(MachVmTraps, AllocateAnywhereReturnsUsableGuestMemory)
    {
        const auto emu = macos_test::make_emulator();
        ASSERT_TRUE(emu->memory.allocate_memory(scratch, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        constexpr uint64_t size = 3 * sogen::MACOS_PAGE_SIZE;

        trap_runner run{*emu};
        EXPECT_EQ(run(movn_x16_vm_allocate, {emu->mach.task_self, scratch, size, VM_FLAGS_ANYWHERE}), kr(sogen::mach::kr::success));

        const auto address = get(*emu, scratch);
        EXPECT_NE(address, 0u);
        EXPECT_EQ(address % sogen::MACOS_PAGE_SIZE, 0u);

        const auto info = emu->memory.get_region_info(address);
        ASSERT_TRUE(info.has_value());
        EXPECT_EQ(info->length, size);
        EXPECT_EQ(info->permissions, sogen::memory_permission::read_write);

        constexpr uint64_t probe = 0x1122334455667788ULL;
        const auto last_word = address + size - sizeof(probe);
        put(*emu, last_word, probe);
        EXPECT_EQ(get(*emu, last_word), probe);
    }

    TEST(MachVmTraps, AllocateRoundsAPartialPageUpToAWholePage)
    {
        const auto emu = macos_test::make_emulator();
        ASSERT_TRUE(emu->memory.allocate_memory(scratch, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        trap_runner run{*emu};
        EXPECT_EQ(run(movn_x16_vm_allocate, {emu->mach.task_self, scratch, sogen::MACOS_PAGE_SIZE + 1, VM_FLAGS_ANYWHERE}),
                  kr(sogen::mach::kr::success));

        const auto info = emu->memory.get_region_info(get(*emu, scratch));
        ASSERT_TRUE(info.has_value());
        EXPECT_EQ(info->length, 2 * sogen::MACOS_PAGE_SIZE);
    }

    TEST(MachVmTraps, AllocateAnywhereNeverHandsOutTheSameRangeTwice)
    {
        const auto emu = macos_test::make_emulator();
        ASSERT_TRUE(emu->memory.allocate_memory(scratch, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        trap_runner run{*emu};

        ASSERT_EQ(run(movn_x16_vm_allocate, {emu->mach.task_self, scratch, sogen::MACOS_PAGE_SIZE, VM_FLAGS_ANYWHERE}),
                  kr(sogen::mach::kr::success));
        const auto first = get(*emu, scratch);

        ASSERT_EQ(run(movn_x16_vm_allocate, {emu->mach.task_self, scratch, sogen::MACOS_PAGE_SIZE, VM_FLAGS_ANYWHERE}),
                  kr(sogen::mach::kr::success));
        const auto second = get(*emu, scratch);

        EXPECT_NE(first, second);
        EXPECT_GE(second, first + sogen::MACOS_PAGE_SIZE);
    }

    TEST(MachVmTraps, AllocateAcceptsAnAllocationTagInTheHighFlagBits)
    {
        const auto emu = macos_test::make_emulator();
        ASSERT_TRUE(emu->memory.allocate_memory(scratch, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        trap_runner run{*emu};
        EXPECT_EQ(
            run(movn_x16_vm_allocate, {emu->mach.task_self, scratch, sogen::MACOS_PAGE_SIZE, VM_FLAGS_ANYWHERE | VM_MEMORY_MALLOC_TAG}),
            kr(sogen::mach::kr::success))
            << "dyld puts VM_MAKE_TAG in the top byte of flags; rejecting unknown flag bits would refuse every one of its "
               "allocations";

        EXPECT_TRUE(emu->memory.get_region_info(get(*emu, scratch)).has_value());
    }

    TEST(MachVmTraps, AllocateWritesTheAlignedBaseBackForAFixedRequest)
    {
        const auto emu = macos_test::make_emulator();
        ASSERT_TRUE(emu->memory.allocate_memory(scratch, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        put(*emu, scratch, fixed_target + 0x10);

        trap_runner run{*emu};
        EXPECT_EQ(run(movn_x16_vm_allocate, {emu->mach.task_self, scratch, sogen::MACOS_PAGE_SIZE, VM_FLAGS_FIXED}),
                  kr(sogen::mach::kr::success));

        EXPECT_EQ(get(*emu, scratch), fixed_target);
        EXPECT_TRUE(emu->memory.get_region_info(fixed_target).has_value());
    }

    TEST(MachVmTraps, AllocateFixedRefusesAnOccupiedRange)
    {
        const auto emu = macos_test::make_emulator();
        ASSERT_TRUE(emu->memory.allocate_memory(scratch, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        ASSERT_TRUE(emu->memory.allocate_memory(fixed_target, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read));
        put(*emu, scratch, fixed_target);

        trap_runner run{*emu};
        EXPECT_EQ(run(movn_x16_vm_allocate, {emu->mach.task_self, scratch, sogen::MACOS_PAGE_SIZE, VM_FLAGS_FIXED}),
                  kr(sogen::mach::kr::no_space));

        const auto info = emu->memory.get_region_info(fixed_target);
        ASSERT_TRUE(info.has_value());
        EXPECT_EQ(info->permissions, sogen::memory_permission::read);
    }

    TEST(MachVmTraps, AllocateFixedWithOverwriteReplacesTheRange)
    {
        const auto emu = macos_test::make_emulator();
        ASSERT_TRUE(emu->memory.allocate_memory(scratch, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        ASSERT_TRUE(emu->memory.allocate_memory(fixed_target, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read));
        put(*emu, fixed_target, 0xFEEDFACECAFEBEEFULL);
        put(*emu, scratch, fixed_target);

        trap_runner run{*emu};
        EXPECT_EQ(run(movn_x16_vm_allocate, {emu->mach.task_self, scratch, sogen::MACOS_PAGE_SIZE, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE}),
                  kr(sogen::mach::kr::success));

        const auto info = emu->memory.get_region_info(fixed_target);
        ASSERT_TRUE(info.has_value());
        EXPECT_EQ(info->permissions, sogen::memory_permission::read_write);
        EXPECT_EQ(get(*emu, fixed_target), 0u);
    }

    // The sentinel is deliberately not page aligned: a write-back of the aligned base would be
    // indistinguishable from no write at all if the guest had asked for an aligned address.
    TEST(MachVmTraps, AllocateLeavesTheAddressPointerUntouchedWhenItFails)
    {
        const auto emu = macos_test::make_emulator();
        ASSERT_TRUE(emu->memory.allocate_memory(scratch, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        put(*emu, scratch, sogen::MACOS_COMMPAGE_NESTING_START + 0x10);

        trap_runner run{*emu};
        ASSERT_NE(run(movn_x16_vm_allocate, {emu->mach.task_self, scratch, sogen::MACOS_PAGE_SIZE, VM_FLAGS_FIXED}),
                  kr(sogen::mach::kr::success));

        EXPECT_EQ(get(*emu, scratch), sogen::MACOS_COMMPAGE_NESTING_START + 0x10);
    }

    TEST(MachVmTraps, AllocateAtAFixedAddressInsideAReservedRangeFails)
    {
        const auto emu = macos_test::make_emulator();
        ASSERT_TRUE(emu->memory.allocate_memory(scratch, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        put(*emu, scratch, sogen::MACOS_COMMPAGE_NESTING_START);

        trap_runner run{*emu};
        EXPECT_EQ(run(movn_x16_vm_allocate, {emu->mach.task_self, scratch, sogen::MACOS_PAGE_SIZE, VM_FLAGS_FIXED}),
                  kr(sogen::mach::kr::no_space))
            << "the commpage nesting region must never be handed to a fixed allocation";

        EXPECT_FALSE(emu->memory.get_region_info(sogen::MACOS_COMMPAGE_NESTING_START).has_value());
    }

    TEST(MachVmTraps, AllocateRejectsASizeThatOverflowsPageRounding)
    {
        const auto emu = macos_test::make_emulator();
        ASSERT_TRUE(emu->memory.allocate_memory(scratch, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        trap_runner run{*emu};
        EXPECT_EQ(run(movn_x16_vm_allocate, {emu->mach.task_self, scratch, ~uint64_t{0}, VM_FLAGS_ANYWHERE}),
                  kr(sogen::mach::kr::invalid_argument));
    }

    TEST(MachVmTraps, AllocateRefusesASizeLargerThanTheAddressSpace)
    {
        const auto emu = macos_test::make_emulator();
        ASSERT_TRUE(emu->memory.allocate_memory(scratch, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        trap_runner run{*emu};
        EXPECT_EQ(run(movn_x16_vm_allocate,
                      {emu->mach.task_self, scratch, sogen::MACOS_MAX_MMAP_END_EXCL + sogen::MACOS_PAGE_SIZE, VM_FLAGS_ANYWHERE}),
                  kr(sogen::mach::kr::no_space));
    }

    TEST(MachVmTraps, AllocateReportsMemoryErrorForAnUnreadableAddressPointer)
    {
        const auto emu = macos_test::make_emulator();

        trap_runner run{*emu};
        EXPECT_EQ(run(movn_x16_vm_allocate, {emu->mach.task_self, unmapped, sogen::MACOS_PAGE_SIZE, VM_FLAGS_ANYWHERE}),
                  kr(sogen::mach::kr::memory_error));
    }

    // Measured against _kernelrpc_mach_vm_allocate_trap on macOS 26.6.1: a zero size succeeds and stores a
    // zero address, while the same size through _kernelrpc_mach_vm_map_trap is KERN_INVALID_ARGUMENT.
    TEST(MachVmTraps, AZeroSizeSucceedsForAllocateAndFailsForMap)
    {
        const auto emu = macos_test::make_emulator();
        ASSERT_TRUE(emu->memory.allocate_memory(scratch, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        put(*emu, scratch, 0xDEADBEEFULL);

        trap_runner run{*emu};
        EXPECT_EQ(run(movn_x16_vm_allocate, {emu->mach.task_self, scratch, 0, VM_FLAGS_ANYWHERE}), kr(sogen::mach::kr::success));
        EXPECT_EQ(get(*emu, scratch), 0u);

        put(*emu, scratch, 0xDEADBEEFULL);
        EXPECT_EQ(run(movn_x16_vm_map, {emu->mach.task_self, scratch, 0, 0, VM_FLAGS_ANYWHERE, VM_PROT_READ}),
                  kr(sogen::mach::kr::invalid_argument));
        EXPECT_EQ(get(*emu, scratch), 0xDEADBEEFULL);
    }

    TEST(MachVmTraps, DeallocateReleasesOnlyTheRequestedPages)
    {
        const auto emu = macos_test::make_emulator();
        const auto address = emu->memory.allocate_memory(2 * sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);
        ASSERT_NE(address, 0u);

        trap_runner run{*emu};
        EXPECT_EQ(run(movn_x16_vm_deallocate, {emu->mach.task_self, address, sogen::MACOS_PAGE_SIZE}), kr(sogen::mach::kr::success));

        EXPECT_FALSE(emu->memory.get_region_info(address).has_value());

        const auto tail = emu->memory.get_region_info(address + sogen::MACOS_PAGE_SIZE);
        ASSERT_TRUE(tail.has_value());
        EXPECT_EQ(tail->length, sogen::MACOS_PAGE_SIZE);
    }

    // Darwin's vm_map_remove reports success for a range that holds no entries, measured against
    // _kernelrpc_mach_vm_deallocate_trap on macOS 26.6.1; a guest that unmaps twice must not see a failure.
    TEST(MachVmTraps, DeallocateSucceedsOnARangeThatWasNeverMapped)
    {
        const auto emu = macos_test::make_emulator();

        trap_runner run{*emu};
        EXPECT_EQ(run(movn_x16_vm_deallocate, {emu->mach.task_self, unmapped, sogen::MACOS_PAGE_SIZE}), kr(sogen::mach::kr::success));
    }

    TEST(MachVmTraps, DeallocateRejectsASizeThatOverflowsPageRounding)
    {
        const auto emu = macos_test::make_emulator();
        const auto address = emu->memory.allocate_memory(sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);
        ASSERT_NE(address, 0u);

        trap_runner run{*emu};
        EXPECT_EQ(run(movn_x16_vm_deallocate, {emu->mach.task_self, address, ~uint64_t{0}}), kr(sogen::mach::kr::invalid_argument));
        EXPECT_TRUE(emu->memory.get_region_info(address).has_value());
    }

    TEST(MachVmTraps, ProtectNarrowsOnlyTheRequestedPages)
    {
        const auto emu = macos_test::make_emulator();
        const auto address = emu->memory.allocate_memory(2 * sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);
        ASSERT_NE(address, 0u);

        trap_runner run{*emu};
        EXPECT_EQ(run(movn_x16_vm_protect, {emu->mach.task_self, address, sogen::MACOS_PAGE_SIZE, 0, VM_PROT_READ}),
                  kr(sogen::mach::kr::success));

        const auto head = emu->memory.get_region_info(address);
        ASSERT_TRUE(head.has_value());
        EXPECT_EQ(head->permissions, sogen::memory_permission::read);

        const auto tail = emu->memory.get_region_info(address + sogen::MACOS_PAGE_SIZE);
        ASSERT_TRUE(tail.has_value());
        EXPECT_EQ(tail->permissions, sogen::memory_permission::read_write);
    }

    TEST(MachVmTraps, ProtectAppliesTheExecuteBit)
    {
        const auto emu = macos_test::make_emulator();
        const auto address = emu->memory.allocate_memory(sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);
        ASSERT_NE(address, 0u);

        trap_runner run{*emu};
        EXPECT_EQ(run(movn_x16_vm_protect, {emu->mach.task_self, address, sogen::MACOS_PAGE_SIZE, 0, VM_PROT_READ | VM_PROT_EXECUTE}),
                  kr(sogen::mach::kr::success));

        const auto info = emu->memory.get_region_info(address);
        ASSERT_TRUE(info.has_value());
        EXPECT_EQ(info->permissions, sogen::memory_permission::read_exec);
    }

    TEST(MachVmTraps, ProtectReportsInvalidAddressForAnUnmappedRange)
    {
        const auto emu = macos_test::make_emulator();

        trap_runner run{*emu};
        EXPECT_EQ(run(movn_x16_vm_protect, {emu->mach.task_self, unmapped, sogen::MACOS_PAGE_SIZE, 0, VM_PROT_READ}),
                  kr(sogen::mach::kr::invalid_address));
    }

    TEST(MachVmTraps, MapAppliesTheCurrentProtection)
    {
        const auto emu = macos_test::make_emulator();
        ASSERT_TRUE(emu->memory.allocate_memory(scratch, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        trap_runner run{*emu};
        EXPECT_EQ(run(movn_x16_vm_map,
                      {emu->mach.task_self, scratch, sogen::MACOS_PAGE_SIZE, 0, VM_FLAGS_ANYWHERE, VM_PROT_READ | VM_PROT_EXECUTE}),
                  kr(sogen::mach::kr::success));

        const auto info = emu->memory.get_region_info(get(*emu, scratch));
        ASSERT_TRUE(info.has_value());
        EXPECT_EQ(info->permissions, sogen::memory_permission::read_exec);
    }

    TEST(MachVmTraps, MapWithoutAMaskReturnsTheFirstFreeBase)
    {
        const auto emu = macos_test::make_emulator();
        ASSERT_TRUE(emu->memory.allocate_memory(scratch, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        ASSERT_TRUE(emu->memory.allocate_memory(first_free_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        trap_runner run{*emu};
        ASSERT_EQ(run(movn_x16_vm_map, {emu->mach.task_self, scratch, sogen::MACOS_PAGE_SIZE, 0, VM_FLAGS_ANYWHERE, VM_PROT_READ}),
                  kr(sogen::mach::kr::success));

        const auto base = get(*emu, scratch);
        EXPECT_EQ(base, second_free_base);
        EXPECT_NE(base & alignment_mask, 0u) << "MapHonoursTheAlignmentMask is only meaningful while the unmasked base is misaligned";
    }

    TEST(MachVmTraps, MapHonoursTheAlignmentMask)
    {
        const auto emu = macos_test::make_emulator();
        ASSERT_TRUE(emu->memory.allocate_memory(scratch, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        ASSERT_TRUE(emu->memory.allocate_memory(first_free_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        trap_runner run{*emu};
        ASSERT_EQ(
            run(movn_x16_vm_map, {emu->mach.task_self, scratch, sogen::MACOS_PAGE_SIZE, alignment_mask, VM_FLAGS_ANYWHERE, VM_PROT_READ}),
            kr(sogen::mach::kr::success));

        const auto base = get(*emu, scratch);
        EXPECT_EQ(base & alignment_mask, 0u);
        EXPECT_NE(base, second_free_base);
        EXPECT_TRUE(emu->memory.get_region_info(base).has_value());
    }

    TEST(MachVmTraps, MapRejectsAMaskNoAddressCanSatisfy)
    {
        const auto emu = macos_test::make_emulator();
        ASSERT_TRUE(emu->memory.allocate_memory(scratch, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        trap_runner run{*emu};
        EXPECT_EQ(
            run(movn_x16_vm_map, {emu->mach.task_self, scratch, sogen::MACOS_PAGE_SIZE, ~uint64_t{0}, VM_FLAGS_ANYWHERE, VM_PROT_READ}),
            kr(sogen::mach::kr::no_space));
    }

    TEST(MachVmTraps, MapReportsMemoryErrorForAnUnreadableAddressPointer)
    {
        const auto emu = macos_test::make_emulator();

        trap_runner run{*emu};
        EXPECT_EQ(run(movn_x16_vm_map, {emu->mach.task_self, unmapped, sogen::MACOS_PAGE_SIZE, 0, VM_FLAGS_ANYWHERE, VM_PROT_READ}),
                  kr(sogen::mach::kr::memory_error));
    }

    // Measured on macOS 26.6.1: every one of the four traps answers a target that is not the caller's own
    // task with MACH_SEND_INVALID_DEST rather than a kern_return_t.
    TEST(MachVmTraps, EveryVmTrapRefusesATargetThatIsNotTheTaskPort)
    {
        const auto emu = macos_test::make_emulator();
        ASSERT_TRUE(emu->memory.allocate_memory(scratch, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        const auto victim = emu->memory.allocate_memory(sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);
        ASSERT_NE(victim, 0u);
        ASSERT_NE(emu->mach.host_self, emu->mach.task_self);

        trap_runner run{*emu};

        EXPECT_EQ(run(movn_x16_vm_allocate, {emu->mach.host_self, scratch, sogen::MACOS_PAGE_SIZE, VM_FLAGS_ANYWHERE}),
                  MACH_SEND_INVALID_DEST);
        EXPECT_EQ(get(*emu, scratch), 0u);

        EXPECT_EQ(run(movn_x16_vm_map, {emu->mach.host_self, scratch, sogen::MACOS_PAGE_SIZE, 0, VM_FLAGS_ANYWHERE, VM_PROT_READ}),
                  MACH_SEND_INVALID_DEST);
        EXPECT_EQ(get(*emu, scratch), 0u);

        EXPECT_EQ(run(movn_x16_vm_protect, {emu->mach.host_self, victim, sogen::MACOS_PAGE_SIZE, 0, VM_PROT_READ}), MACH_SEND_INVALID_DEST);

        const auto untouched = emu->memory.get_region_info(victim);
        ASSERT_TRUE(untouched.has_value());
        EXPECT_EQ(untouched->permissions, sogen::memory_permission::read_write);

        EXPECT_EQ(run(movn_x16_vm_deallocate, {emu->mach.host_self, victim, sogen::MACOS_PAGE_SIZE}), MACH_SEND_INVALID_DEST);
        EXPECT_TRUE(emu->memory.get_region_info(victim).has_value());
    }

    TEST(MachVmTraps, NoVmTrapCanReachTheCommpage)
    {
        const auto emu = macos_test::make_emulator();
        ASSERT_TRUE(emu->memory.allocate_memory(scratch, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        put(*emu, scratch, sogen::MACOS_COMMPAGE_BASE);

        const auto before = get(*emu, sogen::MACOS_COMMPAGE_BASE);

        trap_runner run{*emu};

        run(movn_x16_vm_deallocate, {emu->mach.task_self, sogen::MACOS_COMMPAGE_BASE, sogen::MACOS_COMMPAGE_MAP_SIZE});
        ASSERT_TRUE(emu->memory.get_region_info(sogen::MACOS_COMMPAGE_BASE).has_value());

        EXPECT_EQ(run(movn_x16_vm_protect,
                      {emu->mach.task_self, sogen::MACOS_COMMPAGE_BASE, sogen::MACOS_COMMPAGE_MAP_SIZE, 0, VM_PROT_READ | VM_PROT_WRITE}),
                  kr(sogen::mach::kr::invalid_address));

        EXPECT_EQ(run(movn_x16_vm_map, {emu->mach.task_self, scratch, sogen::MACOS_COMMPAGE_MAP_SIZE, 0,
                                        VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, VM_PROT_READ | VM_PROT_WRITE}),
                  kr(sogen::mach::kr::no_space));

        const auto info = emu->memory.get_region_info(sogen::MACOS_COMMPAGE_BASE);
        ASSERT_TRUE(info.has_value());
        EXPECT_EQ(info->permissions, sogen::memory_permission::read);
        EXPECT_EQ(get(*emu, sogen::MACOS_COMMPAGE_BASE), before);
    }

    TEST(MachVmTraps, AFailingVmTrapLeavesTheCarryFlagClear)
    {
        const auto emu = macos_test::make_emulator();
        emu->emu().reg(sogen::arm64_register::nzcv, carry);

        std::vector<uint32_t> words{};
        macos_test::load_x(words, 0, emu->mach.task_self);
        macos_test::load_x(words, 1, unmapped);
        macos_test::load_x(words, 2, sogen::MACOS_PAGE_SIZE);
        macos_test::load_x(words, 3, 0);
        macos_test::load_x(words, 4, VM_PROT_READ);
        words.push_back(movn_x16_vm_protect);
        words.push_back(svc_80);
        words.push_back(cset_x3_cs);

        macos_test::write_guest_code(*emu, code_base, words);
        emu->start(words.size());

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), kr(sogen::mach::kr::invalid_address));
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x3), 0u)
            << "a failing mach trap reports through x0 only; libsyscall's vm trap stubs have no cerror branch";
    }

    TEST(MachVmTraps, VmProtToPermissionMapsEveryProtectionBit)
    {
        using sogen::mach_traps::vm_prot_to_permission;

        EXPECT_EQ(vm_prot_to_permission(VM_PROT_NONE), sogen::memory_permission::none);
        EXPECT_EQ(vm_prot_to_permission(VM_PROT_READ), sogen::memory_permission::read);
        EXPECT_EQ(vm_prot_to_permission(VM_PROT_WRITE), sogen::memory_permission::write);
        EXPECT_EQ(vm_prot_to_permission(VM_PROT_EXECUTE), sogen::memory_permission::exec);
        EXPECT_EQ(vm_prot_to_permission(VM_PROT_READ | VM_PROT_WRITE), sogen::memory_permission::read_write);
        EXPECT_EQ(vm_prot_to_permission(VM_PROT_READ | VM_PROT_EXECUTE), sogen::memory_permission::read_exec);
        EXPECT_EQ(vm_prot_to_permission(VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE), sogen::memory_permission::all);

        EXPECT_EQ(vm_prot_to_permission(0xFFFFFFF8u), sogen::memory_permission::none);
        EXPECT_EQ(vm_prot_to_permission(VM_PROT_READ | 0x40u), sogen::memory_permission::read);
    }
}
