#include <gtest/gtest.h>

#include <gui/macos_display_state.hpp>

#include <bit>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

namespace
{
    // Offsets of the wire form MIG 34003 hands over, measured by tracing every CFDataGetBytes call the
 // SkyLight client makes while it decodes one:.
    constexpr size_t header_bytes = 36;
    constexpr size_t display_prefix_bytes = 216;
    constexpr size_t mode_bytes = 132;
    constexpr size_t display_tail_bytes = 67;

    constexpr size_t display_at = header_bytes;
    constexpr size_t mode_count_at = display_at + display_prefix_bytes;
    constexpr size_t mode_at = mode_count_at + sizeof(uint32_t);
    constexpr size_t tail_at = mode_at + mode_bytes;

    uint32_t u32_at(const std::span<const uint8_t> blob, const size_t offset)
    {
        uint32_t value = 0;
        std::memcpy(&value, blob.data() + offset, sizeof(value));
        return value;
    }

    uint64_t u64_at(const std::span<const uint8_t> blob, const size_t offset)
    {
        uint64_t value = 0;
        std::memcpy(&value, blob.data() + offset, sizeof(value));
        return value;
    }

    float f32_at(const std::span<const uint8_t> blob, const size_t offset)
    {
        return std::bit_cast<float>(u32_at(blob, offset));
    }

    double f64_at(const std::span<const uint8_t> blob, const size_t offset)
    {
        return std::bit_cast<double>(u64_at(blob, offset));
    }

    std::vector<uint8_t> one_display(const double width, const double height, const uint32_t id = 1)
    {
        const sogen::macos_display_description display{.id = id, .width = width, .height = height};
        return sogen::macos_build_display_system_state(std::span{&display, 1}, sogen::MACOS_DISPLAY_STATE_GENERATION);
    }

    TEST(DisplayState, OneDisplayIsExactlyTheMeasuredNumberOfBytes)
    {
        const auto blob = one_display(640.0, 480.0);
        EXPECT_EQ(blob.size(), header_bytes + display_prefix_bytes + sizeof(uint32_t) + mode_bytes + display_tail_bytes);
    }

    TEST(DisplayState, TheHeaderCarriesTheGenerationAndTheDisplayCount)
    {
        const auto blob = one_display(640.0, 480.0);

        EXPECT_EQ(u32_at(blob, 0x00), 0u);
        EXPECT_EQ(u64_at(blob, 0x04), sogen::MACOS_DISPLAY_STATE_GENERATION) << "the client compares this against the generation it sent";
        EXPECT_EQ(u32_at(blob, 0x0c), 0x58u);
        EXPECT_EQ(u32_at(blob, 0x10), 0x14u);
        EXPECT_EQ(u32_at(blob, 0x14), 1u);
        EXPECT_EQ(u32_at(blob, 0x18), 0u);
        EXPECT_EQ(u32_at(blob, 0x1c), 0u);
        EXPECT_EQ(u32_at(blob, 0x20), 1u);
    }

    TEST(DisplayState, TheDisplayRecordCarriesTheIdBoundsAndUuidTheClientReadsBack)
    {
        const auto blob = one_display(640.0, 480.0, 7);

        EXPECT_EQ(u32_at(blob, display_at + 0x00), 6u);
        EXPECT_EQ(u32_at(blob, display_at + 0x04), 7u) << "CGSDisplaySystemState::displayByDisplayID matches on this";

        const auto uuid = sogen::macos_display_uuid(7);
        EXPECT_TRUE(std::equal(uuid.begin(), uuid.end(), blob.begin() + display_at + 0x3c));

        EXPECT_EQ(f64_at(blob, display_at + 0x4c), 0.0);
        EXPECT_EQ(f64_at(blob, display_at + 0x54), 0.0);
        EXPECT_EQ(f64_at(blob, display_at + 0x5c), 640.0);
        EXPECT_EQ(f64_at(blob, display_at + 0x64), 480.0);

        EXPECT_EQ(f64_at(blob, display_at + 0x8c), 0.0);
        EXPECT_EQ(f64_at(blob, display_at + 0x94), 0.0);
        EXPECT_EQ(f64_at(blob, display_at + 0x9c), 640.0) << "a backing scale of one makes the pixel bounds the point bounds";
        EXPECT_EQ(f64_at(blob, display_at + 0xa4), 480.0);
    }

