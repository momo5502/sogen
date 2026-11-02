#include <gtest/gtest.h>

#include <backend_selection.hpp>
#include <dyld_cache_format.hpp>
#include <macho.hpp>
#include <macos_emulator.hpp>
#include <macos_memory_protection.hpp>
#include <utils/io.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace sogen::macos_test
{
    namespace
    {
        constexpr uint64_t IMAGE_BASE = 0x100000000;
        constexpr uint64_t IMAGE_SIZE = 0x1000;
        constexpr uint32_t CPU_TYPE_ARM64 = 0x0100000C;

        constexpr auto HELLO_CODE = std::to_array<uint8_t>({
            0xB8, 0x04, 0x00, 0x00, 0x02,             // mov eax, 0x2000004 (write)
            0xBF, 0x01, 0x00, 0x00, 0x00,             // mov edi, 1
            0x48, 0x8D, 0x35, 0x13, 0x00, 0x00, 0x00, // lea rsi, [rip + message]
            0xBA, 0x06, 0x00, 0x00, 0x00,             // mov edx, 6
            0x0F, 0x05,                               // syscall
            0xB8, 0x01, 0x00, 0x00, 0x02,             // mov eax, 0x2000001 (exit)
            0xBF, 0x07, 0x00, 0x00, 0x00,             // mov edi, 7
            0x0F, 0x05,                               // syscall
            'h',  'e',  'l',  'l',  'o',  '\n',       // message
        });

        constexpr auto BAD_FD_WRITE_CODE = std::to_array<uint8_t>({
            0xB8, 0x04, 0x00, 0x00, 0x02, // mov eax, 0x2000004 (write)
            0xBF, 0x05, 0x00, 0x00, 0x00, // mov edi, 5
            0x31, 0xF6,                   // xor esi, esi
            0x31, 0xD2,                   // xor edx, edx
            0x0F, 0x05,                   // syscall
            0x89, 0xC7,                   // mov edi, eax
            0x11, 0xFF,                   // adc edi, edi
            0xB8, 0x01, 0x00, 0x00, 0x02, // mov eax, 0x2000001 (exit)
            0x0F, 0x05,                   // syscall
        });

        constexpr auto DUPLICATED_STDERR_WRITE_CODE = std::to_array<uint8_t>({
            0xB8, 0x29, 0x00, 0x00, 0x02,             // mov eax, 0x2000029 (dup)
            0xBF, 0x02, 0x00, 0x00, 0x00,             // mov edi, 2
            0x0F, 0x05,                               // syscall
            0x89, 0xC7,                               // mov edi, eax
            0xB8, 0x04, 0x00, 0x00, 0x02,             // mov eax, 0x2000004 (write)
            0x48, 0x8D, 0x35, 0x10, 0x00, 0x00, 0x00, // lea rsi, [rip + message]
            0xBA, 0x06, 0x00, 0x00, 0x00,             // mov edx, 6
            0x0F, 0x05,                               // syscall
            0xB8, 0x01, 0x00, 0x00, 0x02,             // mov eax, 0x2000001 (exit)
            0x31, 0xFF,                               // xor edi, edi
            0x0F, 0x05,                               // syscall
            'h',  'e',  'l',  'l',  'o',  '\n',       // message
        });

        constexpr auto READ_AFTER_FAULTING_READ_CODE = std::to_array<uint8_t>({
            0x48, 0x8D, 0x3D, 0x44, 0x00, 0x00, 0x00, // lea rdi, [rip + path]
            0x31, 0xF6,                               // xor esi, esi (O_RDONLY)
            0xB8, 0x05, 0x00, 0x00, 0x02,             // mov eax, 0x2000005 (open)
            0x0F, 0x05,                               // syscall
            0x89, 0xC3,                               // mov ebx, eax
            0x89, 0xDF,                               // mov edi, ebx
            0xBE, 0x00, 0x10, 0x00, 0x00,             // mov esi, 0x1000 (unmapped)
            0xBA, 0x01, 0x00, 0x00, 0x00,             // mov edx, 1
            0xB8, 0x03, 0x00, 0x00, 0x02,             // mov eax, 0x2000003 (read)
            0x0F, 0x05,                               // syscall
            0x41, 0x89, 0xC4,                         // mov r12d, eax
            0x89, 0xDF,                               // mov edi, ebx
            0x48, 0x89, 0xE6,                         // mov rsi, rsp
            0xBA, 0x01, 0x00, 0x00, 0x00,             // mov edx, 1
            0xB8, 0x03, 0x00, 0x00, 0x02,             // mov eax, 0x2000003 (read)
            0x0F, 0x05,                               // syscall
            0x41, 0xC1, 0xE4, 0x08,                   // shl r12d, 8
            0x0F, 0xB6, 0x3C, 0x24,                   // movzx edi, byte [rsp]
            0x44, 0x01, 0xE7,                         // add edi, r12d
            0xB8, 0x01, 0x00, 0x00, 0x02,             // mov eax, 0x2000001 (exit)
            0x0F, 0x05,                               // syscall
            '/',  'd',  'a',  't',  'a',  '\0',       // path
        });

        constexpr auto OVERSIZED_WRITE_CODE = std::to_array<uint8_t>({
            0xB8, 0x04, 0x00, 0x00, 0x02,                               // mov eax, 0x2000004 (write)
            0xBF, 0x01, 0x00, 0x00, 0x00,                               // mov edi, 1
            0x48, 0x8D, 0x35, 0x00, 0x00, 0x00, 0x00,                   // lea rsi, [rip]
            0x48, 0xBA, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, // mov rdx, 0xFFFFFFFFFFFF
            0x0F, 0x05,                                                 // syscall
            0x89, 0xC7,                                                 // mov edi, eax
            0x11, 0xFF,                                                 // adc edi, edi
            0xB8, 0x01, 0x00, 0x00, 0x02,                               // mov eax, 0x2000001 (exit)
            0x0F, 0x05,                                                 // syscall
        });

        constexpr auto MACH_MSG_FROM_UNMAPPED_BUFFER_CODE = std::to_array<uint8_t>({
            0xB8, 0x2F, 0x00, 0x00, 0x01,                               // mov eax, 0x100002F (mach_msg2)
            0xBF, 0x00, 0x10, 0x00, 0x00,                               // mov edi, 0x1000 (unmapped)
            0xBE, 0x01, 0x00, 0x00, 0x00,                               // mov esi, 1 (MACH64_SEND_MSG)
            0x48, 0xBA, 0x00, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, // mov rdx, 24 << 32 (send size)
            0x0F, 0x05,                                                 // syscall
            0x31, 0xFF,                                                 // xor edi, edi
            0x3D, 0x02, 0x00, 0x00, 0x10,                               // cmp eax, 0x10000002 (MACH_SEND_INVALID_DATA)
            0x40, 0x0F, 0x94, 0xC7,                                     // sete dil
            0xB8, 0x01, 0x00, 0x00, 0x02,                               // mov eax, 0x2000001 (exit)
            0x0F, 0x05,                                                 // syscall
        });

        constexpr auto TRUNCATED_TASK_INFO_CODE = std::to_array<uint8_t>({
            0x48, 0x83, 0xEC, 0x40,                                     // sub rsp, 0x40
            0xC7, 0x04, 0x24, 0x13, 0x00, 0x00, 0x00,                   // mov dword [rsp], 0x13 (copy send)
            0xC7, 0x44, 0x24, 0x04, 0x18, 0x00, 0x00, 0x00,             // mov dword [rsp + 4], 24 (header only)
            0xC7, 0x44, 0x24, 0x08, 0x03, 0x02, 0x00, 0x00,             // mov dword [rsp + 8], 0x203 (task port)
            0xC7, 0x44, 0x24, 0x0C, 0x00, 0x00, 0x00, 0x00,             // mov dword [rsp + 12], 0
            0xC7, 0x44, 0x24, 0x10, 0x00, 0x00, 0x00, 0x00,             // mov dword [rsp + 16], 0
            0xC7, 0x44, 0x24, 0x14, 0x4D, 0x0D, 0x00, 0x00,             // mov dword [rsp + 20], 3405 (task_info)
            0x48, 0x89, 0xE7,                                           // mov rdi, rsp
            0xBE, 0x01, 0x00, 0x00, 0x00,                               // mov esi, 1 (MACH64_SEND_MSG)
            0x48, 0xBA, 0x00, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, // mov rdx, 24 << 32 (send size)
            0xB8, 0x2F, 0x00, 0x00, 0x01,                               // mov eax, 0x100002F (mach_msg2)
            0x0F, 0x05,                                                 // syscall
            0x89, 0xC7,                                                 // mov edi, eax
            0xB8, 0x01, 0x00, 0x00, 0x02,                               // mov eax, 0x2000001 (exit)
            0x0F, 0x05,                                                 // syscall
        });

        constexpr auto EXIT_WITH_ARGC_CODE = std::to_array<uint8_t>({
            0x48,
            0x8B,
            0x3C,
            0x24, // mov rdi, [rsp]
            0xB8,
            0x01,
            0x00,
            0x00,
            0x02, // mov eax, 0x2000001 (exit)
            0x0F,
            0x05, // syscall
        });

        constexpr auto INVALID_MACH_TRAP_CODE = std::to_array<uint8_t>({
            0xB8,
            0x00,
            0x00,
            0x00,
            0x01, // mov eax, 0x1000000 (Mach trap 0, kern_invalid)
            0x0F,
            0x05, // syscall
        });

        constexpr auto READ_COMMPAGE_PAGE_SHIFT_CODE = std::to_array<uint8_t>({
            0xA0, 0x4E, 0x00, 0xE0, 0xFF, 0xFF, 0x7F, 0x00, 0x00, // mov al, [0x7FFFFFE0004E] (user page shift)
            0x0F, 0xB6, 0xF8,                                     // movzx edi, al
            0xB8, 0x01, 0x00, 0x00, 0x02,                         // mov eax, 0x2000001 (exit)
            0x0F, 0x05,                                           // syscall
        });

        constexpr auto FAKE_DYLD_CODE = std::to_array<uint8_t>({
            0x48, 0x8B, 0x04, 0x24,                                     // mov rax, [rsp]
            0x48, 0xB9, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, // mov rcx, 0x100000000
            0x31, 0xFF,                                                 // xor edi, edi
            0x48, 0x39, 0xC8,                                           // cmp rax, rcx
            0x40, 0x0F, 0x95, 0xC7,                                     // setne dil
            0x6B, 0xFF, 0x64,                                           // imul edi, edi, 100
            0x48, 0x03, 0x7C, 0x24, 0x08,                               // add rdi, [rsp + 8]
            0xB8, 0x01, 0x00, 0x00, 0x02,                               // mov eax, 0x2000001 (exit)
            0x0F, 0x05,                                                 // syscall
        });

        constexpr auto CHECK_REBASED_CACHE_POINTERS_CODE = std::to_array<uint8_t>({
            0x48, 0xA1, 0x00, 0x10, 0x00, 0x00, 0xF8, 0x7F, 0x00, 0x00, // mov rax, [0x7FF800001000]
            0x48, 0xB9, 0x34, 0x12, 0x00, 0x00, 0xF8, 0x7F, 0x00, 0x00, // mov rcx, 0x7FF800001234
            0x31, 0xFF,                                                 // xor edi, edi
            0x48, 0x39, 0xC8,                                           // cmp rax, rcx
            0x40, 0x0F, 0x94, 0xC7,                                     // sete dil
            0x48, 0xA1, 0x08, 0x10, 0x00, 0x00, 0xF8, 0x7F, 0x00, 0x00, // mov rax, [0x7FF800001008]
            0x48, 0xB9, 0x10, 0x00, 0x00, 0x00, 0xF8, 0x7F, 0x00, 0x00, // mov rcx, 0x7FF800000010
            0x48, 0x39, 0xC8,                                           // cmp rax, rcx
            0x0F, 0x94, 0xC0,                                           // sete al
            0x0F, 0xB6, 0xC0,                                           // movzx eax, al
            0x8D, 0x3C, 0x47,                                           // lea edi, [rdi + 2 * rax]
            0xB8, 0x01, 0x00, 0x00, 0x02,                               // mov eax, 0x2000001 (exit)
            0x0F, 0x05,                                                 // syscall
        });

        constexpr uint64_t SHARED_REGION_BASE = 0x7FF800000000;

        template <typename T>
        void write_at(std::vector<std::byte>& data, const size_t offset, const T& value)
        {
            std::memcpy(data.data() + offset, &value, sizeof(value));
        }

        // A one-file cache: a text mapping holding the header and a data mapping whose two pointers form a
        // slide info v2 chain (delta in bits 40-55, scaled by 4).
        std::vector<std::byte> make_synthetic_shared_cache()
        {
            constexpr uint64_t page_size = 0x1000;
            constexpr uint64_t delta_mask = 0x00FFFF0000000000;
            constexpr size_t header_size = 0x228;
            constexpr size_t slide_info_offset = 2 * page_size;

            std::vector<std::byte> data(slide_info_offset + sizeof(dyld_cache::slide_info_v2) + sizeof(uint16_t));
            std::memcpy(data.data(), "dyld_v1  x86_64", 16);
            write_at<uint64_t>(data, 0xE0, SHARED_REGION_BASE);
            write_at<uint64_t>(data, 0xE8, 0x10000);
            write_at<uint32_t>(data, 0x138, header_size);
            write_at<uint32_t>(data, 0x13C, 2);
            write_at<uint64_t>(data, 0x1F0, 0x8000);
            write_at<uint64_t>(data, 0x1F8, 0x4000);

            write_at(data, header_size,
                     dyld_cache::mapping_and_slide_info{.address = SHARED_REGION_BASE,
                                                        .size = page_size,
                                                        .file_offset = 0,
                                                        .slide_info_file_offset = 0,
                                                        .slide_info_file_size = 0,
                                                        .flags = 0,
                                                        .max_prot = MACOS_PROT_READ | MACOS_PROT_EXECUTE,
                                                        .init_prot = MACOS_PROT_READ | MACOS_PROT_EXECUTE});
            write_at(data, header_size + sizeof(dyld_cache::mapping_and_slide_info),
                     dyld_cache::mapping_and_slide_info{.address = SHARED_REGION_BASE + page_size,
                                                        .size = page_size,
                                                        .file_offset = page_size,
                                                        .slide_info_file_offset = slide_info_offset,
                                                        .slide_info_file_size = sizeof(dyld_cache::slide_info_v2) + sizeof(uint16_t),
                                                        .flags = 0,
                                                        .max_prot = MACOS_PROT_READ | MACOS_PROT_WRITE,
                                                        .init_prot = MACOS_PROT_READ | MACOS_PROT_WRITE});

            write_at<uint64_t>(data, page_size, (uint64_t{2} << 40) | 0x1234);
            write_at<uint64_t>(data, page_size + 8, 0x10);

            write_at(data, slide_info_offset,
                     dyld_cache::slide_info_v2{.version = 2,
                                               .page_size = page_size,
                                               .page_starts_offset = sizeof(dyld_cache::slide_info_v2),
                                               .page_starts_count = 1,
                                               .page_extras_offset = sizeof(dyld_cache::slide_info_v2) + sizeof(uint16_t),
                                               .page_extras_count = 0,
                                               .delta_mask = delta_mask,
                                               .value_add = SHARED_REGION_BASE});
            write_at<uint16_t>(data, slide_info_offset + sizeof(dyld_cache::slide_info_v2), 0);

            return data;
        }

        struct test_image
        {
            macho::mach_header_64 header{};
            macho::segment_command_64 text{};
            macho::thread_command thread{};
            macho::x86_thread_state64 thread_state{};
            std::vector<std::vector<std::byte>> extra_commands{};
            std::vector<uint8_t> code{};

            std::vector<std::byte> serialize() const
            {
                std::vector<std::byte> data{};
                const auto append = [&data](const void* value, const size_t size) {
                    std::ranges::copy(std::span{static_cast<const std::byte*>(value), size}, std::back_inserter(data));
                };

                auto final_header = this->header;
                final_header.ncmds = static_cast<uint32_t>(2 + this->extra_commands.size());
                final_header.sizeofcmds = static_cast<uint32_t>(sizeof(this->text) + sizeof(this->thread) + sizeof(this->thread_state));
                for (const auto& command : this->extra_commands)
                {
                    final_header.sizeofcmds += static_cast<uint32_t>(command.size());
                }

                append(&final_header, sizeof(final_header));
                append(&this->text, sizeof(this->text));
                append(&this->thread, sizeof(this->thread));
                append(&this->thread_state, sizeof(this->thread_state));
                for (const auto& command : this->extra_commands)
                {
                    append(command.data(), command.size());
                }
                append(this->code.data(), this->code.size());

                return data;
            }
        };

        struct image_options
        {
            uint64_t base{IMAGE_BASE};
            uint32_t file_type{macho::MH_EXECUTE};
            std::vector<std::vector<std::byte>> extra_commands{};
        };

        test_image make_test_image(const std::span<const uint8_t> code, image_options options = {})
        {
            test_image image{};
            image.code.assign(code.begin(), code.end());
            image.extra_commands = std::move(options.extra_commands);

            auto code_offset = sizeof(image.header) + sizeof(image.text) + sizeof(image.thread) + sizeof(image.thread_state);
            for (const auto& command : image.extra_commands)
            {
                code_offset += command.size();
            }

            image.header.magic = macho::MH_MAGIC_64;
            image.header.cputype = macho::CPU_TYPE_X86_64;
            image.header.filetype = options.file_type;

            image.text.cmd = macho::LC_SEGMENT_64;
            image.text.cmdsize = sizeof(image.text);
            std::memcpy(image.text.segname.data(), "__TEXT", sizeof("__TEXT"));
            image.text.vmaddr = options.base;
            image.text.vmsize = IMAGE_SIZE;
            image.text.filesize = code_offset + image.code.size();
            image.text.maxprot = MACOS_PROT_READ | MACOS_PROT_EXECUTE;
            image.text.initprot = MACOS_PROT_READ | MACOS_PROT_EXECUTE;

            image.thread.cmd = macho::LC_UNIXTHREAD;
            image.thread.cmdsize = sizeof(image.thread) + sizeof(image.thread_state);
            image.thread.flavor = macho::X86_THREAD_STATE64;
            image.thread.count = sizeof(image.thread_state) / sizeof(uint32_t);
            image.thread_state.rip = options.base + code_offset;

            return image;
        }

        std::vector<std::byte> make_dylinker_command(const std::string_view path)
        {
            macho::dylinker_command command{};
            command.cmd = macho::LC_LOAD_DYLINKER;
            command.name_offset = sizeof(command);
            command.cmdsize = static_cast<uint32_t>((sizeof(command) + path.size() + 1 + 7) & ~size_t{7});

            std::vector<std::byte> data(command.cmdsize);
            std::memcpy(data.data(), &command, sizeof(command));
            std::memcpy(data.data() + sizeof(command), path.data(), path.size());
            return data;
        }

        class temporary_root
        {
          public:
            temporary_root()
                : path_(std::filesystem::temp_directory_path() /
                        ("sogen-macos-root-" + std::string{::testing::UnitTest::GetInstance()->current_test_info()->name()}))
            {
                std::filesystem::remove_all(this->path_);
                std::filesystem::create_directories(this->path_);
            }

            temporary_root(const temporary_root&) = delete;
            temporary_root& operator=(const temporary_root&) = delete;
            temporary_root(temporary_root&&) = delete;
            temporary_root& operator=(temporary_root&&) = delete;

            ~temporary_root()
            {
                std::error_code ec{};
                std::filesystem::remove_all(this->path_, ec);
            }

            void add_file(const std::string_view guest_path, const std::span<const std::byte> data) const
            {
                const auto host_path = this->path_ / guest_path.substr(1);
                std::filesystem::create_directories(host_path.parent_path());
                ASSERT_TRUE(utils::io::write_file(host_path, data));
            }

            const std::filesystem::path& path() const
            {
                return this->path_;
            }

          private:
            std::filesystem::path path_{};
        };

        struct run_result
        {
            std::string stdout_data{};
            std::string stderr_data{};
            std::optional<int> exit_status{};
        };

        struct universal_slice
        {
            uint32_t cputype{};
            std::vector<std::byte> data{};
        };

        std::vector<std::byte> make_universal_binary(const std::vector<universal_slice>& slices)
        {
            constexpr uint64_t slice_alignment = 0x1000;

            std::vector<std::byte> data{};
            const auto append_big_endian_u32 = [&data](const uint64_t value) {
                for (int shift = 24; shift >= 0; shift -= 8)
                {
                    data.push_back(static_cast<std::byte>((value >> shift) & 0xFF));
                }
            };

            append_big_endian_u32(macho::FAT_MAGIC);
            append_big_endian_u32(slices.size());

            uint64_t slice_offset = slice_alignment;
            for (const auto& slice : slices)
            {
                append_big_endian_u32(slice.cputype);
                append_big_endian_u32(0);
                append_big_endian_u32(slice_offset);
                append_big_endian_u32(slice.data.size());
                append_big_endian_u32(12);
                slice_offset += (slice.data.size() + slice_alignment - 1) & ~(slice_alignment - 1);
            }

            for (const auto& slice : slices)
            {
                data.resize((data.size() + slice_alignment - 1) & ~(slice_alignment - 1));
                data.insert(data.end(), slice.data.begin(), slice.data.end());
            }

            return data;
        }

        run_result run_binary(const std::span<const std::byte> data, const std::vector<std::string>& argv = {"test"},
                              const std::filesystem::path& emulation_root = {})
        {
            macos_emulator emu{create_x86_64_emulator_from_environment(), emulation_root, data, "/test", argv, {}};
            emu.log.disable_output(true);

            run_result result{};
            emu.on_stdout = [&result](const std::string_view output) { result.stdout_data += output; };
            emu.on_stderr = [&result](const std::string_view output) { result.stderr_data += output; };

            emu.start();

            result.exit_status = emu.exit_status;
            return result;
        }

        run_result run_image(const test_image& image, const std::vector<std::string>& argv = {"test"},
                             const std::filesystem::path& emulation_root = {})
        {
            return run_binary(image.serialize(), argv, emulation_root);
        }

        void expect_rejected(const std::span<const std::byte> data, const std::filesystem::path& emulation_root = {})
        {
            auto backend = create_x86_64_emulator_from_environment();

            EXPECT_THROW(macos_emulator(std::move(backend), emulation_root, data, "/test", {"test"}, {}), std::runtime_error);
        }

        void expect_rejected(const test_image& image, const std::filesystem::path& emulation_root = {})
        {
            expect_rejected(image.serialize(), emulation_root);
        }
    }

    TEST(MacosEmulationTest, RunsStaticExecutable)
    {
        const auto result = run_image(make_test_image(HELLO_CODE));

        EXPECT_EQ(result.stdout_data, "hello\n");
        EXPECT_TRUE(result.stderr_data.empty());
        EXPECT_EQ(result.exit_status, 7);
    }

    TEST(MacosEmulationTest, ReportsSyscallErrorsThroughCarryFlag)
    {
        const auto result = run_image(make_test_image(BAD_FD_WRITE_CODE));

        constexpr int ebadf = 9;
        EXPECT_EQ(result.exit_status, (ebadf * 2) + 1);
    }

    TEST(MacosEmulationTest, WritesThroughDuplicatedStandardStream)
    {
        const auto result = run_image(make_test_image(DUPLICATED_STDERR_WRITE_CODE));

        EXPECT_EQ(result.stderr_data, "hello\n");
        EXPECT_TRUE(result.stdout_data.empty());
        EXPECT_EQ(result.exit_status, 0);
    }

    TEST(MacosEmulationTest, FailsReadIntoUnwritableRangeWithoutConsumingInput)
    {
        const temporary_root root{};
        root.add_file("/data", std::as_bytes(std::span{std::string_view{"AB"}}));

        const auto result = run_image(make_test_image(READ_AFTER_FAULTING_READ_CODE), {"test"}, root.path());

        constexpr int efault = 14;
        EXPECT_EQ(result.exit_status, (efault << 8) + 'A');
    }

    TEST(MacosEmulationTest, FailsWriteFromUnreadableRangeWithoutHostAllocation)
    {
        const auto result = run_image(make_test_image(OVERSIZED_WRITE_CODE));

        constexpr int efault = 14;
        EXPECT_EQ(result.exit_status, (efault * 2) + 1);
    }

    TEST(MacosEmulationTest, FailsMachMessageFromUnmappedBuffer)
    {
        const auto result = run_image(make_test_image(MACH_MSG_FROM_UNMAPPED_BUFFER_CODE));

        EXPECT_EQ(result.exit_status, 1);
    }

    TEST(MacosEmulationTest, AnswersTruncatedMigRequestWithoutStopping)
    {
        const auto result = run_image(make_test_image(TRUNCATED_TASK_INFO_CODE));

        EXPECT_EQ(result.exit_status, 0);
    }

    TEST(MacosEmulationTest, PassesArgumentCountOnInitialStack)
    {
        const auto result = run_image(make_test_image(EXIT_WITH_ARGC_CODE), {"test", "a", "b"});

        EXPECT_EQ(result.exit_status, 3);
    }

    TEST(MacosEmulationTest, StopsOnUnknownMachTrap)
    {
        const auto result = run_image(make_test_image(INVALID_MACH_TRAP_CODE));

        EXPECT_FALSE(result.exit_status.has_value());
    }

    TEST(MacosEmulationTest, MapsCommpage)
    {
        const auto result = run_image(make_test_image(READ_COMMPAGE_PAGE_SHIFT_CODE));

        EXPECT_EQ(result.exit_status, 12);
    }

    TEST(MacosEmulationTest, StartsDyldWithMainHeaderAndArguments)
    {
        const temporary_root root{};
        root.add_file("/usr/lib/dyld", make_test_image(FAKE_DYLD_CODE, {.base = 0, .file_type = macho::MH_DYLINKER}).serialize());

        const auto executable = make_test_image(HELLO_CODE, {.extra_commands = {make_dylinker_command("/usr/lib/dyld")}});
        const auto result = run_image(executable, {"test", "a"}, root.path());

        EXPECT_TRUE(result.stdout_data.empty());
        EXPECT_EQ(result.exit_status, 2);
    }

    TEST(MacosEmulationTest, RebasesSharedCachePagesOnFirstAccess)
    {
        const temporary_root root{};
        root.add_file("/usr/lib/dyld",
                      make_test_image(CHECK_REBASED_CACHE_POINTERS_CODE, {.base = 0, .file_type = macho::MH_DYLINKER}).serialize());
        root.add_file("/System/Library/dyld/dyld_shared_cache_x86_64", make_synthetic_shared_cache());

        const auto executable = make_test_image(HELLO_CODE, {.extra_commands = {make_dylinker_command("/usr/lib/dyld")}});
        const auto result = run_image(executable, {"test"}, root.path());

        EXPECT_EQ(result.exit_status, 3);
    }

    TEST(MacosLoaderTest, RejectsDynamicExecutableWithoutDyld)
    {
        const temporary_root root{};

        expect_rejected(make_test_image(HELLO_CODE, {.extra_commands = {make_dylinker_command("/usr/lib/dyld")}}), root.path());
    }

    TEST(MacosLoaderTest, RejectsNonX86_64Image)
    {
        auto image = make_test_image(HELLO_CODE);
        image.header.cputype = CPU_TYPE_ARM64;

        expect_rejected(image);
    }

    TEST(MacosLoaderTest, RejectsSegmentExceedingFile)
    {
        auto image = make_test_image(HELLO_CODE);
        image.text.filesize = IMAGE_SIZE;

        expect_rejected(image);
    }

    TEST(MacosLoaderTest, RejectsSegmentOutsideUserAddressSpace)
    {
        auto image = make_test_image(HELLO_CODE);
        image.text.vmaddr = 0x800000000000;

        expect_rejected(image);
    }

    TEST(MacosLoaderTest, RejectsLoadCommandsBeyondSizeofcmds)
    {
        const auto image = make_test_image(HELLO_CODE);
        auto data = image.serialize();

        const auto truncated_size = static_cast<uint32_t>(sizeof(image.text));
        std::memcpy(data.data() + offsetof(macho::mach_header_64, sizeofcmds), &truncated_size, sizeof(truncated_size));

        expect_rejected(data);
    }

    TEST(MacosLoaderTest, RejectsUnsupportedThreadStateFlavor)
    {
        auto image = make_test_image(HELLO_CODE);
        image.thread.flavor = 1;

        expect_rejected(image);
    }

    TEST(MacosEmulationTest, RunsX86_64SliceOfUniversalBinary)
    {
        const auto data = make_universal_binary({
            {.cputype = CPU_TYPE_ARM64, .data = std::vector<std::byte>(0x40)},
            {.cputype = macho::CPU_TYPE_X86_64, .data = make_test_image(HELLO_CODE).serialize()},
        });

        const auto result = run_binary(data);

        EXPECT_EQ(result.stdout_data, "hello\n");
        EXPECT_EQ(result.exit_status, 7);
    }

    TEST(MacosLoaderTest, RejectsUniversalBinaryWithoutX86_64Slice)
    {
        expect_rejected(make_universal_binary({{.cputype = CPU_TYPE_ARM64, .data = std::vector<std::byte>(0x40)}}));
    }
}
