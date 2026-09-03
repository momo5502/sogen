#include <gtest/gtest.h>

#include <platform/macho.hpp>

#include "fixture_utils.hpp"

namespace
{
    TEST(MachoFixups, Decodes64OffsetRebase)
    {
        const auto value = sogen::macho::decode_64_pointer(0x0010000000008000ULL);

        EXPECT_FALSE(value.bind);
        EXPECT_EQ(value.next, 2u);
        EXPECT_EQ(value.target, 0x8000u);
        EXPECT_EQ(value.high8, 0u);
    }

    TEST(MachoFixups, Decodes64OffsetBind)
    {
        const auto value = sogen::macho::decode_64_pointer(0x8000000000000000ULL);

        EXPECT_TRUE(value.bind);
        EXPECT_EQ(value.next, 0u);
        EXPECT_EQ(value.ordinal, 0u);
        EXPECT_EQ(value.addend, 0u);
    }

    // Both words come from a two-link chain purpose-built to separate a 12-bit next from an 11-bit one:
    // clang -target arm64-apple-macos over a const struct holding two pointers 12288 bytes apart, giving
    // next = 3072 units of 4 bytes. Reading next as 11 bits yields 1024 and lands the walk on a zero word
    // that still decodes as a well-formed chain terminator, so the error is silent. Bit 62 belongs to next
    // here: taken as bind it would turn this rebase into an ordinal-1088 bind of an empty imports table.
    TEST(MachoFixups, Format64NextSpansBit62)
    {
        const auto first = sogen::macho::decode_64_pointer(0x6000000000000440ULL);

        EXPECT_FALSE(first.bind);
        EXPECT_EQ(first.next, 3072u);
        EXPECT_EQ(first.target, 0x440u);
        EXPECT_EQ(first.high8, 0u);

        const auto second = sogen::macho::decode_64_pointer(0x0000000000000440ULL);

        EXPECT_FALSE(second.bind);
        EXPECT_EQ(second.next, 0u);
        EXPECT_EQ(second.target, 0x440u);
    }

    TEST(MachoFixups, DecodesArm64eUserland24Rebase)
    {
        const auto value = sogen::macho::decode_arm64e_pointer(0x01780000000c4000ULL, sogen::macho::DYLD_CHAINED_PTR_ARM64E_USERLAND24);

        EXPECT_FALSE(value.auth);
        EXPECT_FALSE(value.bind);
        EXPECT_EQ(value.next, 47u);
        EXPECT_EQ(value.target, 0xc4000u);
        EXPECT_EQ(value.high8, 0u);
    }

    TEST(MachoFixups, DecodesArm64eAuthRebase)
    {
        const auto dyld_auth_const =
            sogen::macho::decode_arm64e_pointer(0x800987030009078cULL, sogen::macho::DYLD_CHAINED_PTR_ARM64E_USERLAND24);

        EXPECT_TRUE(dyld_auth_const.auth);
        EXPECT_FALSE(dyld_auth_const.bind);
        EXPECT_EQ(dyld_auth_const.next, 1u);
        EXPECT_EQ(dyld_auth_const.key, 0u);
        EXPECT_TRUE(dyld_auth_const.addr_div);
        EXPECT_EQ(dyld_auth_const.diversity, 0x8703u);
        EXPECT_EQ(dyld_auth_const.target, 0x9078cu);

        const auto fixture_data =
            sogen::macho::decode_arm64e_pointer(0x80080000000004f8ULL, sogen::macho::DYLD_CHAINED_PTR_ARM64E_USERLAND24);

        EXPECT_TRUE(fixture_data.auth);
        EXPECT_FALSE(fixture_data.bind);
        EXPECT_EQ(fixture_data.next, 1u);
        EXPECT_EQ(fixture_data.key, 0u);
        EXPECT_FALSE(fixture_data.addr_div);
        EXPECT_EQ(fixture_data.diversity, 0u);
        EXPECT_EQ(fixture_data.target, 0x4f8u);
    }

