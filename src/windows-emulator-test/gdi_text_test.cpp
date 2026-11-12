#include "emulation_test_utils.hpp"
#include "gdi_font_test_utils.hpp"

#include <file_system.hpp>
#include <gdi_font_manager.hpp>
#include <gdi_text.hpp>
#include <registry/registry_manager.hpp>

#include <atomic>
#include <fstream>

namespace sogen::test
{
    namespace
    {
        class GdiTextTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                static std::atomic_uint64_t counter{};
                const auto path = std::filesystem::temp_directory_path() /
                                  ("sogen-gdi-text-test-" + std::to_string(getpid()) + "-" + std::to_string(counter++));
                ASSERT_TRUE(std::filesystem::create_directory(path));
                this->directory = path;
                this->font_directory = path / "filesys" / "c" / "windows" / "Fonts";
                ASSERT_TRUE(std::filesystem::create_directories(this->font_directory));
                this->files = std::make_unique<file_system>(path / "filesys");
            }

            void TearDown() override
            {
                this->fonts.clear();
                this->files.reset();
                if (!this->directory.empty())
                {
                    std::error_code error{};
                    std::filesystem::remove_all(this->directory, error);
                }
            }

            void store(const std::string_view name, const font_fixture::bytes& bytes)
            {
                std::ofstream output(this->font_directory / name, std::ios::binary);
                output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                ASSERT_TRUE(output.good());
            }

            EMU_LOGFONTW request(const std::u16string_view family = u"Sogen Fixture", const LONG height = -16) const
            {
                EMU_LOGFONTW result{};
                result.lfHeight = height;
                result.lfWeight = FW_NORMAL;
                result.lfCharSet = DEFAULT_CHARSET;
                utils::string::copy(result.lfFaceName, family);
                return result;
            }

            gdi_font_reference resolve(const EMU_LOGFONTW& font, const char32_t character = 0)
            {
                const auto resolved = this->fonts.resolve(*this->files, this->registry, this->system_root, font, character);
                EXPECT_TRUE(resolved.has_value());
                return resolved.value_or(gdi_font_reference{});
            }

            gdi_text::font_source source()
            {
                return {this->fonts, *this->files, this->registry, this->system_root};
            }

            std::vector<gdi_text::measured_cell> cells_of(const gdi_text::font_source& source, const EMU_LOGFONTW& logfont,
                                                          const std::u16string_view text)
            {
                std::vector<gdi_text::measured_cell> cells{};
                gdi_text::measure(source, logfont, text, gdi_text::glyph_input::characters, {}, false, 8,
                                  [&](const gdi_text::measured_cell& cell) { cells.push_back(cell); });
                return cells;
            }

