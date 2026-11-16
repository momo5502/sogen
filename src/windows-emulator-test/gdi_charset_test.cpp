#include "emulation_test_utils.hpp"
#include "code_page_test_utils.hpp"

#include <syscall_utils.hpp>

#include <atomic>

namespace sogen::syscalls
{
    uint64_t handle_NtGdiCreateCompatibleDC(const syscall_context& c, hdc dc);
    uint64_t handle_NtGdiHfontCreate(const syscall_context& c, emulator_pointer logfont, uint32_t angle);
    uint64_t handle_NtGdiSelectFont(const syscall_context& c, hdc dc, uint64_t font);
    uint32_t handle_NtGdiGetCharSet(const syscall_context& c, hdc dc);
    uint32_t handle_NtGdiGetTextCharsetInfo(const syscall_context& c, hdc dc, emulator_pointer signature, uint32_t flags);
    uint32_t handle_NtGdiGetTextMetricsW(const syscall_context& c, hdc dc, emulator_pointer metrics, uint32_t size);
    uint64_t handle_NtGdiGetDCObject(const syscall_context& c, hdc dc, int32_t object_type);
    BOOL handle_NtGdiFlush(const syscall_context& c);
    int32_t handle_NtGdiSaveDC(const syscall_context& c, hdc dc);
    BOOL handle_NtGdiRestoreDC(const syscall_context& c, hdc dc, int32_t saved_dc);
}

namespace sogen::test
{
    namespace
    {
        constexpr uint64_t charset_offset = 0x04;
        constexpr uint64_t dirty_offset = 0x98;
        constexpr uint64_t font_offset = 0x128;
        constexpr uint32_t dirty_charset = 0x10;
        constexpr int32_t dc_object_font = 0xA0000;

        struct select_font_batch
        {
            int16_t size = 16;
            int16_t command = 6;
            uint32_t padding{};
            uint64_t font{};
        };

        static_assert(sizeof(select_font_batch) == 16);
        static_assert(offsetof(select_font_batch, font) == 8);

        class GdiCharsetTest : public testing::TestWithParam<bool>
        {
          protected:
            void SetUp() override
            {
                static std::atomic_uint64_t counter{};
                const auto path = std::filesystem::temp_directory_path() /
                                  ("sogen-gdi-charset-test-" + std::to_string(getpid()) + "-" + std::to_string(counter++));
                ASSERT_TRUE(std::filesystem::create_directory(path));
                this->directory = path;
                ASSERT_TRUE(std::filesystem::create_directory(this->directory / "filesys"));
                emulator_interfaces interfaces{};
                interfaces.ui = std::make_unique<null_ui_backend>();
                interfaces.audio = std::make_unique<null_audio_backend>();
                interfaces.console = std::make_unique<null_console_backend>();
                this->emu = std::make_unique<windows_emulator>(
                    create_x86_64_emulator_from_environment(),
                    emulator_settings{.disable_logging = true, .emulation_root = this->directory, .load_registry = false},
                    emulator_callbacks{}, std::move(interfaces));

                constexpr size_t allocation_size = 0x400000;
                const auto base =
                    this->emu->memory.allocate_memory(allocation_size, memory_permission::read_write, false,
                                                      DEFAULT_ALLOCATION_ADDRESS_32BIT, memory_region_kind::private_allocation, 0xFFFFFFFF);
                ASSERT_NE(base, 0);
                auto& process = this->emu->process;
                process.base_allocator = emulator_allocator{this->emu->memory, base, allocation_size};
                process.peb64 = process.base_allocator.reserve<PEB64>();
                process.peb64.write({});
                process.is_wow64_process = GetParam();
                if (process.is_wow64_process)
                {
                    process.peb32 = process.base_allocator.reserve<PEB32>();
                    process.peb32->write({});
                }
                process.ansi_code_page = code_page{make_dbcs_code_page()};
                process.oem_code_page = process.ansi_code_page;
                process.user_handles.setup(process.is_wow64_process);

                const auto thread_handle = process.threads.store(emulator_thread{this->emu->memory});
                auto* thread = process.threads.get(thread_handle);
                ASSERT_NE(thread, nullptr);
                thread->teb64 = process.base_allocator.reserve<TEB64>();
                thread->teb64->write({});
                if (process.is_wow64_process)
                {
                    thread->teb32 = process.base_allocator.reserve<TEB32>();
                    thread->teb32->write({});
                }
                this->emu->vcpu(0).active_thread = thread;

                this->scratch = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);
                this->dc = static_cast<hdc>(syscalls::handle_NtGdiCreateCompatibleDC(this->context(), 0));
                ASSERT_NE(this->dc, 0);
            }

            void TearDown() override
            {
                this->emu.reset();
                if (!this->directory.empty())
                {
                    std::error_code error{};
                    std::filesystem::remove_all(this->directory, error);
                }
            }

