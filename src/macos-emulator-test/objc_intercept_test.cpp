#include <gtest/gtest.h>

#include "macos_test_utils.hpp"

#include <gui/macos_native_dispatch.hpp>
#include <gui/macos_objc_intercept.hpp>
#include <host_range_reader.hpp>
#include <macos_emulator.hpp>
#include <module/dyld_cache_pager.hpp>
#include <module/dyld_cache_slide.hpp>
#include <module/dyld_shared_cache.hpp>
#include <module/macos_cache_symbols.hpp>

#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace
{
    constexpr std::string_view quartzcore = "/System/Library/Frameworks/QuartzCore.framework/Versions/A/QuartzCore";

    std::optional<sogen::dyld_shared_cache_reader> open_host_cache()
    {
        const std::filesystem::path path{MACOS_DYLD_CACHE_HOST_PATH};
        if (!std::filesystem::is_regular_file(path))
        {
            return std::nullopt;
        }

        return sogen::dyld_shared_cache_reader::parse(path, [](const std::filesystem::path& p) { return sogen::open_dyld_cache_file(p); });
    }
}

// Measurement probe for the ObjC method metadata inside the 25G76 shared cache. Disabled because it is a
// measurement, not an assertion. The cache file holds packed chained-fixup entries where guest memory would hold pointers, so
// this decodes slide info v5 chains by hand (same decode as src/macos-emulator/module/dyld_cache_slide.cpp,
// against file bytes).
//
//   ./macos-emulator-test --gtest_also_run_disabled_tests --gtest_filter='ObjcIntercept.DISABLED_*'
namespace
{
    struct region_slide
    {
        uint64_t address{};
        uint64_t size{};
        uint64_t slide_address{};
    };

    uint64_t read_u64(const std::vector<std::byte>& data, const size_t offset)
    {
        uint64_t value = 0;
        std::memcpy(&value, data.data() + offset, sizeof(value));
        return value;
    }

    uint32_t read_u32(const std::vector<std::byte>& data, const size_t offset)
    {
        uint32_t value = 0;
        std::memcpy(&value, data.data() + offset, sizeof(value));
        return value;
    }

    int64_t sign_extend_48(const uint64_t value)
    {
        return static_cast<int64_t>(value << 16) >> 16;
    }

    std::vector<region_slide> collect_slide_regions(const sogen::dyld_shared_cache_reader& reader)
    {
        std::vector<region_slide> result{};

        for (const auto& file : reader.files())
        {
            if (file.regions.empty())
            {
                continue;
            }

            // Address of file offset 0: the first region starts at file offset 0 in every subcache.
            const auto file_base = file.regions.front().address - file.regions.front().file_offset;

            const auto header = reader.read_at_address(file_base, 0x4000);
            if (header.size() < 0x140)
            {
                continue;
            }

            const auto mapping_offset = read_u32(header, sogen::macho::dyld_cache::MAPPING_WITH_SLIDE_OFFSET);
            const auto mapping_count = read_u32(header, sogen::macho::dyld_cache::MAPPING_WITH_SLIDE_COUNT);
            if (mapping_count == 0 || mapping_count > 64)
            {
                continue;
            }

            const auto entries = reader.read_at_address(
                file_base + mapping_offset, mapping_count * sizeof(sogen::macho::dyld_cache::dyld_cache_mapping_and_slide_info));
            if (entries.size() < mapping_count * sizeof(sogen::macho::dyld_cache::dyld_cache_mapping_and_slide_info))
            {
                continue;
            }

            for (uint32_t i = 0; i < mapping_count; ++i)
            {
                const auto base = i * sizeof(sogen::macho::dyld_cache::dyld_cache_mapping_and_slide_info);
                const auto address = read_u64(entries, base + 0);
                const auto size = read_u64(entries, base + 8);
                const auto slide_file_offset = read_u64(entries, base + 24);
                const auto slide_file_size = read_u64(entries, base + 32);
                if (slide_file_size == 0)
                {
                    continue;
                }

                // The slide blob's file offset does not translate through file_base: on 25G76 the blobs
                // sit in the subcache's tail, past every data region's file range, and are mapped by
                // their own small read-only region. Translate through whichever region of this file
                // actually covers that file offset.
                uint64_t slide_address = 0;
                for (const auto& region : file.regions)
                {
                    if (slide_file_offset >= region.file_offset && slide_file_offset < region.file_offset + region.size)
                    {
                        slide_address = region.address + (slide_file_offset - region.file_offset);
                        break;
                    }
                }

                if (slide_address == 0)
                {
                    continue;
                }

                result.push_back(region_slide{.address = address, .size = size, .slide_address = slide_address});
            }
        }

        return result;
    }

