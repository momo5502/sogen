#include "emulation_test_utils.hpp"
#include "code_page_test_utils.hpp"
#include "gdi_font_test_utils.hpp"

#include <gdi_font_manager.hpp>
#include <syscall_utils.hpp>

#include <atomic>
#include <fstream>

namespace sogen::syscalls
{
    NTSTATUS handle_NtGdiInit(const syscall_context& c);
    uint64_t handle_NtGdiCreateCompatibleDC(const syscall_context& c, hdc dc);
    uint64_t handle_NtGdiCreateBitmap(const syscall_context& c, uint32_t width, uint32_t height, uint32_t planes, uint32_t bits_pixel,
                                      emulator_pointer bits);
    uint64_t handle_NtGdiSelectBitmap(const syscall_context& c, hdc dc, handle bitmap);
    uint64_t handle_NtGdiHfontCreate(const syscall_context& c, emulator_pointer logfont, uint32_t angle);
    uint64_t handle_NtGdiSelectFont(const syscall_context& c, hdc dc, uint64_t font);
    uint64_t handle_NtGdiGetDCObject(const syscall_context& c, hdc dc, int32_t object_type);
    COLORREF handle_NtGdiGetPixel(const syscall_context& c, hdc dc, int x, int y);
    BOOL handle_NtGdiExtTextOutW(const syscall_context& c, hdc dc, LONG x, LONG y, UINT options, emulator_pointer rect,
                                 emulator_pointer text, UINT count, emulator_pointer dx, DWORD code_page);
    uint32_t handle_NtGdiGetTextMetricsW(const syscall_context& c, hdc dc, emulator_pointer metrics, uint32_t size);
    int32_t handle_NtGdiGetTextFaceW(const syscall_context& c, hdc dc, int32_t count, emulator_pointer face_name, BOOL alias_name);
    uint32_t handle_NtGdiGetGlyphIndicesW(const syscall_context& c, hdc dc, emulator_pointer text, int32_t char_count,
                                          emulator_pointer glyph_indices, uint32_t flags);
    BOOL handle_NtGdiGetCharWidthW(const syscall_context& c, hdc dc, UINT first_char, UINT char_count, emulator_pointer chars, UINT flags,
                                   emulator_pointer buffer);
    BOOL handle_NtGdiGetCharABCWidthsW(const syscall_context& c, hdc dc, UINT first_char, UINT char_count, emulator_pointer chars,
                                       UINT flags, emulator_pointer buffer);
    BOOL handle_NtGdiGetTextExtent(const syscall_context& c, hdc dc, emulator_pointer text, int32_t char_count, emulator_pointer size,
                                   ULONG flags);
    BOOL handle_NtGdiGetTextExtentExW(const syscall_context& c, hdc dc, emulator_pointer text, ULONG char_count, ULONG max_extent,
                                      emulator_object<ULONG> fit, emulator_pointer extents, emulator_pointer size, ULONG flags);
    BOOL handle_NtGdiFlush(const syscall_context& c);
}

namespace sogen::test
{
    namespace
    {
        constexpr uint64_t k_background_offset = 0xB4;
        constexpr uint64_t k_text_align_offset = 0x110;
        constexpr uint64_t k_current_point_offset = 0xD8;
        constexpr int k_side = 64;
        constexpr COLORREF k_white = 0x00FFFFFF;
        constexpr COLORREF k_black = 0x00000000;
        constexpr UINT k_eto_glyph_index = 0x10;

        // GDIBSTEXTOUT record layout verified against native gdi32full (64-bit slot for the font handle).
        struct batched_text_out
        {
            int16_t size{};
            int16_t command = 2;
            uint32_t foreground{};
            uint32_t background = 0x00FFFFFF;
            int32_t bk_mode = 2;
            uint32_t foreground_ul{};
            uint32_t background_ul{};
            int32_t x{};
            int32_t y{};
            uint32_t options{};
            RECT rect{};
            uint32_t code_page{};
            uint32_t count{};
            uint32_t dx_offset{};
            uint64_t font{};
            uint32_t text_align{};
            POINTL viewport_org{};
            std::array<char16_t, 16> payload{};
        };