    // The arm64e counterpart of Format64NextSpansBit62, from the arm64e build of the same source. Here bit
    // 62 really is bind and next stops at bit 61: an auth-bind whose 11-bit next of 1535 lands the walk on
    // the far rebase 12280 bytes away, while a 12-bit read gives 3583 and leaves the segment entirely.
    TEST(MachoFixups, Arm64eNextStopsAtBit61)
    {
        const auto value = sogen::macho::decode_arm64e_pointer(0xeff8000000000000ULL, sogen::macho::DYLD_CHAINED_PTR_ARM64E_USERLAND24);

        EXPECT_TRUE(value.auth);
        EXPECT_TRUE(value.bind);
        EXPECT_EQ(value.next, 1535u);
        EXPECT_EQ(value.ordinal, 0u);
        EXPECT_EQ(value.key, 0u);
        EXPECT_FALSE(value.addr_div);
        EXPECT_EQ(value.diversity, 0u);
    }

    TEST(MachoFixups, Arm64eOrdinalWidthDependsOnTheFormat)
    {
        constexpr uint64_t raw = 0x4000000000ffffffULL;

        const auto legacy = sogen::macho::decode_arm64e_pointer(raw, sogen::macho::DYLD_CHAINED_PTR_ARM64E);
        const auto userland24 = sogen::macho::decode_arm64e_pointer(raw, sogen::macho::DYLD_CHAINED_PTR_ARM64E_USERLAND24);

        EXPECT_TRUE(legacy.bind);
        EXPECT_TRUE(userland24.bind);
        EXPECT_EQ(legacy.ordinal, 0xffffu);
        EXPECT_EQ(userland24.ordinal, 0xffffffu);
    }

    TEST(MachoFixups, Arm64eBindAddendIsSigned)
    {
        const auto negative = sogen::macho::decode_arm64e_pointer(0x4007ffff00000001ULL, sogen::macho::DYLD_CHAINED_PTR_ARM64E);

        EXPECT_TRUE(negative.bind);
        EXPECT_FALSE(negative.auth);
        EXPECT_EQ(negative.ordinal, 1u);
        EXPECT_EQ(negative.addend, -1);

        const auto positive = sogen::macho::decode_arm64e_pointer(0x4000000100000001ULL, sogen::macho::DYLD_CHAINED_PTR_ARM64E);

        EXPECT_EQ(positive.addend, 1);
    }

    TEST(MachoFixups, DecodesSharedCachePlainPointer)
    {
        const auto value = sogen::macho::decode_shared_cache_pointer(0x001000006dc3cb48ULL);

        EXPECT_FALSE(value.auth);
        EXPECT_EQ(value.next, 1u);
        EXPECT_EQ(value.high8, 0u);
        EXPECT_EQ(value.runtime_offset, 0x6dc3cb48u);
        EXPECT_EQ(0x180000000ULL + value.runtime_offset, 0x1edc3cb48ULL);
    }

    TEST(MachoFixups, DecodesSharedCacheAuthPointers)
    {
        const auto data_key = sogen::macho::decode_shared_cache_pointer(0x801dab846cf35e88ULL);

        EXPECT_TRUE(data_key.auth);
        EXPECT_EQ(data_key.next, 1u);
        EXPECT_TRUE(data_key.key_is_data);
        EXPECT_TRUE(data_key.addr_div);
        EXPECT_EQ(data_key.diversity, 0x6ae1u);
        EXPECT_EQ(0x180000000ULL + data_key.runtime_offset, 0x1ecf35e88ULL);

        const auto instr_key = sogen::macho::decode_shared_cache_pointer(0x80140000a6c322ecULL);

        EXPECT_TRUE(instr_key.auth);
        EXPECT_EQ(instr_key.next, 1u);
        EXPECT_FALSE(instr_key.key_is_data);
        EXPECT_TRUE(instr_key.addr_div);
        EXPECT_EQ(instr_key.diversity, 0u);
        EXPECT_EQ(0x180000000ULL + instr_key.runtime_offset, 0x226c322ecULL);
    }