    struct decoded_page
    {
        std::map<uint64_t, uint64_t> pointers{};
        uint64_t value_add{};
    };

    decoded_page decode_chains(const sogen::dyld_shared_cache_reader& reader, const std::vector<region_slide>& regions,
                               const uint64_t address)
    {
        decoded_page result{};

        const region_slide* region = nullptr;
        for (const auto& candidate : regions)
        {
            if (address >= candidate.address && address < candidate.address + candidate.size)
            {
                region = &candidate;
                break;
            }
        }

        if (region == nullptr)
        {
            return result;
        }

        const auto header = reader.read_at_address(region->slide_address, 24);
        if (header.size() < 24 || read_u32(header, 0) != 5)
        {
            return result;
        }

        const auto page_size = read_u32(header, 4);
        const auto page_starts_count = read_u32(header, 8);
        result.value_add = read_u64(header, 16);

        const auto page_index = (address - region->address) / page_size;
        if (page_index >= page_starts_count)
        {
            return result;
        }

        const auto page_base = region->address + page_index * page_size;
        const auto starts = reader.read_at_address(region->slide_address + 24 + page_index * 2, 2);
        const auto page = reader.read_at_address(page_base, page_size);
        if (starts.size() < 2 || page.size() < page_size)
        {
            return result;
        }

        uint16_t offset = 0;
        std::memcpy(&offset, starts.data(), 2);
        if (offset == 0xFFFF)
        {
            return result;
        }

        while (offset + sizeof(uint64_t) <= page_size)
        {
            const auto raw = read_u64(page, offset);
            const auto runtime_offset = raw & ((uint64_t{1} << 34) - 1);
            const auto next = static_cast<uint32_t>((raw >> 52) & 0x7FF);
            const bool authenticated = ((raw >> 63) & 1) != 0;

            auto value = result.value_add + runtime_offset;
            if (!authenticated)
            {
                value |= static_cast<uint64_t>((raw >> 34) & 0xFF) << 56;
            }

            result.pointers[page_base + offset] = value;

            if (next == 0)
            {
                break;
            }

            offset += static_cast<uint16_t>(next) * 8;
        }

        return result;
    }

    uint64_t pointer_at(const sogen::dyld_shared_cache_reader& reader, const std::vector<region_slide>& regions, const uint64_t address)
    {
        const auto decoded = decode_chains(reader, regions, address);
        const auto it = decoded.pointers.find(address);
        return it == decoded.pointers.end() ? 0 : it->second;
    }

    std::optional<std::string> printable_cstring(const sogen::dyld_shared_cache_reader& reader, const uint64_t address)
    {
        const auto bytes = reader.read_at_address(address, 96);
        if (bytes.empty())
        {
            return std::nullopt;
        }

        std::string result{};
        for (const auto b : bytes)
        {
            const auto ch = static_cast<char>(b);
            if (ch == '\0')
            {
                break;
            }
            if (ch < 0x20 || ch > 0x7e)
            {
                return std::nullopt;
            }
            result.push_back(ch);
        }

        if (result.empty() || result.size() >= 96)
        {
            return std::nullopt;
        }

        return result;
    }

