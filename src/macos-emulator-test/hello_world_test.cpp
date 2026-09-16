#include <gtest/gtest.h>

#include "macho_fixture.hpp"
#include "macos_test_utils.hpp"

#include <utils/io.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <random>
#include <string>

namespace
{
    constexpr uint64_t scratch_base = 0x300000000ULL;

    std::filesystem::path make_unique_fixture_path()
    {
        static std::atomic_uint64_t counter{0};

        std::random_device device{};
        const auto stamp = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        const auto salt = (static_cast<uint64_t>(device()) << 32) ^ device();

        return std::filesystem::temp_directory_path() /
               ("sogen-hello-" + std::to_string(stamp) + "-" + std::to_string(salt) + "-" + std::to_string(counter++));
    }

    struct macho_on_disk
    {
        std::filesystem::path path{};

        explicit macho_on_disk(const std::vector<std::byte>& image)
            : path(make_unique_fixture_path())
        {
            sogen::utils::io::write_file(this->path, image);
        }

        macho_on_disk()
            : macho_on_disk(macos_test::build_hello_world_macho())
        {
        }

        ~macho_on_disk()
        {
            std::error_code error{};
            std::filesystem::remove(this->path, error);
        }

        macho_on_disk(const macho_on_disk&) = delete;
        macho_on_disk& operator=(const macho_on_disk&) = delete;
        macho_on_disk(macho_on_disk&&) = delete;
        macho_on_disk& operator=(macho_on_disk&&) = delete;
    };

    template <typename T>
    T read_image_value(const std::vector<std::byte>& image, const size_t offset)
    {
        T value{};
        std::memcpy(&value, image.data() + offset, sizeof(value));
        return value;
    }

    TEST(MacosHelloWorld, TheFixtureIsAValidArm64MachO)
    {
        const auto image = macos_test::build_hello_world_macho();

        ASSERT_EQ(image.size(), macos_test::hello_text_size);

        EXPECT_EQ(read_image_value<uint32_t>(image, 0), 0xFEEDFACFU);
        EXPECT_EQ(read_image_value<uint32_t>(image, 4), 0x0100000CU);
        EXPECT_EQ(read_image_value<uint32_t>(image, 12), 2U);
        EXPECT_EQ(read_image_value<uint32_t>(image, 16), 3U);
        EXPECT_EQ(read_image_value<uint32_t>(image, 20), 432U);

        EXPECT_EQ(read_image_value<uint32_t>(image, 32), 0x19U);
        EXPECT_EQ(read_image_value<uint64_t>(image, 32 + 32), macos_test::hello_text_vmaddr);
        EXPECT_EQ(read_image_value<uint32_t>(image, 104), 0x19U);
        EXPECT_EQ(read_image_value<uint64_t>(image, 104 + 24), macos_test::hello_text_vmaddr);
        EXPECT_EQ(read_image_value<uint32_t>(image, 176), 0x5U);
        EXPECT_EQ(read_image_value<uint32_t>(image, 176 + 8), 6U);

        EXPECT_EQ(read_image_value<uint64_t>(image, macos_test::macho_entry_pc_offset),
                  macos_test::hello_text_vmaddr + macos_test::hello_code_offset);

        // Every word was re-assembled with clang -arch arm64 and dumped with otool -t; they are pinned
        // here because a wrong destination register (0xD2800300 is `mov x0, #24`, not `mov x16, #24`)
        // silently reroutes through the indirect syscall(2) path instead of failing.
        const std::array<uint32_t, 9> expected_code{
            0xD2800020U, 0x100003E1U, 0xD28001C2U, 0xD2800090U, 0xD4001001U, 0xD2800000U, 0xD2800030U, 0xD4001001U, 0xD4200000U,
        };

        for (size_t i = 0; i < expected_code.size(); ++i)
        {
            EXPECT_EQ(read_image_value<uint32_t>(image, static_cast<size_t>(macos_test::hello_code_offset) + i * sizeof(uint32_t)),
                      expected_code[i])
                << "instruction word " << i;
        }

        const std::string message{reinterpret_cast<const char*>(image.data()) + macos_test::hello_message_offset,
                                  macos_test::hello_message.size()};
        EXPECT_EQ(message, "Hello, sogen!\n");
    }

