#include <gtest/gtest.h>

#include "macos_test_utils.hpp"

#include <dyld_process.hpp>
#include <serialization.hpp>

#include <string>
#include <vector>

namespace
{
    std::string read_guest_c_string(sogen::macos_emulator& emu, const uint64_t address)
    {
        std::string value{};
        for (uint64_t cursor = address;; ++cursor)
        {
            char character{};
            emu.memory.read_memory(cursor, &character, sizeof(character));
            if (character == '\0')
            {
                return value;
            }

            value.push_back(character);
        }
    }

    uint64_t read_guest_word(sogen::macos_emulator& emu, const uint64_t address)
    {
        uint64_t value{};
        emu.memory.read_memory(address, &value, sizeof(value));
        return value;
    }

    TEST(DyldStack, AppleStringsMatchTheTwelveEntriesTheKernelEmits)
    {
        sogen::macos_apple_strings apple{};
        apple.executable_path = "/bin/hello";
        apple.stack_guard = 0x0123456789ABCDEFull;
        apple.malloc_entropy = {0x1111111111111111ull, 0x2222222222222222ull};
        apple.ptr_munge = 0x3333333333333333ull;
        apple.executable_file.object_id = 0xB426014ull;
        apple.dyld_file.object_id = 0xB426015ull;
        apple.executable_cdhash = std::string(40, 'a');
        apple.executable_boothash = std::string(40, 'b');
        apple.th_port = 0x103;

        const auto rendered = apple.render();

        ASSERT_EQ(rendered.size(), 12u);
        EXPECT_EQ(rendered[0], "executable_path=/bin/hello");
        EXPECT_EQ(rendered[1], "pfz=0xfffffc000");
        EXPECT_EQ(rendered[2], "stack_guard=0x0123456789abcdef");
        EXPECT_EQ(rendered[3], "malloc_entropy=0x1111111111111111,0x2222222222222222");
        EXPECT_EQ(rendered[4], "ptr_munge=0x3333333333333333");
        EXPECT_EQ(rendered[5], "main_stack=0x16fc00000,0x800000,0x16f400000,0x800000");
        EXPECT_EQ(rendered[6], "executable_file=0x1a0100000f,0xb426014");
        EXPECT_EQ(rendered[7], "dyld_file=0x1a0100000f,0xb426015");
        EXPECT_EQ(rendered[8], "executable_cdhash=" + std::string(40, 'a'));
        EXPECT_EQ(rendered[9], "executable_boothash=" + std::string(40, 'b'));
        EXPECT_EQ(rendered[10], "th_port=0x103");
        EXPECT_EQ(rendered[11], "security_config=0x0");

        // pfz is the commpage base to the digit; that equality is the check that the reconstruction from
        // the measured 16-byte slot is right rather than merely plausible.
        EXPECT_EQ(rendered[1], "pfz=" + sogen::format_hex_key("", sogen::MACOS_COMMPAGE_BASE));
    }

    TEST(DyldStack, KernelArgsDecodeExactlyAsDyldsFindEnvpDoes)
    {
        const auto emu = macos_test::make_emulator();
        ASSERT_TRUE(emu->memory.allocate_memory(sogen::MACOS_MAIN_STACK_TOP - sogen::MACOS_MAIN_STACK_SIZE, sogen::MACOS_MAIN_STACK_SIZE,
                                                sogen::memory_permission::read_write));

        const std::vector<std::string> argv{"/bin/hello", "one", "two"};
        const std::vector<std::string> envp{"A=1", "B=2"};
        const std::vector<std::string> apple{"executable_path=/bin/hello", "pfz=0xfffffc000"};

        const auto layout =
            sogen::build_dyld_kernel_args(emu->memory, sogen::MACOS_MAIN_STACK_TOP,
                                          sogen::MACOS_MAIN_STACK_TOP - sogen::MACOS_MAIN_STACK_SIZE, 0x100000000ull, argv, envp, apple);

        ASSERT_TRUE(layout.valid);
        EXPECT_EQ(layout.stack_pointer % 16, 0u);

        EXPECT_EQ(read_guest_word(*emu, layout.stack_pointer), 0x100000000ull);
        EXPECT_EQ(read_guest_word(*emu, layout.stack_pointer + 8), argv.size());

        EXPECT_EQ(layout.argv, layout.stack_pointer + 16);
        EXPECT_EQ(layout.envp, layout.stack_pointer + 24 + 8 * argv.size());
        EXPECT_EQ(layout.apple, layout.envp + 8 * (envp.size() + 1));

        for (size_t i = 0; i < argv.size(); ++i)
        {
            EXPECT_EQ(read_guest_c_string(*emu, read_guest_word(*emu, layout.argv + 8 * i)), argv[i]);
        }

        EXPECT_EQ(read_guest_word(*emu, layout.argv + 8 * argv.size()), 0u);

        for (size_t i = 0; i < envp.size(); ++i)
        {
            EXPECT_EQ(read_guest_c_string(*emu, read_guest_word(*emu, layout.envp + 8 * i)), envp[i]);
        }

        EXPECT_EQ(read_guest_word(*emu, layout.envp + 8 * envp.size()), 0u);

        for (size_t i = 0; i < apple.size(); ++i)
        {
            EXPECT_EQ(read_guest_c_string(*emu, read_guest_word(*emu, layout.apple + 8 * i)), apple[i]);
        }

        EXPECT_EQ(read_guest_word(*emu, layout.apple + 8 * apple.size()), 0u);
    }