    std::optional<uint64_t> objc_selector_base(const sogen::dyld_shared_cache_reader& reader)
    {
        const auto& main_file = reader.files().front();
        const auto file_base = main_file.regions.front().address - main_file.regions.front().file_offset;
        const auto header = reader.read_at_address(file_base, 0x400);
        if (header.size() < 0x1d8)
        {
            return std::nullopt;
        }

        const auto opts_vm_offset = read_u64(header, sogen::macho::dyld_cache::OBJC_OPTS_OFFSET);
        if (opts_vm_offset == 0)
        {
            return std::nullopt;
        }

        const uint64_t opts_address = reader.shared_region_start() + opts_vm_offset;
        const auto opt = reader.read_at_address(opts_address, 56);
        if (opt.size() < 56)
        {
            return std::nullopt;
        }

        printf("objc opt @ 0x%llx version=%u flags=0x%x\n", static_cast<unsigned long long>(opts_address), read_u32(opt, 0),
               read_u32(opt, 4));

        // Measured on 25G76: the blob is a version-2 layout of 64-bit VM offsets from the shared region
        // start; +0x30 is the selector string base every relative method name offset is relative to.
        // Cross-checked against the runtime: +[CATransaction flush] resolves to the cstring at
        // start + read_u64(+0x30) + name_off.
        return reader.shared_region_start() + read_u64(opt, 0x30);
    }

    uint64_t strip_pointer(const uint64_t value)
    {
        return value & 0x0000ffffffffffffULL;
    }

    uint32_t g_matched_methods = 0;

    void scan_method_list(const sogen::dyld_shared_cache_reader& reader, const uint64_t selector_base, const uint64_t list,
                          const char* wanted)
    {
        const auto header = reader.read_at_address(list, 8);
        if (header.size() < 8)
        {
            printf("    list @ 0x%llx unreadable\n", static_cast<unsigned long long>(list));
            return;
        }

        const auto entsize_flags = read_u32(header, 0);
        const auto count = read_u32(header, 4);
        const auto entsize = entsize_flags & 0x0000fffc;
        const bool relative = (entsize_flags & 0x80000000) != 0;
        const bool selector_offsets = (entsize_flags & 0x40000000) != 0;
        printf("    list @ 0x%llx entsize_flags=0x%x count=%u relative=%d seloffs=%d\n", static_cast<unsigned long long>(list),
               entsize_flags, count, relative, selector_offsets);
        if (!relative || !selector_offsets || entsize != 12 || count == 0 || count > 1000)
        {
            return;
        }

        const auto bytes = reader.read_at_address(list + 8, static_cast<size_t>(count) * 12);
        for (uint32_t i = 0; i < count; ++i)
        {
            const auto entry = list + 8 + static_cast<uint64_t>(i) * 12;
            const auto off = static_cast<size_t>(i) * 12;
            if (off + 12 > bytes.size())
            {
                break;
            }

            const auto name_off = static_cast<int32_t>(read_u32(bytes, off));
            const auto imp_off = static_cast<int32_t>(read_u32(bytes, off + 8));
            const auto sel_address = static_cast<uint64_t>(static_cast<int64_t>(selector_base) + name_off);
            const auto imp = static_cast<uint64_t>(static_cast<int64_t>(entry + 8) + imp_off);

            const auto name = printable_cstring(reader, sel_address).value_or("?");
            if (name == wanted)
            {
                ++g_matched_methods;
                printf("      [%2u] %-44s sel=0x%llx imp=0x%llx  <== WANTED\n", i, name.c_str(),
                       static_cast<unsigned long long>(sel_address), static_cast<unsigned long long>(imp));
            }
        }
    }

