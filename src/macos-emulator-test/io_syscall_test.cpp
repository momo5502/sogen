#include <gtest/gtest.h>

#include "macos_test_utils.hpp"

#include <array>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    constexpr uint64_t code_base = 0x100000000ULL;
    constexpr uint64_t data_base = 0x300000000ULL;
    constexpr uint64_t carry = 0x20000000ULL;

    // The backend caches translated blocks, so a second syscall in one emulator has to be assembled at a
    // fresh address; rewriting code_base would replay the first block instead.
    void run_syscall_at(sogen::macos_emulator& emu, const uint64_t address, const uint32_t mov_x16)
    {
        const std::array<uint32_t, 2> words{mov_x16, 0xD4001001};
        emu.memory.write_memory(address, words.data(), sizeof(words));
        emu.emu().reg(sogen::arm64_register::pc, address);
        emu.start(2);
    }

    int allocate_memory_fd(sogen::macos_emulator& emu, std::string content)
    {
        sogen::guest_fd entry{};
        entry.type = sogen::fd_type::memory_file;
        entry.memory_file = std::make_shared<sogen::guest_memory_fd>();
        entry.memory_file->content = std::move(content);
        return emu.process.fds.allocate(std::move(entry));
    }

    TEST(MacosIoSyscalls, WriteToStdoutIsRoutedToTheCallback)
    {
        const auto emu = macos_test::make_emulator();

        std::string captured{};
        emu->callbacks.on_stdout = [&](const std::string_view data) { captured.append(data); };

        ASSERT_TRUE(emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        constexpr std::string_view message = "Hello, sogen!\n";
        emu->memory.write_memory(data_base, message.data(), message.size());

        macos_test::write_guest_code(*emu, code_base,
                                     {
                                         0xD2800020, // mov x0, #1
                                         0xD28001C2, // mov x2, #14
                                         0xD2800090, // mov x16, #4
                                         0xD4001001, // svc #0x80
                                     });
        emu->emu().reg(sogen::arm64_register::x1, data_base);

        emu->start(4);

        EXPECT_EQ(captured, "Hello, sogen!\n");
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 14u);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, 0u);
    }

    TEST(MacosIoSyscalls, WriteToStderrUsesTheOtherCallback)
    {
        const auto emu = macos_test::make_emulator();

        std::string out{};
        std::string err{};
        emu->callbacks.on_stdout = [&](const std::string_view data) { out.append(data); };
        emu->callbacks.on_stderr = [&](const std::string_view data) { err.append(data); };

        ASSERT_TRUE(emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        constexpr std::string_view message = "bad\n";
        emu->memory.write_memory(data_base, message.data(), message.size());

        macos_test::write_guest_code(*emu, code_base,
                                     {
                                         0xD2800040, // mov x0, #2
                                         0xD2800082, // mov x2, #4
                                         0xD2800090, // mov x16, #4
                                         0xD4001001, // svc #0x80
                                     });
        emu->emu().reg(sogen::arm64_register::x1, data_base);

        emu->start(4);

        EXPECT_EQ(err, "bad\n");
        EXPECT_TRUE(out.empty());
    }

    TEST(MacosIoSyscalls, WriteToABadDescriptorSetsCarryAndEbadf)
    {
        const auto emu = macos_test::make_emulator();

        ASSERT_TRUE(emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        macos_test::write_guest_code(*emu, code_base,
                                     {
                                         0xD2800090, // mov x16, #4
                                         0xD4001001, // svc #0x80
                                     });
        emu->emu().reg(sogen::arm64_register::x0, uint64_t{999});
        emu->emu().reg(sogen::arm64_register::x1, data_base);
        emu->emu().reg(sogen::arm64_register::x2, uint64_t{4});

        emu->start(2);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 9u) << "EBADF";
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, carry);
    }

    TEST(MacosIoSyscalls, WriteToADuplicateOfStdoutStillReachesTheCallback)
    {
        const auto emu = macos_test::make_emulator();

        std::string captured{};
        emu->callbacks.on_stdout = [&](const std::string_view data) { captured.append(data); };

        ASSERT_TRUE(emu->memory.allocate_memory(code_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::all));
        ASSERT_TRUE(emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        constexpr std::string_view message = "through a dup\n";
        emu->memory.write_memory(data_base, message.data(), message.size());

        emu->emu().reg(sogen::arm64_register::x0, uint64_t{1});
        run_syscall_at(*emu, code_base, 0xD2800530); // mov x16, #41 (dup)
        ASSERT_EQ(emu->emu().reg(sogen::arm64_register::x0), 3u);

        emu->emu().reg(sogen::arm64_register::x0, uint64_t{3});
        emu->emu().reg(sogen::arm64_register::x1, data_base);
        emu->emu().reg(sogen::arm64_register::x2, uint64_t{message.size()});
        run_syscall_at(*emu, code_base + 0x40, 0xD2800090); // mov x16, #4 (write)

        EXPECT_EQ(captured, "through a dup\n") << "a duplicate of stdout carries host_path and the host handle, so "
                                                  "routing on the descriptor number writes past on_stdout";
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), message.size());
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, 0u);

        emu->process.fds.close(3);
    }

    TEST(MacosIoSyscalls, WriteToStdinIsEbadf)
    {
        const auto emu = macos_test::make_emulator();

        ASSERT_TRUE(emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        emu->memory.set_memory(data_base, 'x', 4);

        macos_test::write_guest_code(*emu, code_base, {0xD2800090, 0xD4001001}); // mov x16, #4 (write)
        emu->emu().reg(sogen::arm64_register::x0, uint64_t{0});
        emu->emu().reg(sogen::arm64_register::x1, data_base);
        emu->emu().reg(sogen::arm64_register::x2, uint64_t{4});

        emu->start(2);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 9u) << "EBADF; writing must never reach the host stdin handle";
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, carry);
    }

    TEST(MacosIoSyscalls, WriteFromAnUnmappedBufferIsEfault)
    {
        const auto emu = macos_test::make_emulator();

        std::string captured{};
        emu->callbacks.on_stdout = [&](const std::string_view data) { captured.append(data); };

        macos_test::write_guest_code(*emu, code_base, {0xD2800090, 0xD4001001}); // mov x16, #4 (write)
        emu->emu().reg(sogen::arm64_register::x0, uint64_t{1});
        emu->emu().reg(sogen::arm64_register::x1, uint64_t{0x500000000ULL});
        emu->emu().reg(sogen::arm64_register::x2, uint64_t{4});

        emu->start(2);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 14u) << "EFAULT";
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, carry);
        EXPECT_TRUE(captured.empty());
        EXPECT_NE(emu->last_stop_reason(), sogen::stop_reason::backend_error);
    }

    TEST(MacosIoSyscalls, WriteRejectsAByteCountAboveIntMax)
    {
        const auto emu = macos_test::make_emulator();

        ASSERT_TRUE(emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        macos_test::write_guest_code(*emu, code_base, {0xD2800090, 0xD4001001}); // mov x16, #4 (write)
        emu->emu().reg(sogen::arm64_register::x0, uint64_t{1});
        emu->emu().reg(sogen::arm64_register::x1, data_base);
        emu->emu().reg(sogen::arm64_register::x2, uint64_t{0x8000000000000000ULL});

        emu->start(2);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 22u) << "EINVAL";
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, carry);
        EXPECT_NE(emu->last_stop_reason(), sogen::stop_reason::backend_error);
    }

    TEST(MacosIoSyscalls, WriteReadsItsArgumentsThroughTheIndirectSyscall)
    {
        const auto emu = macos_test::make_emulator();

        std::string captured{};
        emu->callbacks.on_stdout = [&](const std::string_view data) { captured.append(data); };

        ASSERT_TRUE(emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        constexpr std::string_view message = "indirect";
        emu->memory.write_memory(data_base, message.data(), message.size());

        macos_test::write_guest_code(*emu, code_base, {0xD4001001}); // svc #0x80
        emu->emu().reg(sogen::arm64_register::x16, uint64_t{0});
        emu->emu().reg(sogen::arm64_register::x0, uint64_t{4});
        emu->emu().reg(sogen::arm64_register::x1, uint64_t{1});
        emu->emu().reg(sogen::arm64_register::x2, data_base);
        emu->emu().reg(sogen::arm64_register::x3, uint64_t{8});

        emu->start(1);

        EXPECT_EQ(captured, "indirect");
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 8u);
    }

    TEST(MacosIoSyscalls, WritevConcatenatesTheVectors)
    {
        const auto emu = macos_test::make_emulator();

        std::string captured{};
        emu->callbacks.on_stdout = [&](const std::string_view data) { captured.append(data); };

        ASSERT_TRUE(emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        constexpr std::string_view first = "Hello, ";
        constexpr std::string_view second = "sogen!\n";
        emu->memory.write_memory(data_base + 0x100, first.data(), first.size());
        emu->memory.write_memory(data_base + 0x200, second.data(), second.size());

        const auto vectors = std::to_array<sogen::macos_iovec>({
            {.iov_base = data_base + 0x100, .iov_len = first.size()},
            {.iov_base = data_base + 0x200, .iov_len = second.size()},
        });
        emu->memory.write_memory(data_base, vectors.data(), sizeof(vectors));

        macos_test::write_guest_code(*emu, code_base, {0xD2800F30, 0xD4001001}); // mov x16, #121 (writev)
        emu->emu().reg(sogen::arm64_register::x0, uint64_t{1});
        emu->emu().reg(sogen::arm64_register::x1, data_base);
        emu->emu().reg(sogen::arm64_register::x2, uint64_t{2});

        emu->start(2);

        EXPECT_EQ(captured, "Hello, sogen!\n");
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 14u);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, 0u);
    }

    TEST(MacosIoSyscalls, WritevRejectsAnOversizedVectorCount)
    {
        const auto emu = macos_test::make_emulator();

        ASSERT_TRUE(emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        macos_test::write_guest_code(*emu, code_base, {0xD2800F30, 0xD4001001}); // mov x16, #121 (writev)
        emu->emu().reg(sogen::arm64_register::x0, uint64_t{1});
        emu->emu().reg(sogen::arm64_register::x1, data_base);
        emu->emu().reg(sogen::arm64_register::x2, uint64_t{1025});

        emu->start(2);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 22u) << "EINVAL";
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, carry);
    }

    TEST(MacosIoSyscalls, WritevRejectsANegativeVectorCount)
    {
        const auto emu = macos_test::make_emulator();

        ASSERT_TRUE(emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        macos_test::write_guest_code(*emu, code_base, {0xD2800F30, 0xD4001001}); // mov x16, #121 (writev)
        emu->emu().reg(sogen::arm64_register::x0, uint64_t{1});
        emu->emu().reg(sogen::arm64_register::x1, data_base);
        emu->emu().reg(sogen::arm64_register::x2, static_cast<uint64_t>(-1));

        emu->start(2);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 22u) << "EINVAL";
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, carry);
        EXPECT_NE(emu->last_stop_reason(), sogen::stop_reason::backend_error);
    }

    TEST(MacosIoSyscalls, ReadServesAMemoryFileAndAdvancesItsOffset)
    {
        const auto emu = macos_test::make_emulator();
        const auto fd = allocate_memory_fd(*emu, "abcdefghij");
        ASSERT_EQ(fd, 3);

        ASSERT_TRUE(emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        emu->memory.set_memory(data_base, 0, 16);

        macos_test::write_guest_code(*emu, code_base, {0xD2800070, 0xD4001001}); // mov x16, #3 (read)
        emu->emu().reg(sogen::arm64_register::x0, static_cast<uint64_t>(fd));
        emu->emu().reg(sogen::arm64_register::x1, data_base);
        emu->emu().reg(sogen::arm64_register::x2, uint64_t{4});

        emu->start(2);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 4u);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, 0u);

        std::array<char, 16> bytes{};
        emu->memory.read_memory(data_base, bytes.data(), bytes.size());
        EXPECT_EQ(std::string(bytes.data(), 4), "abcd");
        EXPECT_EQ(bytes.at(4), '\0') << "read must stop at the requested length";

        ASSERT_NE(emu->process.fds.get(fd), nullptr);
        ASSERT_NE(emu->process.fds.get(fd)->memory_file, nullptr);
        EXPECT_EQ(emu->process.fds.get(fd)->memory_file->offset, 4u);
    }

    TEST(MacosIoSyscalls, ReadFromAnUnknownDescriptorIsEbadf)
    {
        const auto emu = macos_test::make_emulator();

        ASSERT_TRUE(emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        macos_test::write_guest_code(*emu, code_base, {0xD2800070, 0xD4001001}); // mov x16, #3 (read)
        emu->emu().reg(sogen::arm64_register::x0, uint64_t{999});
        emu->emu().reg(sogen::arm64_register::x1, data_base);
        emu->emu().reg(sogen::arm64_register::x2, uint64_t{4});

        emu->start(2);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 9u) << "EBADF";
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, carry);
    }

    TEST(MacosIoSyscalls, ReadOnAStdioPathNeverReachesTheHostHandle)
    {
        const auto emu = macos_test::make_emulator();

        auto* handle = tmpfile();
        ASSERT_NE(handle, nullptr);
        ASSERT_EQ(fwrite("hostdata", 1, 8, handle), 8u);
        ASSERT_EQ(fseek(handle, 0, SEEK_SET), 0);

        sogen::guest_fd entry{};
        entry.type = sogen::fd_type::file;
        entry.host_path = "/dev/stdin";
        entry.handle = handle;
        const auto fd = emu->process.fds.allocate(std::move(entry));

        ASSERT_TRUE(emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        emu->memory.set_memory(data_base, 0, 16);

        macos_test::write_guest_code(*emu, code_base, {0xD2800070, 0xD4001001}); // mov x16, #3 (read)
        emu->emu().reg(sogen::arm64_register::x0, static_cast<uint64_t>(fd));
        emu->emu().reg(sogen::arm64_register::x1, data_base);
        emu->emu().reg(sogen::arm64_register::x2, uint64_t{8});

        emu->start(2);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 0u) << "a stdio path must short-circuit before the host handle";
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, 0u);

        std::array<char, 8> bytes{};
        emu->memory.read_memory(data_base, bytes.data(), bytes.size());
        EXPECT_EQ(std::string(bytes.data(), bytes.size()), std::string(8, '\0'));

        emu->process.fds.close(fd);
    }

    TEST(MacosIoSyscalls, ReadvScattersAcrossTheVectors)
    {
        const auto emu = macos_test::make_emulator();
        const auto fd = allocate_memory_fd(*emu, "abcdefghij");

        ASSERT_TRUE(emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        emu->memory.set_memory(data_base + 0x100, 0, 32);

        const auto vectors = std::to_array<sogen::macos_iovec>({
            {.iov_base = data_base + 0x100, .iov_len = 3},
            {.iov_base = data_base + 0x200, .iov_len = 5},
        });
        emu->memory.write_memory(data_base, vectors.data(), sizeof(vectors));
        emu->memory.set_memory(data_base + 0x200, 0, 32);

        macos_test::write_guest_code(*emu, code_base, {0xD2800F10, 0xD4001001}); // mov x16, #120 (readv)
        emu->emu().reg(sogen::arm64_register::x0, static_cast<uint64_t>(fd));
        emu->emu().reg(sogen::arm64_register::x1, data_base);
        emu->emu().reg(sogen::arm64_register::x2, uint64_t{2});

        emu->start(2);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 8u);

        std::array<char, 8> first{};
        std::array<char, 8> second{};
        emu->memory.read_memory(data_base + 0x100, first.data(), first.size());
        emu->memory.read_memory(data_base + 0x200, second.data(), second.size());
        EXPECT_EQ(std::string(first.data(), 3), "abc");
        EXPECT_EQ(first.at(3), '\0') << "the first vector must not overflow into the next";
        EXPECT_EQ(std::string(second.data(), 5), "defgh");
    }

    TEST(MacosIoSyscalls, DupAllocatesTheLowestFreeDescriptor)
    {
        const auto emu = macos_test::make_emulator();

        macos_test::write_guest_code(*emu, code_base, {0xD2800530, 0xD4001001}); // mov x16, #41 (dup)
        emu->emu().reg(sogen::arm64_register::x0, uint64_t{1});

        emu->start(2);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 3u);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, 0u);
        EXPECT_NE(emu->process.fds.get(3), nullptr);
    }

    TEST(MacosIoSyscalls, DupOfAnUnknownDescriptorIsEbadf)
    {
        const auto emu = macos_test::make_emulator();

        macos_test::write_guest_code(*emu, code_base, {0xD2800530, 0xD4001001}); // mov x16, #41 (dup)
        emu->emu().reg(sogen::arm64_register::x0, uint64_t{999});

        emu->start(2);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 9u) << "EBADF";
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, carry);
    }

    TEST(MacosIoSyscalls, Dup2PlacesTheCopyAtTheRequestedDescriptor)
    {
        const auto emu = macos_test::make_emulator();
        const auto fd = allocate_memory_fd(*emu, "payload");

        macos_test::write_guest_code(*emu, code_base, {0xD2800B50, 0xD4001001}); // mov x16, #90 (dup2)
        emu->emu().reg(sogen::arm64_register::x0, static_cast<uint64_t>(fd));
        emu->emu().reg(sogen::arm64_register::x1, uint64_t{9});

        emu->start(2);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 9u);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, 0u);
        ASSERT_NE(emu->process.fds.get(9), nullptr);
        ASSERT_NE(emu->process.fds.get(9)->memory_file, nullptr);
        EXPECT_EQ(emu->process.fds.get(9)->memory_file->content, "payload");
    }

    TEST(MacosIoSyscalls, Dup2RefusesANegativeTargetDescriptor)
    {
        const auto emu = macos_test::make_emulator();

        macos_test::write_guest_code(*emu, code_base, {0xD2800B50, 0xD4001001}); // mov x16, #90 (dup2)
        emu->emu().reg(sogen::arm64_register::x0, uint64_t{1});
        emu->emu().reg(sogen::arm64_register::x1, static_cast<uint64_t>(-1));

        emu->start(2);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 9u) << "EBADF";
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, carry);
        EXPECT_EQ(emu->process.fds.get(-1), nullptr) << "a negative descriptor must never enter the table";
    }

    TEST(MacosIoSyscalls, DescriptorDuplicationIsCappedAndFreedSlotsAreReusable)
    {
        const auto emu = macos_test::make_emulator();
        const auto fd = allocate_memory_fd(*emu, "payload");
        ASSERT_EQ(fd, 3);

        ASSERT_TRUE(emu->memory.allocate_memory(code_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::all));

        constexpr int attempts = sogen::MACOS_MAX_OPEN_DESCRIPTORS + 8;
        std::vector<uint32_t> program{};
        program.reserve(static_cast<size_t>(attempts) * 2);
        for (int i = 0; i < attempts; ++i)
        {
            program.push_back(0xD2800530); // mov x16, #41 (dup)
            program.push_back(0xD4001001); // svc #0x80
        }
        emu->memory.write_memory(code_base, program.data(), program.size() * sizeof(uint32_t));

        int successes = 0;
        for (int i = 0; i < attempts; ++i)
        {
            emu->emu().reg(sogen::arm64_register::x0, static_cast<uint64_t>(fd));
            emu->emu().reg(sogen::arm64_register::pc, code_base + static_cast<uint64_t>(i) * 8);
            emu->start(2);

            if ((emu->emu().reg(sogen::arm64_register::nzcv) & carry) != 0)
            {
                break;
            }

            ++successes;
        }

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 24u) << "EMFILE";
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, carry);
        EXPECT_EQ(successes, sogen::MACOS_MAX_OPEN_DESCRIPTORS - 4)
            << "stdin, stdout, stderr and the source descriptor are open before the first dup";
        EXPECT_EQ(emu->process.fds.get_fds().size(), static_cast<size_t>(sogen::MACOS_MAX_OPEN_DESCRIPTORS));

        emu->emu().reg(sogen::arm64_register::x0, uint64_t{100});
        run_syscall_at(*emu, code_base + 0x1000, 0xD28000D0); // mov x16, #6 (close)
        ASSERT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, 0u);

        emu->emu().reg(sogen::arm64_register::x0, static_cast<uint64_t>(fd));
        run_syscall_at(*emu, code_base + 0x1010, 0xD2800530); // mov x16, #41 (dup)
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 100u) << "the cap must be a ceiling, not a leak";
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, 0u);
    }

    TEST(MacosIoSyscalls, FcntlDupfdRefusesAMinimumAtTheDescriptorCap)
    {
        const auto emu = macos_test::make_emulator();

        macos_test::write_guest_code(*emu, code_base, {0xD2800B90, 0xD4001001}); // mov x16, #92 (fcntl)
        emu->emu().reg(sogen::arm64_register::x0, uint64_t{1});
        emu->emu().reg(sogen::arm64_register::x1, static_cast<uint64_t>(sogen::macos_fcntl::MACOS_F_DUPFD));
        emu->emu().reg(sogen::arm64_register::x2, static_cast<uint64_t>(sogen::MACOS_MAX_OPEN_DESCRIPTORS));

        emu->start(2);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 22u) << "EINVAL";
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, carry);
    }

    TEST(MacosIoSyscalls, Dup2RefusesATargetAtTheDescriptorCap)
    {
        const auto emu = macos_test::make_emulator();

        macos_test::write_guest_code(*emu, code_base, {0xD2800B50, 0xD4001001}); // mov x16, #90 (dup2)
        emu->emu().reg(sogen::arm64_register::x0, uint64_t{1});
        emu->emu().reg(sogen::arm64_register::x1, static_cast<uint64_t>(sogen::MACOS_MAX_OPEN_DESCRIPTORS));

        emu->start(2);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 9u) << "EBADF";
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, carry);
        EXPECT_EQ(emu->process.fds.get(sogen::MACOS_MAX_OPEN_DESCRIPTORS), nullptr);
    }

    TEST(MacosIoSyscalls, CloseRemovesTheDescriptorAndRefusesTheSecondAttempt)
    {
        const auto emu = macos_test::make_emulator();
        const auto fd = allocate_memory_fd(*emu, "gone");

        ASSERT_TRUE(emu->memory.allocate_memory(code_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::all));

        emu->emu().reg(sogen::arm64_register::x0, static_cast<uint64_t>(fd));
        run_syscall_at(*emu, code_base, 0xD28000D0); // mov x16, #6 (close)

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 0u);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, 0u);
        EXPECT_EQ(emu->process.fds.get(fd), nullptr);

        emu->emu().reg(sogen::arm64_register::x0, static_cast<uint64_t>(fd));
        run_syscall_at(*emu, code_base + 0x40, 0xD28000D0);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 9u) << "EBADF";
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, carry);
    }

    TEST(MacosIoSyscalls, LseekMovesTheMemoryFileOffset)
    {
        const auto emu = macos_test::make_emulator();
        const auto fd = allocate_memory_fd(*emu, "abcdefghij");

        ASSERT_TRUE(emu->memory.allocate_memory(code_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::all));

        emu->emu().reg(sogen::arm64_register::x0, static_cast<uint64_t>(fd));
        emu->emu().reg(sogen::arm64_register::x1, uint64_t{3});
        emu->emu().reg(sogen::arm64_register::x2, static_cast<uint64_t>(sogen::macos_fcntl::MACOS_SEEK_SET));
        run_syscall_at(*emu, code_base, 0xD28018F0); // mov x16, #199 (lseek)
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 3u);

        emu->emu().reg(sogen::arm64_register::x0, static_cast<uint64_t>(fd));
        emu->emu().reg(sogen::arm64_register::x1, uint64_t{2});
        emu->emu().reg(sogen::arm64_register::x2, static_cast<uint64_t>(sogen::macos_fcntl::MACOS_SEEK_CUR));
        run_syscall_at(*emu, code_base + 0x40, 0xD28018F0);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 5u) << "SEEK_CUR must start from the current offset";

        emu->emu().reg(sogen::arm64_register::x0, static_cast<uint64_t>(fd));
        emu->emu().reg(sogen::arm64_register::x1, static_cast<uint64_t>(-2));
        emu->emu().reg(sogen::arm64_register::x2, static_cast<uint64_t>(sogen::macos_fcntl::MACOS_SEEK_END));
        run_syscall_at(*emu, code_base + 0x80, 0xD28018F0);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 8u) << "SEEK_END must start from the content size";
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, 0u);

        ASSERT_NE(emu->process.fds.get(fd), nullptr);
        EXPECT_EQ(emu->process.fds.get(fd)->memory_file->offset, 8u);
    }

    TEST(MacosIoSyscalls, LseekBeforeTheStartIsEinval)
    {
        const auto emu = macos_test::make_emulator();
        const auto fd = allocate_memory_fd(*emu, "abcdefghij");

        macos_test::write_guest_code(*emu, code_base, {0xD28018F0, 0xD4001001}); // mov x16, #199 (lseek)
        emu->emu().reg(sogen::arm64_register::x0, static_cast<uint64_t>(fd));
        emu->emu().reg(sogen::arm64_register::x1, static_cast<uint64_t>(-1));
        emu->emu().reg(sogen::arm64_register::x2, static_cast<uint64_t>(sogen::macos_fcntl::MACOS_SEEK_SET));

        emu->start(2);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 22u) << "EINVAL";
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, carry);
        EXPECT_EQ(emu->process.fds.get(fd)->memory_file->offset, 0u);
    }

    TEST(MacosIoSyscalls, LseekOnStdoutIsEspipeAndLeavesTheHostStreamAlone)
    {
        const auto emu = macos_test::make_emulator();

        macos_test::write_guest_code(*emu, code_base, {0xD28018F0, 0xD4001001}); // mov x16, #199 (lseek)
        emu->emu().reg(sogen::arm64_register::x0, uint64_t{1});
        emu->emu().reg(sogen::arm64_register::x1, uint64_t{0});
        emu->emu().reg(sogen::arm64_register::x2, static_cast<uint64_t>(sogen::macos_fcntl::MACOS_SEEK_END));

        emu->start(2);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 29u) << "ESPIPE";
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, carry);
    }

    TEST(MacosIoSyscalls, LseekWithAnUnknownWhenceIsEinval)
    {
        const auto emu = macos_test::make_emulator();
        const auto fd = allocate_memory_fd(*emu, "abcdefghij");

        macos_test::write_guest_code(*emu, code_base, {0xD28018F0, 0xD4001001}); // mov x16, #199 (lseek)
        emu->emu().reg(sogen::arm64_register::x0, static_cast<uint64_t>(fd));
        emu->emu().reg(sogen::arm64_register::x1, uint64_t{0});
        emu->emu().reg(sogen::arm64_register::x2, uint64_t{77});

        emu->start(2);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 22u) << "EINVAL";
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, carry);
    }

    TEST(MacosIoSyscalls, FcntlGetflAndSetfdRoundTrip)
    {
        const auto emu = macos_test::make_emulator();

        ASSERT_TRUE(emu->memory.allocate_memory(code_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::all));
        ASSERT_NE(emu->process.fds.get(1), nullptr);
        EXPECT_FALSE(emu->process.fds.get(1)->close_on_exec);

        emu->emu().reg(sogen::arm64_register::x0, uint64_t{1});
        emu->emu().reg(sogen::arm64_register::x1, static_cast<uint64_t>(sogen::macos_fcntl::MACOS_F_SETFD));
        emu->emu().reg(sogen::arm64_register::x2, static_cast<uint64_t>(sogen::macos_fcntl::MACOS_FD_CLOEXEC));
        run_syscall_at(*emu, code_base, 0xD2800B90); // mov x16, #92 (fcntl)

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, 0u);
        ASSERT_NE(emu->process.fds.get(1), nullptr);
        EXPECT_TRUE(emu->process.fds.get(1)->close_on_exec);

        emu->emu().reg(sogen::arm64_register::x0, uint64_t{1});
        emu->emu().reg(sogen::arm64_register::x1, static_cast<uint64_t>(sogen::macos_fcntl::MACOS_F_GETFD));
        run_syscall_at(*emu, code_base + 0x40, 0xD2800B90);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 1u) << "F_GETFD must report the flag F_SETFD stored";

        emu->emu().reg(sogen::arm64_register::x0, uint64_t{1});
        emu->emu().reg(sogen::arm64_register::x1, static_cast<uint64_t>(sogen::macos_fcntl::MACOS_F_SETFL));
        emu->emu().reg(sogen::arm64_register::x2, uint64_t{0x4});
        run_syscall_at(*emu, code_base + 0x80, 0xD2800B90);

        emu->emu().reg(sogen::arm64_register::x0, uint64_t{1});
        emu->emu().reg(sogen::arm64_register::x1, static_cast<uint64_t>(sogen::macos_fcntl::MACOS_F_GETFL));
        run_syscall_at(*emu, code_base + 0xC0, 0xD2800B90);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 4u) << "F_GETFL must report the flags F_SETFL stored";
    }

    TEST(MacosIoSyscalls, FcntlDupfdHonoursTheMinimumDescriptor)
    {
        const auto emu = macos_test::make_emulator();

        macos_test::write_guest_code(*emu, code_base, {0xD2800B90, 0xD4001001}); // mov x16, #92 (fcntl)
        emu->emu().reg(sogen::arm64_register::x0, uint64_t{1});
        emu->emu().reg(sogen::arm64_register::x1, static_cast<uint64_t>(sogen::macos_fcntl::MACOS_F_DUPFD_CLOEXEC));
        emu->emu().reg(sogen::arm64_register::x2, uint64_t{7});

        emu->start(2);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 7u);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, 0u);
        ASSERT_NE(emu->process.fds.get(7), nullptr);
        EXPECT_TRUE(emu->process.fds.get(7)->close_on_exec);
    }

    TEST(MacosIoSyscalls, FcntlOnAnUnknownCommandIsEinval)
    {
        const auto emu = macos_test::make_emulator();

        macos_test::write_guest_code(*emu, code_base, {0xD2800B90, 0xD4001001}); // mov x16, #92 (fcntl)
        emu->emu().reg(sogen::arm64_register::x0, uint64_t{1});
        emu->emu().reg(sogen::arm64_register::x1, uint64_t{4242});

        emu->start(2);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 22u) << "EINVAL";
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, carry);
    }

    TEST(MacosIoSyscalls, FcntlOnAnUnknownDescriptorIsEbadf)
    {
        const auto emu = macos_test::make_emulator();

        macos_test::write_guest_code(*emu, code_base, {0xD2800B90, 0xD4001001}); // mov x16, #92 (fcntl)
        emu->emu().reg(sogen::arm64_register::x0, uint64_t{999});
        emu->emu().reg(sogen::arm64_register::x1, static_cast<uint64_t>(sogen::macos_fcntl::MACOS_F_GETFD));

        emu->start(2);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 9u) << "EBADF";
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, carry);
    }

    TEST(MacosIoSyscalls, IoctlOnAnUnknownRequestFailsWithEnotty)
    {
        const auto emu = macos_test::make_emulator();

        macos_test::write_guest_code(*emu, code_base, {0xD28006D0, 0xD4001001}); // mov x16, #54 (ioctl)
        emu->emu().reg(sogen::arm64_register::x0, uint64_t{1});
        emu->emu().reg(sogen::arm64_register::x1, uint64_t{0x12345678});

        emu->start(2);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 25u) << "ENOTTY";
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, carry);
    }
}