    TEST(MacosHelloWorld, WritesToStdoutAndExitsCleanly)
    {
        const macho_on_disk fixture{};
        const auto emu = macos_test::make_emulator();

        std::string captured{};
        emu->callbacks.on_stdout = [&](const std::string_view data) { captured.append(data); };

        std::string captured_stderr{};
        emu->callbacks.on_stderr = [&](const std::string_view data) { captured_stderr.append(data); };

        emu->load_application(fixture.path, {"/bin/hello"}, {});

        ASSERT_NE(emu->mod_manager.executable, nullptr);
        EXPECT_EQ(emu->mod_manager.executable->entry_point, macos_test::hello_text_vmaddr + macos_test::hello_code_offset);
        EXPECT_EQ(emu->mod_manager.executable->image_base, macos_test::hello_text_vmaddr);
        EXPECT_EQ(emu->mod_manager.executable->page_zero_size, macos_test::hello_text_vmaddr);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::pc), macos_test::hello_text_vmaddr + macos_test::hello_code_offset);

        emu->start(64);

        EXPECT_EQ(captured, "Hello, sogen!\n");
        EXPECT_EQ(captured_stderr, "");
        ASSERT_TRUE(emu->process.exit_status.has_value());
        EXPECT_EQ(*emu->process.exit_status, 0);
        EXPECT_EQ(emu->last_stop_reason(), sogen::stop_reason::normal_exit) << "stop detail: " << emu->last_stop_detail();
    }

    // The gate's own exit status is 0, which a broken exit handler that never records one would match by
    // accident once the optional is populated. A non-zero status separates the two.
    TEST(MacosHelloWorld, PropagatesTheGuestExitStatus)
    {
        const macho_on_disk fixture{macos_test::build_hello_world_macho(7)};
        const auto emu = macos_test::make_emulator();

        std::string captured{};
        emu->callbacks.on_stdout = [&](const std::string_view data) { captured.append(data); };

        emu->load_application(fixture.path, {"/bin/hello"}, {});
        emu->start(64);

        EXPECT_EQ(captured, "Hello, sogen!\n");
        ASSERT_TRUE(emu->process.exit_status.has_value());
        EXPECT_EQ(*emu->process.exit_status, 7);
        EXPECT_EQ(emu->last_stop_reason(), sogen::stop_reason::normal_exit);
        EXPECT_EQ(emu->last_stop_detail(), "7");
    }

    // Nothing in the hello-world image reads its own stack, so the Darwin initial stack needs an image
    // that does: this one writes argv[0] and exits with argc, making both observables depend on argc
    // sitting at sp and the argv vector starting one word above it.
    TEST(MacosHelloWorld, TheInitialStackFeedsArgcAndArgvToTheGuest)
    {
        constexpr std::string_view argv0 = "/bin/hello";

        const macho_on_disk fixture{macos_test::build_stack_echo_macho(static_cast<uint16_t>(argv0.size()))};
        const auto emu = macos_test::make_emulator();

        std::string captured{};
        emu->callbacks.on_stdout = [&](const std::string_view data) { captured.append(data); };

        emu->load_application(fixture.path, {std::string{argv0}, "world"}, {});
        emu->start(64);

        EXPECT_EQ(captured, argv0);
        ASSERT_TRUE(emu->process.exit_status.has_value());
        EXPECT_EQ(*emu->process.exit_status, 2);
        EXPECT_EQ(emu->last_stop_reason(), sogen::stop_reason::normal_exit) << "stop detail: " << emu->last_stop_detail();
    }

    TEST(MacosHelloWorld, PageZeroIsReservedWithoutBackingSoNullDerefsFault)
    {
        const macho_on_disk fixture{};
        const auto emu = macos_test::make_emulator();

        emu->load_application(fixture.path, {"/bin/hello"}, {});

        const auto& regions = emu->memory.get_mapped_regions();
        const auto page_zero = regions.find(0);
        ASSERT_NE(page_zero, regions.end());
        EXPECT_EQ(page_zero->second.length, sogen::MACOS_PAGEZERO_END);
        EXPECT_FALSE(page_zero->second.backed) << "a backed __PAGEZERO commits 4 GiB and makes a null dereference succeed";
        EXPECT_EQ(page_zero->second.permissions, sogen::memory_permission::none);

        uint32_t probe{};
        EXPECT_FALSE(emu->memory.try_read_memory(0, &probe, sizeof(probe)));
        EXPECT_FALSE(emu->memory.try_read_memory(sogen::MACOS_PAGEZERO_END - sizeof(probe), &probe, sizeof(probe)));

        ASSERT_TRUE(emu->memory.try_read_memory(sogen::MACOS_EXECUTABLE_BASE, &probe, sizeof(probe)));
        EXPECT_EQ(probe, 0xFEEDFACFU);
    }

    TEST(MacosHelloWorld, AGuestNullDereferenceFaultsInsteadOfReadingZeroes)
    {
        const macho_on_disk fixture{};
        const auto emu = macos_test::make_emulator();

        emu->load_application(fixture.path, {"/bin/hello"}, {});

        macos_test::write_guest_code(*emu, scratch_base,
                                     {
                                         0xD2800003, // mov x3, #0
                                         0xF9400064, // ldr x4, [x3]
                                     });

        emu->start(8);

        EXPECT_EQ(emu->last_stop_reason(), sogen::stop_reason::unhandled_memory_violation) << "detail: " << emu->last_stop_detail();
        EXPECT_NE(emu->last_stop_detail().find("unmapped read"), std::string::npos) << "detail: " << emu->last_stop_detail();
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x4), 0u);
    }

    TEST(MacosHelloWorld, ExecutesFewerThanThirtyInstructions)
    {
        const macho_on_disk fixture{};
        const auto emu = macos_test::make_emulator();

        emu->callbacks.on_stdout = [](std::string_view) {};
        emu->load_application(fixture.path, {"/bin/hello"}, {});
        emu->start(64);

        EXPECT_LT(emu->get_executed_instructions(), 30u)
            << "the fixture is nine instructions; a much larger count means the guest ran off the end";
    }

    TEST(MacosHelloWorld, RefusesAnImageThatIsNotAMachO)
    {
        const std::vector<std::byte> garbage(static_cast<size_t>(macos_test::hello_text_size), std::byte{0x41});
        const macho_on_disk fixture{garbage};
        const auto emu = macos_test::make_emulator();

        EXPECT_THROW(emu->load_application(fixture.path, {"/bin/hello"}, {}), std::runtime_error);
        EXPECT_EQ(emu->mod_manager.executable, nullptr);
    }
}