    void scan_class_methods(const sogen::dyld_shared_cache_reader& reader, const std::vector<region_slide>& regions,
                            const uint64_t selector_base, const uint64_t class_address, const char* label, const char* wanted)
    {
        const auto isa = strip_pointer(pointer_at(reader, regions, class_address));
        printf("== %s @ 0x%llx isa=0x%llx ==\n", label, static_cast<unsigned long long>(class_address),
               static_cast<unsigned long long>(isa));

        for (const auto& [side, cls] : {std::pair{"instance", class_address}, std::pair{"class", isa}})
        {
            const auto ro = strip_pointer(pointer_at(reader, regions, cls + 0x20)) & 0x00007ffffffffff8ULL;
            const auto base_methods = strip_pointer(pointer_at(reader, regions, ro + 0x20));
            printf("  %s ro @ 0x%llx baseMethods=0x%llx\n", side, static_cast<unsigned long long>(ro),
                   static_cast<unsigned long long>(base_methods));
            if (base_methods == 0)
            {
                continue;
            }

            if ((base_methods & 1) == 0)
            {
                scan_method_list(reader, selector_base, base_methods, wanted);
                continue;
            }

            const auto lists = base_methods & ~1ULL;
            const auto header = reader.read_at_address(lists, 8);
            if (header.size() < 8)
            {
                continue;
            }

            const auto count = read_u32(header, 4);
            printf("  %s list-of-lists @ 0x%llx count=%u\n", side, static_cast<unsigned long long>(lists), count);
            const auto bytes = reader.read_at_address(lists + 8, static_cast<size_t>(count) * 8);
            for (uint32_t i = 0; i < count && (static_cast<size_t>(i) + 1) * 8 <= bytes.size(); ++i)
            {
                const auto entry = lists + 8 + static_cast<uint64_t>(i) * 8;
                const auto raw = read_u64(bytes, static_cast<size_t>(i) * 8);
                const auto image_index = raw & 0xFFFF;
                const auto target = static_cast<uint64_t>(static_cast<int64_t>(entry) + sign_extend_48(raw >> 16));
                printf("    list[%u] image=%u -> 0x%llx\n", i, static_cast<unsigned>(image_index), static_cast<unsigned long long>(target));
                scan_method_list(reader, selector_base, target, wanted);
            }
        }
    }

    TEST(ObjcIntercept, DISABLED_DecodeRelativeLists)
    {
        const auto cache = open_host_cache();
        if (!cache)
        {
            GTEST_SKIP() << "no host cache";
        }

        const sogen::macos_cache_symbols symbols{*cache};
        const auto regions = collect_slide_regions(*cache);
        const auto selector_base = objc_selector_base(*cache);
        ASSERT_TRUE(selector_base.has_value());
        printf("sharedCacheRelativeMethodBase = 0x%llx\n", static_cast<unsigned long long>(*selector_base));

        for (const auto& [cls, wanted] : {std::pair{"_OBJC_CLASS_$_CATransaction", "flush"},
                                          {"_OBJC_CLASS_$_CAContext", "remoteContext"},
                                          {"_OBJC_CLASS_$_CALayer", "setBounds:"}})
        {
            const auto address = symbols.find_export(quartzcore, cls);
            ASSERT_TRUE(address.has_value()) << cls;
            scan_class_methods(*cache, regions, *selector_base, *address, cls, wanted);
        }
    }
}

namespace
{
    struct objc_lazy_fixture
    {
        std::unique_ptr<sogen::macos_emulator> emu{};
        sogen::dyld_shared_cache_reader cache{};
        sogen::macos_cache_symbols symbols{};
        sogen::dyld_cache_pager* pager{};
    };