        static_assert(offsetof(batched_text_out, payload) == 0x54);

        struct glyph_box
        {
            int64_t left{};
            int64_t top{};
            int64_t right{};  // exclusive
            int64_t bottom{}; // exclusive
        };

        class GdiTextRenderTest : public testing::TestWithParam<bool>
        {
          protected:
            void SetUp() override
            {
                static std::atomic_uint64_t counter{};
                const auto path = std::filesystem::temp_directory_path() /
                                  ("sogen-gdi-text-render-" + std::to_string(getpid()) + "-" + std::to_string(counter++));
                ASSERT_TRUE(std::filesystem::create_directory(path));
                this->directory = path;
                const auto font_directory = path / "filesys" / "c" / "windows" / "Fonts";
                ASSERT_TRUE(std::filesystem::create_directories(font_directory));
                const auto fixture = font_fixture::make_font();
                std::ofstream output(font_directory / "fixture.ttf", std::ios::binary);
                output.write(reinterpret_cast<const char*>(fixture.data()), static_cast<std::streamsize>(fixture.size()));
                ASSERT_TRUE(output.good());
                output.close();

                emulator_interfaces interfaces{};
                interfaces.ui = std::make_unique<null_ui_backend>();
                interfaces.audio = std::make_unique<null_audio_backend>();
                interfaces.console = std::make_unique<null_console_backend>();
                this->emu = std::make_unique<windows_emulator>(
                    create_x86_64_emulator_from_environment(),
                    emulator_settings{.disable_logging = true, .emulation_root = this->directory, .load_registry = false},
                    emulator_callbacks{}, std::move(interfaces));
                this->emu->version.set_system_root(windows_path{"C:\\Windows"});

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

                syscalls::handle_NtGdiInit(this->context());
                this->dc = static_cast<hdc>(syscalls::handle_NtGdiCreateCompatibleDC(this->context(), 0));
                const auto bitmap = syscalls::handle_NtGdiCreateBitmap(this->context(), k_side, k_side, 1, 32, 0);
                ASSERT_NE(this->dc, 0);
                ASSERT_NE(bitmap, 0u);
                syscalls::handle_NtGdiSelectBitmap(this->context(), this->dc, make_handle(bitmap));
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

            gdi_font_reference fixture_reference()
            {
                EMU_LOGFONTW font{};
                font.lfHeight = -16;
                font.lfWeight = FW_NORMAL;
                font.lfCharSet = DEFAULT_CHARSET;
                utils::string::copy(font.lfFaceName, u"Sogen Fixture");
                const auto resolved = this->emu->process.gdi_font_resources.resolve(this->emu->file_sys, this->emu->registry,
                                                                                    this->emu->version.get_system_root(), font);
                EXPECT_TRUE(resolved.has_value());
                return resolved.value_or(gdi_font_reference{});
            }

            uint32_t select_fixture_font()
            {
                EMU_LOGFONTW font{};
                font.lfHeight = -16;
                font.lfWeight = FW_NORMAL;
                font.lfCharSet = DEFAULT_CHARSET;
                utils::string::copy(font.lfFaceName, u"Sogen Fixture");
                this->emu->memory.write_memory(this->scratch + 0x800, font);
                const auto handle = static_cast<uint32_t>(syscalls::handle_NtGdiHfontCreate(this->context(), this->scratch + 0x800, 0));
                EXPECT_NE(handle, 0u);
                EXPECT_NE(syscalls::handle_NtGdiSelectFont(this->context(), this->dc, handle), 0u);
                return handle;
            }

            int32_t advance_of(const gdi_font_reference& font, const uint32_t index)
            {
                const auto metrics = this->emu->process.gdi_font_resources.glyph_metrics(font, index);
                return (metrics.advance_x_26_6 + 32) >> 6;
            }

            glyph_box box_of(const gdi_font_reference& font, const uint32_t index, const int64_t origin_x, const int64_t baseline)
            {
                const auto bitmap = this->emu->process.gdi_font_resources.rasterize(font, index);
                return {.left = origin_x + bitmap->left,
                        .top = baseline - bitmap->top,
                        .right = origin_x + bitmap->left + bitmap->width,
                        .bottom = baseline - bitmap->top + bitmap->height};
            }

            int32_t ascent_of(const gdi_font_reference& font)
            {
                return this->emu->process.gdi_font_resources.metrics(font).ascent;
            }

            void draw_text(const int32_t x, const int32_t y, const std::u16string_view text, const UINT options = 0)
            {
                this->emu->memory.write_memory(this->scratch, text.data(), text.size() * sizeof(char16_t));
                ASSERT_EQ(TRUE, syscalls::handle_NtGdiExtTextOutW(this->context(), this->dc, x, y, options, 0, this->scratch,
                                                                  static_cast<UINT>(text.size()), 0, 0));
            }

            COLORREF pixel(const int64_t x, const int64_t y)
            {
                return syscalls::handle_NtGdiGetPixel(this->context(), this->dc, static_cast<int>(x), static_cast<int>(y));
            }

            void fill_batch(TEB64& teb, const batched_text_out& record, const size_t used_bytes)
            {
                auto* buffer = reinterpret_cast<uint8_t*>(teb.GdiTebBatch.Buffer);
                std::memcpy(buffer, &record, used_bytes);
                teb.GdiTebBatch.Offset = static_cast<ULONG>(used_bytes);
                teb.GdiTebBatch.HDC = static_cast<uint64_t>(this->dc);
                teb.GdiBatchCount = 1;
            }

            void fill_batch(TEB32& teb, const batched_text_out& record, const size_t used_bytes)
            {
                auto* buffer = reinterpret_cast<uint8_t*>(teb.GdiTebBatch.Buffer);
                std::memcpy(buffer, &record, used_bytes);
                teb.GdiTebBatch.Offset = static_cast<ULONG>(used_bytes);
                teb.GdiTebBatch.HDC = static_cast<uint32_t>(this->dc);
                teb.GdiBatchCount = 1;
            }

            void queue_text_out(const uint32_t font, const int32_t x, const int32_t y, const std::u16string_view text,
                                const bool wow64_batch)
            {
                batched_text_out record{};
                const auto used_bytes = offsetof(batched_text_out, payload) + (text.size() * sizeof(char16_t));
                record.size = static_cast<int16_t>(used_bytes);
                record.x = x;
                record.y = y;
                record.count = static_cast<uint32_t>(text.size());
                record.font = font;
                std::memcpy(record.payload.data(), text.data(), text.size() * sizeof(char16_t));

                auto& thread = *this->emu->vcpu(0).active_thread;
                if (wow64_batch)
                {
                    thread.teb32->access([&](TEB32& teb) { this->fill_batch(teb, record, used_bytes); });
                }
                else
                {
                    thread.teb64->access([&](TEB64& teb) { this->fill_batch(teb, record, used_bytes); });
                }
            }

            std::unique_ptr<windows_emulator> emu{};
            std::filesystem::path directory{};
            hdc dc{};
            emulator_pointer scratch{};
        };