    TEST(MachoFixups, ReportsPointerStrides)
    {
        EXPECT_EQ(sogen::macho::chained_pointer_stride(sogen::macho::DYLD_CHAINED_PTR_ARM64E), 8u);
        EXPECT_EQ(sogen::macho::chained_pointer_stride(sogen::macho::DYLD_CHAINED_PTR_ARM64E_USERLAND), 8u);
        EXPECT_EQ(sogen::macho::chained_pointer_stride(sogen::macho::DYLD_CHAINED_PTR_ARM64E_USERLAND24), 8u);
        EXPECT_EQ(sogen::macho::chained_pointer_stride(sogen::macho::DYLD_CHAINED_PTR_ARM64E_SHARED_CACHE), 8u);
        EXPECT_EQ(sogen::macho::chained_pointer_stride(sogen::macho::DYLD_CHAINED_PTR_64), 4u);
        EXPECT_EQ(sogen::macho::chained_pointer_stride(sogen::macho::DYLD_CHAINED_PTR_64_OFFSET), 4u);
        EXPECT_EQ(sogen::macho::chained_pointer_stride(sogen::macho::DYLD_CHAINED_PTR_32), 4u);
        EXPECT_EQ(sogen::macho::chained_pointer_stride(sogen::macho::DYLD_CHAINED_PTR_32_CACHE), 4u);
        EXPECT_EQ(sogen::macho::chained_pointer_stride(sogen::macho::DYLD_CHAINED_PTR_32_FIRMWARE), 4u);
        EXPECT_EQ(sogen::macho::chained_pointer_stride(sogen::macho::DYLD_CHAINED_PTR_64_KERNEL_CACHE), 4u);
        EXPECT_EQ(sogen::macho::chained_pointer_stride(0), 0u);
        EXPECT_EQ(sogen::macho::chained_pointer_stride(15), 0u);
    }

    // The four formats whose stride does not follow from their name: three arm64e formats stride by 4
    // rather than 8, and the x86_64 kernel cache strides by 1.
    TEST(MachoFixups, ReportsTheIrregularPointerStrides)
    {
        EXPECT_EQ(sogen::macho::chained_pointer_stride(sogen::macho::DYLD_CHAINED_PTR_ARM64E_KERNEL), 4u);
        EXPECT_EQ(sogen::macho::chained_pointer_stride(sogen::macho::DYLD_CHAINED_PTR_ARM64E_FIRMWARE), 4u);
        EXPECT_EQ(sogen::macho::chained_pointer_stride(sogen::macho::DYLD_CHAINED_PTR_ARM64E_SEGMENTED), 4u);
        EXPECT_EQ(sogen::macho::chained_pointer_stride(sogen::macho::DYLD_CHAINED_PTR_X86_64_KERNEL_CACHE), 1u);
    }

    // Formats 13 and 14 are arm64e formats that decode_arm64e_pointer cannot decode: 13 puts next at 52..62
    // and 14 has no bind bit where the decoder reads one. Neither failure is loud, so a caller has to be able
    // to ask first.
    TEST(MachoFixups, IdentifiesTheArm64eFormatsTheDecoderHandles)
    {
        EXPECT_TRUE(sogen::macho::is_arm64e_chained_format(sogen::macho::DYLD_CHAINED_PTR_ARM64E));
        EXPECT_TRUE(sogen::macho::is_arm64e_chained_format(sogen::macho::DYLD_CHAINED_PTR_ARM64E_KERNEL));
        EXPECT_TRUE(sogen::macho::is_arm64e_chained_format(sogen::macho::DYLD_CHAINED_PTR_ARM64E_USERLAND));
        EXPECT_TRUE(sogen::macho::is_arm64e_chained_format(sogen::macho::DYLD_CHAINED_PTR_ARM64E_FIRMWARE));
        EXPECT_TRUE(sogen::macho::is_arm64e_chained_format(sogen::macho::DYLD_CHAINED_PTR_ARM64E_USERLAND24));

        EXPECT_FALSE(sogen::macho::is_arm64e_chained_format(sogen::macho::DYLD_CHAINED_PTR_ARM64E_SHARED_CACHE));
        EXPECT_FALSE(sogen::macho::is_arm64e_chained_format(sogen::macho::DYLD_CHAINED_PTR_ARM64E_SEGMENTED));

        EXPECT_FALSE(sogen::macho::is_arm64e_chained_format(sogen::macho::DYLD_CHAINED_PTR_64));
        EXPECT_FALSE(sogen::macho::is_arm64e_chained_format(sogen::macho::DYLD_CHAINED_PTR_64_OFFSET));
        EXPECT_FALSE(sogen::macho::is_arm64e_chained_format(sogen::macho::DYLD_CHAINED_PTR_32));
        EXPECT_FALSE(sogen::macho::is_arm64e_chained_format(sogen::macho::DYLD_CHAINED_PTR_32_CACHE));
        EXPECT_FALSE(sogen::macho::is_arm64e_chained_format(sogen::macho::DYLD_CHAINED_PTR_32_FIRMWARE));
        EXPECT_FALSE(sogen::macho::is_arm64e_chained_format(sogen::macho::DYLD_CHAINED_PTR_64_KERNEL_CACHE));
        EXPECT_FALSE(sogen::macho::is_arm64e_chained_format(sogen::macho::DYLD_CHAINED_PTR_X86_64_KERNEL_CACHE));

        EXPECT_FALSE(sogen::macho::is_arm64e_chained_format(0));
        EXPECT_FALSE(sogen::macho::is_arm64e_chained_format(15));
    }