            std::filesystem::path directory{};
            std::filesystem::path font_directory{};
            std::unique_ptr<file_system> files{};
            registry_manager registry{};
            gdi_font_manager fonts{};
            windows_path system_root{"C:\\Windows"};
        };

        TEST_F(GdiTextTest, LaysOutMixedTextWithRealAdvances)
        {
            this->store("fixture.ttf", font_fixture::make_font());
            const auto logfont = this->request();
            const auto run = gdi_text::layout(this->source(), logfont, u"A中文", gdi_text::glyph_input::characters, {}, false, 8);

            ASSERT_TRUE(run.physical);
            EXPECT_EQ(run.ascent, 16);
            EXPECT_EQ(run.descent, 4);
            EXPECT_EQ(run.width, 40);
            ASSERT_EQ(run.cells.size(), 3);

            EXPECT_EQ(run.cells[0].origin, 0);
            EXPECT_EQ(run.cells[0].advance, 8);
            EXPECT_EQ(run.cells[0].index, 1);
            EXPECT_EQ(run.cells[1].origin, 8);
            EXPECT_EQ(run.cells[1].advance, 16);
            EXPECT_EQ(run.cells[1].index, 2);
            EXPECT_EQ(run.cells[1].bearing_x_26_6, 64);
            EXPECT_EQ(run.cells[1].width_26_6, 14 * 64);
            EXPECT_EQ(run.cells[2].origin, 24);
            EXPECT_EQ(run.cells[2].advance, 16);
            EXPECT_EQ(run.cells[2].index, 3);
        }

        TEST_F(GdiTextTest, UsesGlyphIndicesWithoutRemappingThem)
        {
            this->store("fixture.ttf", font_fixture::make_font());
            const auto logfont = this->request();
            const char16_t glyphs[] = u"\x0002\x0001";
            const auto run = gdi_text::layout(this->source(), logfont, glyphs, gdi_text::glyph_input::indices, {}, false, 8);

            ASSERT_TRUE(run.physical);
            ASSERT_EQ(run.cells.size(), 2);
            EXPECT_EQ(run.cells[0].index, 2);
            EXPECT_EQ(run.cells[0].advance, 16);
            EXPECT_EQ(run.cells[1].index, 1);
            EXPECT_EQ(run.cells[1].advance, 8);
            EXPECT_EQ(run.width, 24);
        }

        TEST_F(GdiTextTest, FallsBackPerCharacterForMissingCoverage)
        {
            this->store("latin.ttf", font_fixture::make_font(u"Fixture Latin", false));
            this->store("cjk.ttf", font_fixture::make_font(u"Fixture CJK"));
            const auto logfont = this->request(u"Fixture Latin");
            const auto run = gdi_text::layout(this->source(), logfont, u"A中", gdi_text::glyph_input::characters, {}, false, 8);

            ASSERT_TRUE(run.physical);
            ASSERT_EQ(run.cells.size(), 2);
            EXPECT_EQ(run.cells[0].index, 1);
            EXPECT_EQ(run.cells[0].advance, 8);
            EXPECT_EQ(run.cells[1].index, 2);
            EXPECT_EQ(run.cells[1].advance, 16);
            EXPECT_NE(run.cells[1].font.file, run.cells[0].font.file);
            EXPECT_EQ(run.width, 24);
        }

        TEST_F(GdiTextTest, MarksMissingGlyphWithNotDefMetricsWhenNothingCoversIt)
        {
            this->store("latin.ttf", font_fixture::make_font(u"Fixture Latin", false));
            const auto logfont = this->request(u"Fixture Latin");
            const auto run = gdi_text::layout(this->source(), logfont, u"中", gdi_text::glyph_input::characters, {}, false, 8);

            ASSERT_TRUE(run.physical);
            ASSERT_EQ(run.cells.size(), 1);
            EXPECT_TRUE(run.cells[0].missing);
            EXPECT_EQ(run.cells[0].index, 0);
            EXPECT_EQ(run.cells[0].advance, 8);
        }

        TEST_F(GdiTextTest, DecodesSurrogatePairsIntoSingleCells)
        {
            this->store("fixture.ttf", font_fixture::make_font());
            const auto logfont = this->request();
            const std::array<char16_t, 4> text{u'A', 0xD800, 0xDC00, u'B'};
            const auto view = std::u16string_view(text.data(), text.size());
            const auto run = gdi_text::layout(this->source(), logfont, view, gdi_text::glyph_input::characters, {}, false, 8);

            ASSERT_TRUE(run.physical);
            ASSERT_EQ(run.cells.size(), 3);
            EXPECT_EQ(run.cells[1].unit_count, 2);
            EXPECT_TRUE(run.cells[1].missing);
            EXPECT_EQ(run.cells[2].origin, 16);
            EXPECT_EQ(run.width, 24);
        }

        TEST_F(GdiTextTest, AppliesSignedAdvancesAndPdyShifts)
        {
            this->store("fixture.ttf", font_fixture::make_font());
            const auto logfont = this->request();
            const std::array advances{12, -4, 30};
            const auto run = gdi_text::layout(this->source(), logfont, u"ABC", gdi_text::glyph_input::characters, advances, false, 8);

            ASSERT_EQ(run.cells.size(), 3);
            EXPECT_EQ(run.cells[0].advance, 12);
            EXPECT_EQ(run.cells[1].advance, -4);
            EXPECT_EQ(run.cells[1].origin, 12);
            EXPECT_EQ(run.cells[2].origin, 8);
            EXPECT_EQ(run.width, 38);

            const std::array pdy{10, 2, 20, -3};
            const auto shifted = gdi_text::layout(this->source(), logfont, u"AB", gdi_text::glyph_input::characters, pdy, true, 8);
            ASSERT_EQ(shifted.cells.size(), 2);
            EXPECT_EQ(shifted.cells[0].advance, 10);
            EXPECT_EQ(shifted.cells[0].shift_y, 2);
            EXPECT_EQ(shifted.cells[1].advance, 20);
            EXPECT_EQ(shifted.cells[1].shift_y, -3);
            EXPECT_EQ(shifted.width, 30);
        }

        TEST_F(GdiTextTest, MeasuresCumulativeWidthsPerUnit)
        {
            this->store("fixture.ttf", font_fixture::make_font());
            const auto logfont = this->request();
            const auto source = this->source();
            const std::array<char16_t, 4> text{u'A', 0xD800, 0xDC00, u'B'};
            const auto view = std::u16string_view(text.data(), text.size());
            const auto measured = gdi_text::measure(source, logfont, view, gdi_text::glyph_input::characters, {}, false, 8, {});
            EXPECT_TRUE(measured.physical);
            EXPECT_EQ(measured.width, 24);

            const auto cells = this->cells_of(source, logfont, view);
            ASSERT_EQ(cells.size(), 3);
            EXPECT_EQ(cells[1].unit_count, 2);
            const auto pair_width = cells[1].advance;
            EXPECT_EQ(cells[1].origin + pair_width / 2, 12);
            EXPECT_EQ(cells[1].origin + pair_width, 16);
        }

        TEST_F(GdiTextTest, MeasuresLargeTextsInBlocks)
        {
            this->store("fixture.ttf", font_fixture::make_font());
            const auto logfont = this->request();
            const std::u16string text(0x5000, u'A');
            size_t visited_units = 0;
            const auto measured = gdi_text::measure(this->source(), logfont, text, gdi_text::glyph_input::characters, {}, false, 8,
                                                    [&](const gdi_text::measured_cell& cell) { visited_units += cell.unit_count; });

            EXPECT_TRUE(measured.physical);
            EXPECT_EQ(measured.width, 0x5000 * 8);
            EXPECT_EQ(visited_units, text.size());
        }

        TEST_F(GdiTextTest, MeasuresDeterministicallyWithoutAFont)
        {
            const auto measured =
                gdi_text::measure(this->source(), std::nullopt, u"A中", gdi_text::glyph_input::characters, {}, false, 8, {});
            EXPECT_FALSE(measured.physical);
            EXPECT_EQ(measured.width, 16);

            std::vector<gdi_text::measured_cell> cells{};
            gdi_text::measure(this->source(), std::nullopt, u"A中", gdi_text::glyph_input::characters, {}, false, 8,
                              [&](const gdi_text::measured_cell& cell) { cells.push_back(cell); });
            ASSERT_EQ(cells.size(), 2);
            EXPECT_EQ(cells[0].origin, 0);
            EXPECT_EQ(cells[1].origin, 8);
            EXPECT_EQ(cells[1].a, 0);
            EXPECT_EQ(cells[1].b, 8);
            EXPECT_EQ(cells[1].c, 0);
        }
    }
}
