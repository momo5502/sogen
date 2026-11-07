#include "emulation_test_utils.hpp"

namespace sogen::test
{
    namespace
    {
        bool has_sub_key(registry_manager& registry, const registry_key& key, const std::string_view name)
        {
            for (size_t index = 0; index < registry.get_sub_key_count(key); ++index)
            {
                if (const auto sub_key = registry.get_sub_key_name(key, index);
                    sub_key && utils::string::equals_ignore_case(*sub_key, name))
                {
                    return true;
                }
            }

            return false;
        }
    }

    // An open key keeps its full path and hive node once looked up, so sub keys created or deleted afterwards must
    // still show through it.
    TEST(RegistryKeyTest, OpenKeySeesSubKeysCreatedAfterItWasRead)
    {
        auto emu = create_empty_emulator();
        auto& registry = emu.registry;
        const auto control = registry.get_key({u"\\Registry\\Machine\\System\\CurrentControlSet\\Control"});
        ASSERT_TRUE(control.has_value());
        const auto count = registry.get_sub_key_count(*control);
        ASSERT_GT(count, 0u);
        EXPECT_FALSE(has_sub_key(registry, *control, "SogenKeyProbe"));

        ASSERT_TRUE(registry.create_key(u"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\SogenKeyProbe").has_value());
        EXPECT_EQ(count + 1, registry.get_sub_key_count(*control));
        EXPECT_TRUE(has_sub_key(registry, *control, "SogenKeyProbe"));
    }

    TEST(RegistryKeyTest, OpenKeyReportsItsDeletion)
    {
        auto emu = create_empty_emulator();
        auto& registry = emu.registry;
        const auto probe = registry.create_key(u"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\SogenKeyProbe");
        ASSERT_TRUE(probe.has_value());
        ASSERT_TRUE(registry.key_exists(*probe));

        registry.delete_key(*probe);
        EXPECT_FALSE(registry.key_exists(*probe));

        const auto control = registry.get_key({u"\\Registry\\Machine\\System\\CurrentControlSet\\Control"});
        ASSERT_TRUE(control.has_value());
        EXPECT_FALSE(has_sub_key(registry, *control, "SogenKeyProbe"));
    }
}