    TEST(DisplayState, TheDisplayCarriesExactlyOneModeThatMatchesItsGeometry)
    {
        const auto blob = one_display(640.0, 480.0);

        ASSERT_EQ(u32_at(blob, mode_count_at), 1u);
        EXPECT_EQ(u32_at(blob, mode_at + 0x04), 0u);
        EXPECT_EQ(f32_at(blob, mode_at + 0x14), 60.0f);
        EXPECT_EQ(u32_at(blob, mode_at + 0x18), 640u);
        EXPECT_EQ(u32_at(blob, mode_at + 0x1c), 480u);
        EXPECT_EQ(u32_at(blob, mode_at + 0x20), 640u);
        EXPECT_EQ(u32_at(blob, mode_at + 0x24), 480u);
        EXPECT_EQ(u32_at(blob, mode_at + 0x28), 640u * 4) << "bytes per row of a 32-bit surface";
        EXPECT_EQ(f32_at(blob, mode_at + 0x34), 1.0f);

        const std::string_view encoding{reinterpret_cast<const char*>(blob.data()) + mode_at + 0x38};
        EXPECT_EQ(encoding, "--------RRRRRRRRGGGGGGGGBBBBBBBB");
        EXPECT_EQ(blob[mode_at + 0x38 + 32], 0u) << "the encoding is a fixed 33-byte field, terminator included";

        EXPECT_EQ(u32_at(blob, mode_at + 0x5d), 32u);
        EXPECT_EQ(u32_at(blob, mode_at + 0x61), 8u);
        EXPECT_EQ(u32_at(blob, mode_at + 0x65), 3u);

        EXPECT_EQ(u32_at(blob, tail_at + 0x00), 0u) << "the only mode is the current one";
    }

    TEST(DisplayState, TheMenubarHeightIsWhatTheDescriptionAsksFor)
    {
        sogen::macos_display_description display{.id = 1, .width = 640.0, .height = 480.0};
        const auto silent = sogen::macos_build_display_system_state(std::span{&display, 1}, sogen::MACOS_DISPLAY_STATE_GENERATION);
        EXPECT_EQ(u32_at(silent, tail_at + 0x34), 0u);

        display.menubar_height = 39;
        const auto reserved = sogen::macos_build_display_system_state(std::span{&display, 1}, sogen::MACOS_DISPLAY_STATE_GENERATION);
        EXPECT_EQ(u32_at(reserved, tail_at + 0x34), 39u) << "SLSGetDisplayMenubarHeight reads this word straight out of the record";
    }

    TEST(DisplayState, ARetinaDisplayReportsPixelBoundsAndAModeScaledPastItsPoints)
    {
        const sogen::macos_display_description display{.id = 1, .width = 640.0, .height = 480.0, .scale = 2.0};
        const auto blob = sogen::macos_build_display_system_state(std::span{&display, 1}, sogen::MACOS_DISPLAY_STATE_GENERATION);

        EXPECT_EQ(f64_at(blob, display_at + 0x9c), 1280.0);
        EXPECT_EQ(f64_at(blob, display_at + 0xa4), 960.0);
        EXPECT_EQ(u32_at(blob, mode_at + 0x18), 1280u);
        EXPECT_EQ(u32_at(blob, mode_at + 0x1c), 960u);
        EXPECT_EQ(u32_at(blob, mode_at + 0x20), 640u);
        EXPECT_EQ(u32_at(blob, mode_at + 0x24), 480u);
        EXPECT_EQ(f32_at(blob, mode_at + 0x34), 2.0f);
        EXPECT_EQ(u32_at(blob, mode_at + 0x2c), 192u) << "the physical extent is fixed, so a doubled mode doubles the DPI";
    }

    TEST(DisplayState, EveryDisplayGetsItsOwnWellFormedUuid)
    {
        const auto one = sogen::macos_display_uuid(1);
        const auto two = sogen::macos_display_uuid(2);

        EXPECT_NE(one, two);
        EXPECT_EQ(one[6] & 0xf0, 0x40) << "CFUUIDCreateString wants an RFC 4122 version nibble";
        EXPECT_EQ(one[8] & 0xc0, 0x80) << "and its variant bits";
        EXPECT_EQ(one[15], 1u);
        EXPECT_EQ(two[15], 2u);
    }

    TEST(DisplayState, TwoDisplaysAreTwoRecordsBackToBack)
    {
        const std::array<sogen::macos_display_description, 2> displays{
            sogen::macos_display_description{.id = 1, .width = 640.0, .height = 480.0},
            sogen::macos_display_description{.id = 2, .x = 640.0, .width = 320.0, .height = 240.0},
        };

        const auto blob = sogen::macos_build_display_system_state(displays, sogen::MACOS_DISPLAY_STATE_GENERATION);
        const auto record = display_prefix_bytes + sizeof(uint32_t) + mode_bytes + display_tail_bytes;

        ASSERT_EQ(blob.size(), header_bytes + 2 * record);
        EXPECT_EQ(u32_at(blob, 0x20), 2u);
        EXPECT_EQ(u32_at(blob, display_at + 0x04), 1u);
        EXPECT_EQ(u32_at(blob, display_at + record + 0x04), 2u);
        EXPECT_EQ(f64_at(blob, display_at + record + 0x4c), 640.0);
    }
}