    // The browser's cache layout, reproduced natively: real cache contents behind the lazy pager, with the
    // chunk fixup rebasing each chunk as it materialises (the same staging install_lazy_cache_pager does
    // for the syscall path in src/macos-emulator/syscalls/dyld_support.cpp). Skipped where no host cache
    // exists, same as the native dispatch lazy fixture.
    void make_objc_lazy_fixture(objc_lazy_fixture& fixture)
    {
        auto cache = open_host_cache();
        if (!cache)
        {
            GTEST_SKIP() << "no host cache";
        }

        fixture.cache = std::move(*cache);
        fixture.symbols = sogen::macos_cache_symbols{fixture.cache};

        const auto slide_regions = collect_slide_regions(fixture.cache);
        if (slide_regions.empty())
        {
            GTEST_SKIP() << "host cache carries no slide info";
        }

        fixture.emu = macos_test::make_emulator();

        auto pager = std::make_unique<sogen::dyld_cache_pager>(fixture.emu->memory, sogen::default_host_range_reader(),
                                                               sogen::build_dyld_cache_backing_ranges(fixture.cache, 0));
        fixture.pager = pager.get();

        auto* raw = pager.get();
        auto& emu = *fixture.emu;
        raw->set_chunk_fixup([&emu, raw, slide_regions](const uint64_t chunk_address, const std::span<std::byte> chunk_data,
                                                        sogen::memory_permission) {
            const auto chunk_end = chunk_address + chunk_data.size();

            for (const auto& region : slide_regions)
            {
                if (chunk_address >= region.address + region.size || chunk_end <= region.address)
                {
                    continue;
                }

                if (!emu.memory.allocate_memory(chunk_address, chunk_data.size(), sogen::memory_permission::read_write))
                {
                    return;
                }

                emu.memory.write_memory(chunk_address, chunk_data.data(), chunk_data.size());

                uint64_t applied = 0;
                const auto rebased =
                    sogen::apply_dyld_cache_slide_info(emu, region.address, region.size, region.slide_address, applied, chunk_address,
                                                       chunk_end, [raw](const uint64_t address, const std::span<std::byte> destination) {
                                                           return raw->read_backing(address, destination);
                                                       });

                if (rebased)
                {
                    emu.memory.read_memory(chunk_address, chunk_data.data(), chunk_data.size());
                }

                (void)emu.memory.release_memory(chunk_address, chunk_data.size());
                return;
            }
        });

        sogen::install_dyld_cache_pager(emu, std::move(pager));
    }

    std::vector<uint64_t> g_objc_seen_args{};
    uint64_t g_objc_call_count = 0;

    void reset_objc_probe()
    {
        g_objc_seen_args.clear();
        g_objc_call_count = 0;
    }

    void objc_probe_handler(const sogen::macos_native_call& call)
    {
        ++g_objc_call_count;
        g_objc_seen_args = {call.arg(0), call.arg(1), call.arg(2)};
        call.ret(0xC0FFEE);
    }

    constexpr uint64_t objc_code_base = 0x100000000ULL;

    // Runs x0=self, x1=_cmd, x2=arg0, blr to the IMP, then exits with the handler's return value in x0.
    // The IMP is gigabytes above the test's code page, past any bl displacement, so the call is indirect.
    void run_call(sogen::macos_emulator& emu, const uint64_t self, const uint64_t sel, const uint64_t arg, const uint64_t imp)
    {
        std::vector<uint32_t> words{};
        macos_test::load_x(words, 0, self);
        macos_test::load_x(words, 1, sel);
        macos_test::load_x(words, 2, arg);
        macos_test::load_x(words, 8, imp);
        words.push_back(0xD63F0100); // blr x8
        words.push_back(0xD2800030); // mov x16, #1
        words.push_back(0xD4001001); // svc #0x80 (exit)

        macos_test::write_guest_code(emu, objc_code_base, words);
        emu.start();
    }

    TEST(ObjcIntercept, PatchingCATransactionFlushRoutesTheGuestCallToTheHandler)
    {
        reset_objc_probe();

        objc_lazy_fixture fixture{};
        make_objc_lazy_fixture(fixture);

        const auto class_address = fixture.symbols.find_export(quartzcore, "_OBJC_CLASS_$_CATransaction");
        ASSERT_TRUE(class_address.has_value());

        sogen::macos_native_dispatch dispatch{};
        const auto bindings = sogen::bind_objc_methods(*fixture.emu, fixture.cache, fixture.symbols, dispatch,
                                                       {{
                                                           .image = std::string{quartzcore},
                                                           .class_name = "CATransaction",
                                                           .selector = "flush",
                                                           .class_method = true,
                                                           .handler = objc_probe_handler,
                                                       }});

        ASSERT_EQ(bindings.size(), 1u);
        ASSERT_TRUE(bindings[0].bound);
        EXPECT_EQ(bindings[0].name, "+[CATransaction flush]");

        // _cmd is the selector, and a cache selector is its own name cstring.
        char sel_name[16]{};
        ASSERT_TRUE(fixture.emu->memory.try_read_memory(bindings[0].sel, sel_name, sizeof(sel_name)));
        EXPECT_STREQ(sel_name, "flush");

        fixture.emu->set_native_dispatch(&dispatch);
        run_call(*fixture.emu, *class_address, bindings[0].sel, 0, bindings[0].imp);

        EXPECT_EQ(g_objc_call_count, 1u);
        ASSERT_EQ(g_objc_seen_args.size(), 3u);
        EXPECT_EQ(g_objc_seen_args[0], *class_address);
        EXPECT_EQ(g_objc_seen_args[1], bindings[0].sel);
        EXPECT_EQ(fixture.emu->process.exit_status, 0xC0FFEE);
        EXPECT_TRUE(dispatch.handles(bindings[0].imp));
    }