    TEST(DyldStack, RefusesToBuildWhenTheStackIsTooSmall)
    {
        const auto emu = macos_test::make_emulator();
        constexpr uint64_t tiny_base = 0x300000000ull;
        constexpr uint64_t tiny_top = tiny_base + 0x4000;

        ASSERT_TRUE(emu->memory.allocate_memory(tiny_base, 0x4000, sogen::memory_permission::read_write));

        const std::vector<std::string> argv{std::string(0x8000, 'x')};

        const auto layout = sogen::build_dyld_kernel_args(emu->memory, tiny_top, tiny_base, 0x100000000ull, argv, {}, {});
        EXPECT_FALSE(layout.valid);
        EXPECT_EQ(layout.stack_pointer, 0u);
    }

    // The pointer block sits below the string block and is written second, and the two are placed by
    // separate roundings (8 for the strings, 16 for the pointers). Whether they can ever meet depends
    // on the total string length mod 8 against the pointer base mod 16, so one fixed argv proves
    // nothing about the others; this sweeps every residue.
    TEST(DyldStack, ThePointerBlockNeverOverlapsTheStringsAtAnyLength)
    {
        for (size_t pad = 0; pad < 32; ++pad)
        {
            const auto emu = macos_test::make_emulator();
            ASSERT_TRUE(emu->memory.allocate_memory(sogen::MACOS_MAIN_STACK_TOP - sogen::MACOS_MAIN_STACK_SIZE,
                                                    sogen::MACOS_MAIN_STACK_SIZE, sogen::memory_permission::read_write));

            const std::vector<std::string> argv{std::string(pad, 'A') + "/bin/hello", "second"};
            const std::vector<std::string> envp{"A=1"};
            const std::vector<std::string> apple{"executable_path=/bin/hello"};

            const auto layout = sogen::build_dyld_kernel_args(emu->memory, sogen::MACOS_MAIN_STACK_TOP,
                                                              sogen::MACOS_MAIN_STACK_TOP - sogen::MACOS_MAIN_STACK_SIZE, 0x100000000ull,
                                                              argv, envp, apple);
            ASSERT_TRUE(layout.valid) << "pad=" << pad;

            for (size_t i = 0; i < argv.size(); ++i)
            {
                EXPECT_EQ(read_guest_c_string(*emu, read_guest_word(*emu, layout.argv + 8 * i)), argv[i])
                    << "pad=" << pad << " argv[" << i << "]";
            }

            EXPECT_EQ(read_guest_c_string(*emu, read_guest_word(*emu, layout.envp)), envp[0]) << "pad=" << pad;
            EXPECT_EQ(read_guest_c_string(*emu, read_guest_word(*emu, layout.apple)), apple[0]) << "pad=" << pad;

            // The strings live above every pointer slot the layout names, so the lowest string address
            // has to sit at or above the end of the pointer block.
            const auto pointer_block_end = layout.apple + 8 * (apple.size() + 1);
            for (size_t i = 0; i < argv.size(); ++i)
            {
                EXPECT_GE(read_guest_word(*emu, layout.argv + 8 * i), pointer_block_end) << "pad=" << pad;
            }
        }
    }