            syscall_context context()
            {
                return {.win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
            }

            uint64_t dc_attr()
            {
                const auto table = this->emu->process.peb64.read().GdiSharedHandleTable;
                return this->emu->memory
                    .read_memory<GDI_HANDLE_ENTRY64>(table + ((static_cast<uint32_t>(this->dc) & 0xFFFF) * sizeof(GDI_HANDLE_ENTRY64)))
                    .Object;
            }

            uint32_t make_font(const BYTE charset)
            {
                EMU_LOGFONTW font{};
                font.lfHeight = -16;
                font.lfWeight = FW_NORMAL;
                font.lfCharSet = charset;
                std::ranges::copy(u"Segoe UI", font.lfFaceName);
                this->emu->memory.write_memory(this->scratch, font);
                return static_cast<uint32_t>(syscalls::handle_NtGdiHfontCreate(this->context(), this->scratch, 0));
            }

            uint64_t current_font()
            {
                return syscalls::handle_NtGdiGetDCObject(this->context(), this->dc, dc_object_font);
            }

            uint32_t packed_charset()
            {
                return syscalls::handle_NtGdiGetCharSet(this->context(), this->dc);
            }

            void expect_charset(const uint32_t expected)
            {
                EXPECT_EQ(this->packed_charset(), expected);
                EXPECT_EQ(this->emu->memory.read_memory<uint32_t>(this->dc_attr() + charset_offset), expected);
                EXPECT_EQ(this->emu->memory.read_memory<uint32_t>(this->dc_attr() + dirty_offset) & dirty_charset, 0u);

                const auto signature = this->scratch + 0x100;
                this->emu->memory.write_memory(signature, std::array<uint32_t, 6>{1, 2, 3, 4, 5, 6});
                EXPECT_EQ(syscalls::handle_NtGdiGetTextCharsetInfo(this->context(), this->dc, signature, 0), expected >> 16);
                EXPECT_EQ((this->emu->memory.read_memory<std::array<uint32_t, 6>>(signature)), (std::array<uint32_t, 6>{}));
                const auto metrics = this->scratch + 0x200;
                EXPECT_NE(syscalls::handle_NtGdiGetTextMetricsW(this->context(), this->dc, metrics, 0x3C), 0u);
                EXPECT_EQ(this->emu->memory.read_memory<BYTE>(metrics + 0x38), expected >> 16);
            }

            std::unique_ptr<windows_emulator> emu{};
            std::filesystem::path directory{};
            hdc dc{};
            uint64_t scratch{};
        };
    }

    // Windows 11 26100 原生临时 DC 的结果；同时核验了 native/WoW64 gdi32 的共享属性和选字批记录布局。
    TEST_P(GdiCharsetTest, ReportsSelectedCharsetAndUpdatesClientCodePageCache)
    {
        const std::array rows{
            std::pair{BYTE{DEFAULT_CHARSET}, 0x008603A8u},
            std::pair{BYTE{ANSI_CHARSET}, 0x000003A8u},
            std::pair{BYTE{GB2312_CHARSET}, 0x008603A8u},
            std::pair{BYTE{RUSSIAN_CHARSET}, 0x00CC04E3u},
            std::pair{BYTE{OEM_CHARSET}, 0x00FF03A8u},
            std::pair{BYTE{SYMBOL_CHARSET}, 0x0002002Au},
            std::pair{BYTE{99}, 0x000003A8u},
        };
        for (const auto& [charset, expected] : rows)
        {
            SCOPED_TRACE(charset);
            const auto font = this->make_font(charset);
            ASSERT_NE(font, 0u);
            ASSERT_NE(syscalls::handle_NtGdiSelectFont(this->context(), this->dc, font), 0u);
            EXPECT_NE(this->emu->memory.read_memory<uint32_t>(this->dc_attr() + dirty_offset) & dirty_charset, 0u);
            this->expect_charset(expected);
        }
    }

    TEST_P(GdiCharsetTest, DoesNotForceOtherCodePagesOrExplicitCharsetsToChinese)
    {
        this->emu->process.ansi_code_page = code_page{};
        this->emu->process.oem_code_page.reset();
        const auto default_font = this->make_font(DEFAULT_CHARSET);
        ASSERT_NE(syscalls::handle_NtGdiSelectFont(this->context(), this->dc, default_font), 0u);
        this->expect_charset(0x000004E4);

        const auto oem_font = this->make_font(OEM_CHARSET);
        ASSERT_NE(syscalls::handle_NtGdiSelectFont(this->context(), this->dc, oem_font), 0u);
        this->expect_charset(0x00FF01B5);

        const auto japanese_font = this->make_font(SHIFTJIS_CHARSET);
        ASSERT_NE(syscalls::handle_NtGdiSelectFont(this->context(), this->dc, japanese_font), 0u);
        this->expect_charset(0x008003A4);
    }