    TEST(ObjcIntercept, PatchingCAContextSetLayerRoutesTheInstanceMethodCall)
    {
        reset_objc_probe();

        objc_lazy_fixture fixture{};
        make_objc_lazy_fixture(fixture);

        const auto class_address = fixture.symbols.find_export(quartzcore, "_OBJC_CLASS_$_CAContext");
        ASSERT_TRUE(class_address.has_value());

        sogen::macos_native_dispatch dispatch{};
        const auto bindings = sogen::bind_objc_methods(*fixture.emu, fixture.cache, fixture.symbols, dispatch,
                                                       {{
                                                           .image = std::string{quartzcore},
                                                           .class_name = "CAContext",
                                                           .selector = "setLayer:",
                                                           .class_method = false,
                                                           .handler = objc_probe_handler,
                                                       }});

        ASSERT_EQ(bindings.size(), 1u);
        ASSERT_TRUE(bindings[0].bound);
        EXPECT_EQ(bindings[0].name, "-[CAContext setLayer:]");

        fixture.emu->set_native_dispatch(&dispatch);
        run_call(*fixture.emu, *class_address, bindings[0].sel, 0x1A7E4, bindings[0].imp);

        EXPECT_EQ(g_objc_call_count, 1u);
        ASSERT_EQ(g_objc_seen_args.size(), 3u);
        EXPECT_EQ(g_objc_seen_args[1], bindings[0].sel);
        EXPECT_EQ(g_objc_seen_args[2], 0x1A7E4u);
        EXPECT_EQ(fixture.emu->process.exit_status, 0xC0FFEE);
    }

    TEST(ObjcIntercept, UnresolvableClassesAndSelectorsAreReportedByName)
    {
        objc_lazy_fixture fixture{};
        make_objc_lazy_fixture(fixture);

        std::string captured{};
        fixture.emu->log.set_sink([&](sogen::color, const std::string_view message) { captured.append(message); });

        sogen::macos_native_dispatch dispatch{};
        const auto bindings = sogen::bind_objc_methods(*fixture.emu, fixture.cache, fixture.symbols, dispatch,
                                                       {{
                                                            .image = std::string{quartzcore},
                                                            .class_name = "CATransaction",
                                                            .selector = "definitelyNotAMethod:",
                                                            .class_method = true,
                                                            .handler = objc_probe_handler,
                                                        },
                                                        {
                                                            .image = std::string{quartzcore},
                                                            .class_name = "CADoesNotExist",
                                                            .selector = "flush",
                                                            .class_method = true,
                                                            .handler = objc_probe_handler,
                                                        }});

        ASSERT_EQ(bindings.size(), 2u);
        EXPECT_FALSE(bindings[0].bound);
        EXPECT_FALSE(bindings[1].bound);
        EXPECT_EQ(dispatch.bound_count(), 0u);
        EXPECT_NE(captured.find("+[CATransaction definitelyNotAMethod:]"), std::string::npos) << captured;
        EXPECT_NE(captured.find("+[CADoesNotExist flush]"), std::string::npos) << captured;
    }
}
