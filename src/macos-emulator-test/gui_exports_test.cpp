#include <gtest/gtest.h>

#include <gui/macos_gui_exports.hpp>
#include <module/dyld_shared_cache.hpp>
#include <module/macos_cache_symbols.hpp>
#include <macos_platform.hpp>

#include <cstdlib>
#include <regex>
#include <set>
#include <string>

namespace
{
    std::optional<sogen::dyld_shared_cache_reader> host_cache()
    {
        const std::filesystem::path path{MACOS_DYLD_CACHE_HOST_PATH};
        if (!std::filesystem::exists(path))
        {
            return std::nullopt;
        }

        return sogen::dyld_shared_cache_reader::parse(path, [](const std::filesystem::path& p) { return sogen::open_dyld_cache_file(p); });
    }

    TEST(GuiExports, TheFirstPixelTableIsWellFormed)
    {
        const auto exports = sogen::macos_gui_first_pixel_exports();
        EXPECT_GE(exports.size(), 21u);

        std::set<std::string> seen{};

        for (const auto& entry : exports)
        {
            EXPECT_FALSE(entry.image.empty());
            ASSERT_FALSE(entry.symbol.empty());
            EXPECT_EQ(entry.symbol.front(), '_') << entry.symbol << " is not a linker symbol";
            EXPECT_TRUE(seen.insert(std::string{entry.image} + "|" + std::string{entry.symbol}).second)
                << "duplicate entry " << entry.symbol;
        }
    }

    // The table is a measurement, so it is checked against the thing it measured. A symbol that stops
    // resolving is this release having renamed it, which is exactly the drift the table exists to record.
    TEST(GuiExports, EveryEntryResolvesInTheHostCache)
    {
        const auto cache = host_cache();
        if (!cache)
        {
            GTEST_SKIP() << "no shared cache on this host";
        }

        const sogen::macos_cache_symbols symbols{*cache};

        for (const auto& entry : sogen::macos_gui_first_pixel_exports())
        {
            const auto address = symbols.find_export(entry.image, entry.symbol);
            ASSERT_TRUE(address.has_value()) << entry.symbol << " is not exported by " << entry.image;
            EXPECT_EQ(*address % 4, 0u) << entry.symbol << " resolved to an unaligned address";
        }
    }

    // CoreGraphics re-exports this from SkyLight, so its trie payload is a library ordinal and not an
    // image offset. Reading it as an address gives 0x1872a8001 on build 25G76 -- unaligned, and shared
    // with _CGWindowContextCreateImage, which is how the re-export was spotted. find_export has to refuse
    // it rather than hand back a number that points at nothing.
    TEST(GuiExports, ReexportedSymbolsDoNotResolveToAnAddress)
    {
        const auto cache = host_cache();
        if (!cache)
        {
            GTEST_SKIP() << "no shared cache on this host";
        }

        const sogen::macos_cache_symbols symbols{*cache};

        EXPECT_FALSE(symbols.find_export(sogen::MACOS_CORE_GRAPHICS_IMAGE_PATH, "_CGWindowContextCreate").has_value());
        EXPECT_TRUE(symbols.find_export(sogen::MACOS_SKYLIGHT_IMAGE_PATH, "_SLWindowContextCreate").has_value())
            << "the implementation behind the re-export is the one to intercept";

        bool saw_reexport = false;
        for (const auto& entry : symbols.exports_of(sogen::MACOS_CORE_GRAPHICS_IMAGE_PATH))
        {
            if (entry.name == "_CGWindowContextCreate")
            {
                saw_reexport = true;
                EXPECT_FALSE(entry.has_address());
                EXPECT_EQ(entry.flags & sogen::macho_export_flag::REEXPORT, sogen::macho_export_flag::REEXPORT);
            }
        }

        EXPECT_TRUE(saw_reexport) << "the symbol is still in the trie, it just has no address";
    }
}

namespace
{
    // The regenerator for macos_gui_exports.cpp, and the reason that table needs no external cache
    // extractor: it reads the same tries the emulator reads. Disabled because it is a measurement rather
    // than an assertion.
    //
    //   PROBE_IMAGE=LIST PROBE_PATTERN=SkyLight \
    //     ./macos-emulator-test --gtest_also_run_disabled_tests --gtest_filter='GuiExports.DISABLED_*'
    //   PROBE_IMAGE=/System/.../SkyLight PROBE_PATTERN='^_SLS' ...
    TEST(GuiExports, DISABLED_DumpImageExports)
    {
        const auto cache = host_cache();
        if (!cache)
        {
            GTEST_SKIP() << "no shared cache on this host";
        }

        const char* image = std::getenv("PROBE_IMAGE");
        const char* pattern = std::getenv("PROBE_PATTERN");
        ASSERT_NE(image, nullptr) << "set PROBE_IMAGE to an install name, or to LIST";

        const std::regex expression{pattern ? pattern : "."};

        if (std::string_view{image} == "LIST")
        {
            for (const auto& entry : cache->images())
            {
                if (std::regex_search(entry.path, expression))
                {
                    printf("%s\n", entry.path.c_str());
                }
            }

            return;
        }

        const sogen::macos_cache_symbols symbols{*cache};
        for (const auto& entry : symbols.exports_of(image))
        {
            if (std::regex_search(entry.name, expression))
            {
                printf("%-56s 0x%012llx flags=0x%llx%s\n", entry.name.c_str(), static_cast<unsigned long long>(entry.address),
                       static_cast<unsigned long long>(entry.flags), entry.has_address() ? "" : "  (no address)");
            }
        }
    }
}