    TEST_P(GdiCharsetTest, ReturnsKernelPreviousFontAfterClientPreselectsNewFont)
    {
        const auto original = this->current_font();
        const auto first = this->make_font(ANSI_CHARSET);
        const auto second = this->make_font(GB2312_CHARSET);
        ASSERT_EQ(syscalls::handle_NtGdiSelectFont(this->context(), this->dc, first), original);

        this->emu->memory.write_memory(this->dc_attr() + font_offset, uint64_t{second});
        this->emu->memory.write_memory(this->dc_attr() + dirty_offset, dirty_charset);
        EXPECT_EQ(syscalls::handle_NtGdiSelectFont(this->context(), this->dc, second), first);
        EXPECT_EQ(this->current_font(), second);
        EXPECT_EQ(this->emu->memory.read_memory<uint64_t>(this->dc_attr() + font_offset), second);
        this->expect_charset(0x008603A8);
    }

    TEST_P(GdiCharsetTest, FlushesFontSelectionRecordsBeforeTheNextKernelSelection)
    {
        const auto first = this->make_font(ANSI_CHARSET);
        const auto second = this->make_font(RUSSIAN_CHARSET);
        this->emu->memory.write_memory(this->dc_attr() + font_offset, uint64_t{second});
        auto& thread = *this->emu->vcpu(0).active_thread;
        thread.teb64->access([&](TEB64& teb) {
            const std::array records{select_font_batch{.font = first}, select_font_batch{.font = second}};
            std::memcpy(teb.GdiTebBatch.Buffer, records.data(), sizeof(records));
            teb.GdiTebBatch.Offset = sizeof(records);
            teb.GdiTebBatch.HDC = this->dc;
            teb.GdiBatchCount = static_cast<ULONG>(records.size());
        });
        ASSERT_TRUE(syscalls::handle_NtGdiFlush(this->context()));
        EXPECT_EQ(this->current_font(), second);
        this->expect_charset(0x00CC04E3);
        EXPECT_EQ(syscalls::handle_NtGdiSelectFont(this->context(), this->dc, first), second);
    }

    TEST_P(GdiCharsetTest, SavedFontSurvivesRuntimeStateRoundTripAndRestoreDc)
    {
        const auto first = this->make_font(RUSSIAN_CHARSET);
        const auto second = this->make_font(GB2312_CHARSET);
        ASSERT_NE(syscalls::handle_NtGdiSelectFont(this->context(), this->dc, first), 0u);
        ASSERT_EQ(syscalls::handle_NtGdiSaveDC(this->context(), this->dc), 1);
        ASSERT_EQ(syscalls::handle_NtGdiSelectFont(this->context(), this->dc, second), first);

        utils::buffer_serializer state{};
        state.write_map(this->emu->process.gdi_dc_states);
        state.write_map(this->emu->process.gdi_dc_save_states);
        this->emu->process.gdi_dc_states.clear();
        this->emu->process.gdi_dc_save_states.clear();
        utils::buffer_deserializer input{state};
        input.read_map(this->emu->process.gdi_dc_states);
        input.read_map(this->emu->process.gdi_dc_save_states);

        ASSERT_TRUE(syscalls::handle_NtGdiRestoreDC(this->context(), this->dc, -1));
        EXPECT_EQ(this->current_font(), first);
        this->expect_charset(0x00CC04E3);
        EXPECT_EQ(syscalls::handle_NtGdiSelectFont(this->context(), this->dc, second), first);
    }

    TEST_P(GdiCharsetTest, InvalidObjectsReturnNativeDefaultsWithoutChangingSelection)
    {
        const auto font = this->make_font(GB2312_CHARSET);
        ASSERT_NE(syscalls::handle_NtGdiSelectFont(this->context(), this->dc, font), 0u);
        for (const auto invalid : {0u, 0x12345678u, font ^ 0x01000000u})
        {
            EXPECT_EQ(syscalls::handle_NtGdiSelectFont(this->context(), this->dc, invalid), 0u);
            EXPECT_EQ(this->current_font(), font);
        }
        EXPECT_EQ(syscalls::handle_NtGdiSelectFont(this->context(), 0, font), 0u);
        EXPECT_EQ(syscalls::handle_NtGdiGetCharSet(this->context(), 0), 0x00010000u);
        const auto signature = this->scratch + 0x100;
        this->emu->memory.write_memory(signature, std::array<uint32_t, 6>{1, 2, 3, 4, 5, 6});
        EXPECT_EQ(syscalls::handle_NtGdiGetTextCharsetInfo(this->context(), 0, signature, 0), DEFAULT_CHARSET);
        EXPECT_EQ((this->emu->memory.read_memory<std::array<uint32_t, 6>>(signature)), (std::array<uint32_t, 6>{}));
        EXPECT_EQ(this->emu->vcpu(0).active_thread->teb64->read().LastErrorValue, 6u);
        if (this->emu->process.is_wow64_process)
        {
            EXPECT_EQ(this->emu->vcpu(0).active_thread->teb32->read().LastErrorValue, 6u);
        }
        this->expect_charset(0x008603A8);
    }

    INSTANTIATE_TEST_SUITE_P(NativeAndWow64, GdiCharsetTest, testing::Bool());
}