        TEST_P(GdiTextRenderTest, DrawsChineseGlyphsWithRealAdvances)
        {
            const auto reference = this->fixture_reference();
            this->select_fixture_font();
            this->draw_text(2, 2, u"中文");

            const auto ascent = this->ascent_of(reference);
            const auto first = this->box_of(reference, 2, 2, 2 + ascent);
            const auto second = this->box_of(reference, 3, 2 + this->advance_of(reference, 2), 2 + ascent);

            EXPECT_EQ(k_black, this->pixel(first.left, first.top));
            EXPECT_EQ(k_black, this->pixel(first.right - 1, first.bottom - 1));
            EXPECT_EQ(k_white, this->pixel(first.right, (first.top + first.bottom) / 2));
            EXPECT_EQ(k_black, this->pixel(second.left, second.bottom - 1));
            EXPECT_EQ(k_black, this->pixel(second.right - 1, second.bottom - 1));
            EXPECT_EQ(k_white, this->pixel(second.right, (second.top + second.bottom) / 2));
        }

        TEST_P(GdiTextRenderTest, FillsOpaqueBackgroundAcrossTheRunBox)
        {
            constexpr COLORREF orange = 0x000080FF;
            const auto reference = this->fixture_reference();
            const auto advance = this->advance_of(reference, 2);
            const auto metrics = this->emu->process.gdi_font_resources.metrics(reference);
            this->emu->memory.write_memory(this->dc_attr() + k_background_offset, &orange, sizeof(orange));
            this->select_fixture_font();
            this->draw_text(2, 2, u"中");

            EXPECT_EQ(orange, this->pixel(2, 2));
            EXPECT_EQ(orange, this->pixel(2 + advance - 1, 2 + metrics.ascent + metrics.descent - 1));
            EXPECT_EQ(k_black, this->pixel(2 + advance / 2, 2 + metrics.ascent));
            EXPECT_EQ(k_white, this->pixel(2 + advance, 2));
            EXPECT_EQ(k_white, this->pixel(2, 2 + metrics.ascent + metrics.descent));
        }

