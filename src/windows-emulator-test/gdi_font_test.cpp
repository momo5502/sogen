#include "emulation_test_utils.hpp"
#include "gdi_font_test_utils.hpp"

#include <file_system.hpp>
#include <gdi_font_manager.hpp>
#include <registry/registry_manager.hpp>

#include <atomic>
#include <fstream>

namespace sogen::test
{
    namespace
    {
        class GdiFontTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                static std::atomic_uint64_t counter{};
                const auto path = std::filesystem::temp_directory_path() /
                                  ("sogen-font-test-" + std::to_string(getpid()) + "-" + std::to_string(counter++));
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

            EMU_LOGFONTW request(const std::u16string_view family = u"Sogen Fixture", const LONG height = -16)
            {
                EMU_LOGFONTW result{};
                result.lfHeight = height;
                result.lfWeight = FW_NORMAL;
                result.lfCharSet = DEFAULT_CHARSET;
                utils::string::copy(result.lfFaceName, family);
                return result;
            }

            std::optional<gdi_font_reference> resolve(const EMU_LOGFONTW& font, const char32_t character = 0)
            {
                return this->fonts.resolve(*this->files, this->registry, windows_path{"C:\\Windows"}, font, character);
            }

            std::filesystem::path directory{};
            std::filesystem::path font_directory{};
            std::unique_ptr<file_system> files{};
            registry_manager registry{};
            gdi_font_manager fonts{};
        };
    }

    TEST_F(GdiFontTest, ReadsCmapMetricsAndDistinctOwnedBitmaps)
    {
        this->store("fixture.ttf", font_fixture::make_font());
        const auto font = this->resolve(this->request());
        ASSERT_TRUE(font);
        EXPECT_EQ(this->fonts.family_name(*font), u"Sogen Fixture");
        EXPECT_EQ(this->fonts.glyph_index(*font, U'A'), 1u);
        EXPECT_EQ(this->fonts.glyph_index(*font, U'\x4E2D'), 2u);
        EXPECT_EQ(this->fonts.glyph_index(*font, U'\x6587'), 3u);
        EXPECT_EQ(this->fonts.glyph_index(*font, U'Z'), 0u);
        EXPECT_EQ(this->fonts.glyph_index(*font, char32_t{0xD800}), 0u);
        EXPECT_EQ(this->fonts.glyph_metrics(*font, 1).advance_x_26_6, 8 * 64);
        EXPECT_EQ(this->fonts.glyph_metrics(*font, 2).advance_x_26_6, 16 * 64);
        const auto metrics = this->fonts.metrics(*font);
        EXPECT_EQ(metrics.ascent, 16);
        EXPECT_EQ(metrics.descent, 4);
        EXPECT_EQ(metrics.height, 20);
        EXPECT_EQ(metrics.average_width, 8);

        const auto square = this->fonts.rasterize(*font, 2);
        const auto triangle = this->fonts.rasterize(*font, 3);
        EXPECT_EQ(square->width, 14u);
        EXPECT_EQ(square->height, 12u);
        ASSERT_FALSE(square->coverage.empty());
        EXPECT_TRUE(std::ranges::all_of(square->coverage, [](const uint8_t value) { return value == 255; }));
        EXPECT_NE(square->coverage, triangle->coverage);
        const auto saved = square->coverage;
        this->fonts.clear();
        EXPECT_EQ(square->coverage, saved);
    }

    TEST_F(GdiFontTest, SeparatesSizesStylesAndCellHeightFromEmHeight)
    {
        this->store("fixture.ttf", font_fixture::make_font());
        const auto small = this->resolve(this->request());
        const auto large = this->resolve(this->request(u"Sogen Fixture", -32));
        const auto cell = this->resolve(this->request(u"Sogen Fixture", 20));
        const auto em = this->resolve(this->request(u"Sogen Fixture", -20));
        ASSERT_TRUE(small && large && cell && em);
        EXPECT_EQ(cell->pixel_height, 16u);
        EXPECT_EQ(em->pixel_height, 20u);
        EXPECT_EQ(this->fonts.glyph_metrics(*large, 2).advance_x_26_6, 32 * 64);
        EXPECT_EQ(this->fonts.glyph_metrics(*small, 2).advance_x_26_6, 16 * 64);
        const auto cached = this->fonts.rasterize(*small, 2);
        EXPECT_EQ(cached, this->fonts.rasterize(*small, 2));
        EXPECT_NE(cached, this->fonts.rasterize(*large, 2));

        auto bold_request = this->request();
        bold_request.lfWeight = FW_BOLD;
        bold_request.lfItalic = 1;
        const auto bold = this->resolve(bold_request);
        ASSERT_TRUE(bold);
        EXPECT_TRUE(bold->embolden);
        EXPECT_TRUE(bold->oblique);
        EXPECT_NE(cached, this->fonts.rasterize(*bold, 2));
        EXPECT_EQ(cached, this->fonts.rasterize(*small, 2));
    }

    TEST_F(GdiFontTest, PrefersPhysicalBoldFaceAfterRegularFaceIsCached)
    {
        this->store("a.ttf", font_fixture::make_font());
        this->store("b.ttf", font_fixture::make_font(u"Sogen Fixture", true, true));
        const auto regular = this->resolve(this->request());
        auto bold_request = this->request();
        bold_request.lfWeight = FW_BOLD;
        const auto bold = this->resolve(bold_request);
        ASSERT_TRUE(regular && bold);
        EXPECT_NE(regular->file, bold->file);
        EXPECT_FALSE(bold->embolden);
    }

    TEST_F(GdiFontTest, SelectsCollectionFaceAndFallsBackForMissingChineseGlyph)
    {
        const std::u16string chinese_name = u"\x4E2D\x6587\x5B57\x4F53";
        this->store("msyh.ttc", font_fixture::make_collection(
                                    {font_fixture::make_font(u"Fixture Latin", false), font_fixture::make_font(chinese_name)}));
        const auto latin = this->resolve(this->request(u"Fixture Latin"));
        const auto chinese = this->resolve(this->request(chinese_name));
        ASSERT_TRUE(latin && chinese);
        EXPECT_EQ(latin->face, 0u);
        EXPECT_EQ(chinese->face, 1u);
        EXPECT_EQ(latin->file, chinese->file);
        EXPECT_EQ(this->fonts.glyph_index(*latin, U'\x4E2D'), 0u);
        EXPECT_EQ(this->fonts.glyph_index(*chinese, U'\x4E2D'), 2u);
        const auto fallback = this->resolve(this->request(u"Fixture Latin"), U'\x4E2D');
        ASSERT_TRUE(fallback);
        EXPECT_EQ(fallback->file, chinese->file);
        EXPECT_EQ(fallback->face, 1u);
        EXPECT_EQ(this->fonts.family_name(*fallback), chinese_name);
    }

    TEST_F(GdiFontTest, RestoresFrozenCollectionAndSelectionWithoutOriginalFiles)
    {
        this->store("fixture.ttc",
                    font_fixture::make_collection({font_fixture::make_font(u"First", false), font_fixture::make_font(u"Second")}));
        const auto request = this->request(u"Second");
        const auto font = this->resolve(request);
        ASSERT_TRUE(font);
        const auto original = this->fonts.rasterize(*font, 2);
        utils::buffer_serializer saved{};
        this->fonts.serialize(saved);
        this->fonts.clear();
        ASSERT_TRUE(std::filesystem::remove(this->font_directory / "fixture.ttc"));
        utils::buffer_deserializer input{saved};
        this->fonts.deserialize(input);
        const auto restored = this->resolve(request);
        ASSERT_TRUE(restored);
        EXPECT_EQ(*font, *restored);
        EXPECT_EQ(this->fonts.glyph_index(*restored, U'\x4E2D'), 2u);
        const auto bitmap = this->fonts.rasterize(*restored, 2);
        EXPECT_EQ(bitmap->coverage, original->coverage);
        EXPECT_EQ(bitmap->metrics.advance_x_26_6, original->metrics.advance_x_26_6);
    }

    TEST_F(GdiFontTest, RejectsBadFontsReferencesAndRecoveryWithoutLosingState)
    {
        this->store("broken.ttf", font_fixture::bytes(32));
        EXPECT_FALSE(this->resolve(this->request()));
        this->store("fixture.ttf", font_fixture::make_font());
        const auto font = this->resolve(this->request());
        ASSERT_TRUE(font);
        auto invalid = *font;
        invalid.face = 99;
        EXPECT_THROW(this->fonts.metrics(invalid), std::invalid_argument);
        EXPECT_THROW(this->fonts.rasterize(*font, 65535), std::invalid_argument);
        EXPECT_THROW(this->resolve(this->request(u"Sogen Fixture", std::numeric_limits<LONG>::min())), std::invalid_argument);

        utils::buffer_serializer corrupt{};
        corrupt.write(uint32_t{129});
        utils::buffer_deserializer input{corrupt};
        EXPECT_THROW(this->fonts.deserialize(input), std::length_error);
        EXPECT_EQ(this->fonts.glyph_index(*font, U'\x4E2D'), 2u);

        gdi_font_manager empty{};
        utils::buffer_serializer saved{};
        empty.serialize(saved);
        utils::buffer_deserializer empty_input{saved};
        this->fonts.deserialize(empty_input);
        EXPECT_THROW(this->fonts.metrics(*font), std::invalid_argument);
    }
}
