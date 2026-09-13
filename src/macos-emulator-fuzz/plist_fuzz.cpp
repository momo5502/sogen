#include "fuzz_target.hpp"

#include <plist.hpp>

#include <array>
#include <cstddef>
#include <string_view>

namespace sogen::fuzz
{
    namespace
    {
        // A generator that only produces random bytes almost never reaches the value readers: the
        // binary format needs a plausible trailer, and the XML format needs a matching key. Splitting
        // the first byte off as a key selector keeps both paths reachable while leaving the rest of
        // the buffer fully attacker-shaped.
        constexpr std::array<std::string_view, 4> KEYS{
            "CFBundleExecutable",
            "CFBundleIdentifier",
            "CFBundleName",
            "",
        };
    }

    void run(const std::span<const uint8_t> data)
    {
        if (data.empty())
        {
            return;
        }

        const auto key = KEYS.at(data.front() % KEYS.size());
        const auto payload = data.subspan(1);

        (void)plist_top_level_string({reinterpret_cast<const std::byte*>(payload.data()), payload.size()}, key);
    }
}