        TEST_P(GdiTextRenderTest, HonorsGlyphIndexInputWithoutRemapping)
        {
            const auto reference = this->fixture_reference();
            this->select_fixture_font();
            const std::array<char16_t, 2> glyphs{2, 1};
            this->emu->memory.write_memory(this->scratch, glyphs.data(), glyphs.size() * sizeof(char16_t));
            ASSERT_EQ(TRUE, syscalls::handle_NtGdiExtTextOutW(this->context(), this->dc, 2, 2, k_eto_glyph_index, 0, this->scratch,
                                                              static_cast<UINT>(glyphs.size()), 0, 0));

            const auto ascent = this->ascent_of(reference);
            const auto first = this->box_of(reference, 2, 2, 2 + ascent);
            const auto second = this->box_of(reference, 1, 2 + this->advance_of(reference, 2), 2 + ascent);

            EXPECT_EQ(k_black, this->pixel(first.left, first.top));
            EXPECT_EQ(k_black, this->pixel(first.right - 1, first.bottom - 1));
            EXPECT_EQ(k_black, this->pixel(second.left, second.bottom - 1));
            EXPECT_EQ(k_black, this->pixel(second.right - 1, second.bottom - 1));
            EXPECT_EQ(k_white, this->pixel(second.right, (second.top + second.bottom) / 2));
        }

        TEST_P(GdiTextRenderTest, BatchedTextOutUsesTheRecordedFont)
        {
            const auto reference = this->fixture_reference();
            const auto font = this->select_fixture_font();
            this->queue_text_out(font, 2, 2, u"中", false);

            ASSERT_TRUE(syscalls::handle_NtGdiFlush(this->context()));

            const auto box = this->box_of(reference, 2, 2, 2 + this->ascent_of(reference));
            EXPECT_EQ(k_black, this->pixel(box.left, box.top));
            EXPECT_EQ(k_black, this->pixel(box.right - 1, box.bottom - 1));
            EXPECT_EQ(k_white, this->pixel(box.right, (box.top + box.bottom) / 2));
            EXPECT_EQ(0u, this->emu->vcpu(0).active_thread->teb64->read().GdiTebBatch.Offset);
        }

        TEST_P(GdiTextRenderTest, Wow64BatchInTheTeb32IsNotMaskedByTheEmptyNativeBatch)
        {
            if (!this->emu->process.is_wow64_process)
            {
                GTEST_SKIP() << "TEB32 only exists for WoW64 threads";
            }

            const auto reference = this->fixture_reference();
            const auto font = this->select_fixture_font();
            this->queue_text_out(font, 2, 2, u"中", true);

            ASSERT_TRUE(syscalls::handle_NtGdiFlush(this->context()));

            const auto box = this->box_of(reference, 2, 2, 2 + this->ascent_of(reference));
            EXPECT_EQ(k_black, this->pixel(box.left, box.top));
            EXPECT_EQ(k_black, this->pixel(box.right - 1, box.bottom - 1));
            EXPECT_EQ(0u, this->emu->vcpu(0).active_thread->teb32->read().GdiTebBatch.Offset);
        }