    TEST(MachoFixups, DecodesTheRealFixupTableOfTheDynamicFixture)
    {
        const auto data = sogen::test::read_fixture("macho_dylink_arm64");

        std::optional<sogen::macho::linkedit_data_command> fixups{};
        sogen::macho::for_each_load_command(data, 0, [&](const sogen::macho::load_command& lc, std::span<const std::byte> body) {
            if (lc.cmd == sogen::macho::LC_DYLD_CHAINED_FIXUPS)
            {
                fixups = sogen::macho::read_at<sogen::macho::linkedit_data_command>(body, 0);
                return false;
            }

            return true;
        });

        ASSERT_TRUE(fixups.has_value());
        EXPECT_EQ(fixups->dataoff, 32768u);
        EXPECT_EQ(fixups->datasize, 96u);

        const auto header = sogen::macho::read_at<sogen::macho::dyld_chained_fixups_header>(data, fixups->dataoff);
        ASSERT_TRUE(header.has_value());
        EXPECT_EQ(header->fixups_version, 0u);
        EXPECT_EQ(header->starts_offset, 32u);
        EXPECT_EQ(header->imports_offset, 80u);
        EXPECT_EQ(header->symbols_offset, 84u);
        EXPECT_EQ(header->imports_count, 1u);
        EXPECT_EQ(header->imports_format, sogen::macho::DYLD_CHAINED_IMPORT);

        const auto starts_base = fixups->dataoff + header->starts_offset;
        const auto seg_count = sogen::macho::read_at<uint32_t>(data, starts_base);
        ASSERT_TRUE(seg_count.has_value());
        EXPECT_EQ(*seg_count, 4u);

        const auto seg_info_offset = sogen::macho::read_at<uint32_t>(data, starts_base + 4 + 4 * 2);
        ASSERT_TRUE(seg_info_offset.has_value());
        ASSERT_NE(*seg_info_offset, 0u);

        const auto starts = sogen::macho::read_at<sogen::macho::dyld_chained_starts_in_segment>(data, starts_base + *seg_info_offset);
        ASSERT_TRUE(starts.has_value());
        EXPECT_EQ(starts->size, 24u);
        EXPECT_EQ(starts->page_size, 0x4000u);
        EXPECT_EQ(starts->pointer_format, sogen::macho::DYLD_CHAINED_PTR_64_OFFSET);
        EXPECT_EQ(starts->segment_offset, 0x4000u);
        EXPECT_EQ(starts->max_valid_pointer, 0u);
        EXPECT_EQ(starts->page_count, 1u);
        EXPECT_EQ(starts->page_start[0], 0u);

        const auto word = sogen::macho::read_at<uint64_t>(data, 16384 + starts->page_start[0]);
        ASSERT_TRUE(word.has_value());
        EXPECT_EQ(*word, 0x8000000000000000ULL);

        const auto pointer = sogen::macho::decode_64_pointer(*word);
        EXPECT_TRUE(pointer.bind);
        EXPECT_EQ(pointer.ordinal, 0u);
        EXPECT_EQ(pointer.next, 0u);

        const auto import_word = sogen::macho::read_at<uint32_t>(data, fixups->dataoff + header->imports_offset);
        ASSERT_TRUE(import_word.has_value());

        const auto import = sogen::macho::decode_chained_import(*import_word);
        EXPECT_EQ(import.lib_ordinal, 1u);
        EXPECT_FALSE(import.weak_import);

        const auto* symbols = reinterpret_cast<const char*>(data.data()) + fixups->dataoff + header->symbols_offset;
        EXPECT_STREQ(symbols + import.name_offset, "_printf");
    }
}