    TEST(DyldStack, SetupForDyldLeavesTheGuestExactlyAsTheKernelWould)
    {
        const auto emu = macos_test::make_emulator();

        constexpr uint64_t dyld_entry = 0x1052749C0ull;
        constexpr uint64_t executable_header = 0x100000000ull;

        ASSERT_TRUE(emu->process.setup_for_dyld(emu->emu(), emu->memory, dyld_entry, executable_header, {"/bin/hello", "arg"},
                                                {"PATH=/usr/bin"}, "/bin/hello"));

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::pc), dyld_entry);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::sp), emu->process.kernel_args_pointer);

        // dyld4::start takes prevDyldMH, dyldSharedCache and startTime in x1..x3 and branches on x3 being
        // zero to source its own start time. A stale register here sends it down the restart path.
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 0u);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x1), 0u);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x2), 0u);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x3), 0u);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x29), 0u);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x30), 0u);

        EXPECT_EQ(read_guest_word(*emu, emu->process.kernel_args_pointer), executable_header);
        EXPECT_EQ(read_guest_word(*emu, emu->process.kernel_args_pointer + 8), 2u);

        ASSERT_EQ(emu->process.apple.size(), 12u);
        EXPECT_EQ(emu->process.apple[0], "executable_path=/bin/hello");
        EXPECT_EQ(emu->process.executable_path, "/bin/hello");

        EXPECT_EQ(emu->process.stack_base, sogen::MACOS_MAIN_STACK_TOP - sogen::MACOS_MAIN_STACK_SIZE);
        EXPECT_EQ(emu->process.stack_size, sogen::MACOS_MAIN_STACK_SIZE);
        ASSERT_NE(emu->process.active_thread, nullptr);
    }

    TEST(DyldStack, MainStackEntryAgreesWithTheReportedUserStack)
    {
        const auto emu = macos_test::make_emulator();

        ASSERT_TRUE(emu->process.setup_for_dyld(emu->emu(), emu->memory, 0x1052749C0ull, 0x100000000ull, {"/bin/hello"}, {}, "/bin/hello"));

        // libpthread parses main_stack= and otherwise falls back to sysctl kern.usrstack64; the two must
        // not disagree or the main thread gets a stack the runtime does not believe in.
        EXPECT_EQ(emu->process.apple_strings.main_stack_top, sogen::MACOS_MAIN_STACK_TOP);
        EXPECT_EQ(emu->process.apple[5], "main_stack=0x16fc00000,0x800000,0x16f400000,0x800000");
    }

    // Through macos_emulator::serialize, not macos_process_context::serialize: a snapshot that drops
    // the cookies hands the restored guest a different stack_guard and ptr_munge than the ones already
    // baked into its apple[] strings and its live stack.
    TEST(DyldStack, TheDyldSetupSurvivesAnEmulatorSnapshot)
    {
        const auto emu = macos_test::make_emulator();
        ASSERT_TRUE(emu->process.setup_for_dyld(emu->emu(), emu->memory, 0x1052749C0ull, 0x100000000ull, {"/bin/hello", "arg"},
                                                {"PATH=/usr/bin"}, "/bin/hello"));
        emu->process.apple_strings.th_port = 0x1703;

        sogen::utils::buffer_serializer serializer{};
        emu->serialize(serializer, false);

        const auto restored = macos_test::make_emulator();
        sogen::utils::buffer_deserializer deserializer{serializer.get_buffer()};
        restored->deserialize(deserializer, false);

        EXPECT_EQ(restored->process.kernel_args_pointer, emu->process.kernel_args_pointer);
        EXPECT_EQ(restored->process.apple_strings.executable_path, "/bin/hello");
        EXPECT_EQ(restored->process.apple_strings.stack_guard, emu->process.apple_strings.stack_guard);
        EXPECT_EQ(restored->process.apple_strings.ptr_munge, emu->process.apple_strings.ptr_munge);
        EXPECT_EQ(restored->process.apple_strings.malloc_entropy, emu->process.apple_strings.malloc_entropy);
        EXPECT_EQ(restored->process.apple_strings.th_port, 0x1703u);
        EXPECT_NE(restored->process.apple_strings.stack_guard, 0u);

        EXPECT_EQ(read_guest_word(*restored, restored->process.kernel_args_pointer), 0x100000000ull);
        EXPECT_EQ(restored->emu().reg(sogen::arm64_register::sp), emu->process.kernel_args_pointer);
    }

    // Reproducibility is the point of deriving the cookies rather than drawing them: two runs of one
    // sample have to lay out identical stacks, and two different samples must not share a guard.
    TEST(DyldStack, TheCookiesAreReproducibleAcrossRunsAndDifferPerExecutable)
    {
        const auto first = macos_test::make_emulator();
        const auto again = macos_test::make_emulator();
        const auto other = macos_test::make_emulator();

        ASSERT_TRUE(
            first->process.setup_for_dyld(first->emu(), first->memory, 0x1000ull, 0x100000000ull, {"/bin/hello"}, {}, "/bin/hello"));
        ASSERT_TRUE(
            again->process.setup_for_dyld(again->emu(), again->memory, 0x1000ull, 0x100000000ull, {"/bin/hello"}, {}, "/bin/hello"));
        ASSERT_TRUE(
            other->process.setup_for_dyld(other->emu(), other->memory, 0x1000ull, 0x100000000ull, {"/bin/other"}, {}, "/bin/other"));

        EXPECT_EQ(first->process.apple, again->process.apple);
        EXPECT_EQ(first->process.apple_strings.stack_guard, again->process.apple_strings.stack_guard);
        EXPECT_NE(first->process.apple_strings.stack_guard, other->process.apple_strings.stack_guard);
        EXPECT_NE(first->process.apple_strings.ptr_munge, other->process.apple_strings.ptr_munge);
        EXPECT_NE(first->process.apple_strings.executable_cdhash, other->process.apple_strings.executable_cdhash);

        EXPECT_EQ(first->process.apple_strings.executable_cdhash.size(), 40u);
        EXPECT_EQ(first->process.apple_strings.executable_boothash.size(), 40u);
        EXPECT_NE(first->process.apple_strings.executable_cdhash, first->process.apple_strings.executable_boothash);
    }

    // libSystem's cerror stub runs on every failing syscall and does
    //   mrs x8, TPIDRRO_EL0 ; ldr x8, [x8, #8] ; cbz x8, skip ; str w0, [x8]
    // so a zero thread pointer turns the first failed syscall into a read of address 8. Real dyld hit
    // exactly that, 202,547 instructions in, on the EPERM from csrctl.
    TEST(DyldStack, TheThreadPointerIsSetSoTheCerrorStubCanRead)
    {
        const auto emu = macos_test::make_emulator();
        ASSERT_TRUE(emu->process.setup_for_dyld(emu->emu(), emu->memory, 0x1052749C0ull, 0x100000000ull, {"/bin/hello"}, {}, "/bin/hello"));

        const auto thread_pointer = emu->emu().reg(sogen::arm64_register::tpidrro_el0);
        ASSERT_NE(thread_pointer, 0u);
        EXPECT_EQ(thread_pointer, emu->process.active_thread->thread_self);

        uint64_t errno_slot{};
        EXPECT_TRUE(emu->memory.try_read_memory(thread_pointer + 8, &errno_slot, sizeof(errno_slot)))
            << "the cerror stub reads this unconditionally";
        EXPECT_EQ(errno_slot, 0u) << "a zero here makes cerror take its skip branch, as a thread with no TSD would";

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::tpidr_el0), 0u) << "TPIDR_EL0 belongs to the process, not the kernel";

        // libpthread takes its own struct to begin 0xE0 below the pointer and writes there:
        //   mrs x8, TPIDRRO_EL0 ; subs x21, x8, #224 ; str x8, [x21]
        // A thread pointer that names the stack bottom -- which is what Stage 3 used as a placeholder --
        // puts that write below the stack, where nothing is mapped.
        const auto pthread_struct = thread_pointer - sogen::MACOS_PTHREAD_STRUCT_TO_TSD_OFFSET;
        uint64_t signature = 0xA5A5A5A5A5A5A5A5ULL;
        EXPECT_TRUE(emu->memory.try_write_memory(pthread_struct, &signature, sizeof(signature)))
            << "libpthread writes its struct signature here before anything else runs";

        EXPECT_GE(pthread_struct, sogen::MACOS_MAIN_THREAD_STATE_BASE);
        EXPECT_LT(thread_pointer, sogen::MACOS_MAIN_THREAD_STATE_BASE + sogen::MACOS_MAIN_THREAD_STATE_SIZE);

        // The TSD slots live above the pointer, so the page has to extend both ways around it.
        EXPECT_TRUE(emu->memory.try_read_memory(thread_pointer + 0x200, &signature, sizeof(signature)));

        EXPECT_LT(thread_pointer, emu->process.stack_base) << "the thread state is its own page, not part of the stack";
    }
}