        TEST_P(GdiTextRenderTest, IgnoresMalformedBatchPayloads)
        {
            const auto reference = this->fixture_reference();
            const auto font = this->select_fixture_font();
            this->queue_text_out(font, 2, 2, u"中", false);
            auto& thread = *this->emu->vcpu(0).active_thread;
            thread.teb64->access([&](TEB64& teb) {
                auto* record = reinterpret_cast<batched_text_out*>(teb.GdiTebBatch.Buffer);
                record->dx_offset = 0xFFFF; // Points past the command payload.
            });

            EXPECT_TRUE(syscalls::handle_NtGdiFlush(this->context()));
            const auto box = this->box_of(reference, 2, 2, 2 + this->ascent_of(reference));
            EXPECT_EQ(k_white, this->pixel(box.left, box.top));
        }

        TEST_P(GdiTextRenderTest, ReportsRealMetricsNamesAndGlyphData)
        {
            const auto reference = this->fixture_reference();
            this->select_fixture_font();

            const auto metrics = this->scratch + 0x100;
            ASSERT_NE(syscalls::handle_NtGdiGetTextMetricsW(this->context(), this->dc, metrics, 0x3C), 0u);
            const auto font_metrics = this->emu->process.gdi_font_resources.metrics(reference);
            EXPECT_EQ(font_metrics.ascent + font_metrics.descent, this->emu->memory.read_memory<int32_t>(metrics + 0x00));
            EXPECT_EQ(font_metrics.ascent, this->emu->memory.read_memory<int32_t>(metrics + 0x04));
            EXPECT_EQ(font_metrics.descent, this->emu->memory.read_memory<int32_t>(metrics + 0x08));
            EXPECT_EQ(font_metrics.average_width, this->emu->memory.read_memory<int32_t>(metrics + 0x14));
            EXPECT_EQ(font_metrics.maximum_width, this->emu->memory.read_memory<int32_t>(metrics + 0x18));

            const auto face = this->scratch + 0x200;
            ASSERT_EQ(syscalls::handle_NtGdiGetTextFaceW(this->context(), this->dc, 32, face, 0), 14);
            const auto expected_face =
                std::array<char16_t, 13>{u'S', u'o', u'g', u'e', u'n', u' ', u'F', u'i', u'x', u't', u'u', u'r', u'e'};
            const auto actual_face = this->emu->memory.read_memory<std::array<char16_t, 13>>(face);
            EXPECT_EQ(expected_face, actual_face);

            const std::u16string text = u"中文Z";
            this->emu->memory.write_memory(this->scratch, text.data(), text.size() * sizeof(char16_t));
            const auto indices = this->scratch + 0x300;
            ASSERT_EQ(3u, syscalls::handle_NtGdiGetGlyphIndicesW(this->context(), this->dc, this->scratch,
                                                                 static_cast<int32_t>(text.size()), indices, 0));
            const auto expected_indices = std::array<uint16_t, 3>{2, 3, 0};
            EXPECT_EQ(expected_indices, (this->emu->memory.read_memory<std::array<uint16_t, 3>>(indices)));
            ASSERT_EQ(3u, syscalls::handle_NtGdiGetGlyphIndicesW(this->context(), this->dc, this->scratch,
                                                                 static_cast<int32_t>(text.size()), indices, 1));
            const auto expected_marked = std::array<uint16_t, 3>{2, 3, 0xFFFF};
            EXPECT_EQ(expected_marked, (this->emu->memory.read_memory<std::array<uint16_t, 3>>(indices)));

            const auto width_buffer = this->scratch + 0x400;
            ASSERT_EQ(TRUE, syscalls::handle_NtGdiGetCharWidthW(this->context(), this->dc, 0, 2, this->scratch, 2, width_buffer));
            const auto expected_widths = std::array<int32_t, 2>{8, 16};
            EXPECT_EQ(expected_widths, (this->emu->memory.read_memory<std::array<int32_t, 2>>(width_buffer)));
            ASSERT_EQ(TRUE, syscalls::handle_NtGdiGetCharWidthW(this->context(), this->dc, 0, 2, this->scratch, 0, width_buffer));
            const auto expected_float_widths = std::array<float, 2>{8.0f, 16.0f};
            EXPECT_EQ(expected_float_widths, (this->emu->memory.read_memory<std::array<float, 2>>(width_buffer)));

            const auto abc_buffer = this->scratch + 0x500;
            ASSERT_EQ(TRUE, syscalls::handle_NtGdiGetCharABCWidthsW(this->context(), this->dc, 0, 2, this->scratch, 1, abc_buffer));
            const auto abcs = this->emu->memory.read_memory<std::array<ABC, 2>>(abc_buffer);
            EXPECT_EQ(0, abcs[0].abcA);
            EXPECT_EQ(8u, abcs[0].abcB);
            EXPECT_EQ(0, abcs[0].abcC);
            EXPECT_EQ(1, abcs[1].abcA);
            EXPECT_EQ(14u, abcs[1].abcB);
            EXPECT_EQ(1, abcs[1].abcC);

            const auto size = this->scratch + 0x600;
            ASSERT_EQ(TRUE, syscalls::handle_NtGdiGetTextExtent(this->context(), this->dc, this->scratch, 2, size, 0));
            const auto text_size = this->emu->memory.read_memory<SIZE>(size);
            EXPECT_EQ(text_size.cx, this->advance_of(reference, 2) + this->advance_of(reference, 3));
            EXPECT_EQ(text_size.cy, font_metrics.ascent + font_metrics.descent);

            const auto fit = this->scratch + 0x610;
            const auto extents = this->scratch + 0x620;
            const emulator_object<ULONG> fit_count{this->emu->memory, fit};
            ASSERT_EQ(TRUE,
                      syscalls::handle_NtGdiGetTextExtentExW(this->context(), this->dc, this->scratch, 2, 24, fit_count, extents, size, 0));
            EXPECT_EQ(1u, this->emu->memory.read_memory<ULONG>(fit));
            const auto expected_extents = std::array<ULONG, 2>{16, 32};
            EXPECT_EQ(expected_extents, (this->emu->memory.read_memory<std::array<ULONG, 2>>(extents)));
        }

        TEST_P(GdiTextRenderTest, UpdatesCurrentPointOnlyForUpdateCpAlignment)
        {
            const auto reference = this->fixture_reference();
            this->select_fixture_font();
            const uint64_t attr = this->dc_attr();
            const std::array initial_point{5, 5};
            this->emu->memory.write_memory(attr + k_current_point_offset, initial_point.data(), sizeof(int32_t));
            this->emu->memory.write_memory(attr + k_current_point_offset + sizeof(int32_t), initial_point.data() + 1, sizeof(int32_t));
            const uint32_t update_cp = 1;
            this->emu->memory.write_memory(attr + k_text_align_offset, &update_cp, sizeof(update_cp));

            this->draw_text(99, 99, u"中");
            const auto box = this->box_of(reference, 2, 5, 5 + this->ascent_of(reference));
            EXPECT_EQ(k_black, this->pixel(box.left, box.top));
            EXPECT_EQ(k_white, this->pixel(2 + (box.left - 5), 2));
            EXPECT_EQ(5 + this->advance_of(reference, 2), this->emu->memory.read_memory<int32_t>(attr + k_current_point_offset));
            EXPECT_EQ(5, this->emu->memory.read_memory<int32_t>(attr + k_current_point_offset + sizeof(int32_t)));

            const uint32_t left_top = 0;
            this->emu->memory.write_memory(attr + k_text_align_offset, &left_top, sizeof(left_top));
            const std::array fixed_point{7, 7};
            this->emu->memory.write_memory(attr + k_current_point_offset, fixed_point.data(), sizeof(int32_t));
            this->emu->memory.write_memory(attr + k_current_point_offset + sizeof(int32_t), fixed_point.data() + 1, sizeof(int32_t));
            this->draw_text(20, 20, u"中");
            EXPECT_EQ(7, this->emu->memory.read_memory<int32_t>(attr + k_current_point_offset));
        }

        INSTANTIATE_TEST_SUITE_P(NativeAndWow64, GdiTextRenderTest, testing::Bool());
    }
}
