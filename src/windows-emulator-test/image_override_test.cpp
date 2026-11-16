#include "emulation_test_utils.hpp"

namespace sogen::test
{
    TEST(ImageOverrideTest, MapsTheReplacementUnderTheRequestedPath)
    {
        auto emu = create_empty_emulator();
        const windows_path image{R"(C:\Windows\System32\profapi.dll)"};
        const auto* replacement = emu.mod_manager.map_module(R"(C:\test-sample.exe)", emu.log);
        ASSERT_NE(replacement, nullptr);

        emu.mod_manager.set_image_overrides({{.image = image, .replacement = get_emulator_root() / "filesys" / "c" / "test-sample.exe"}});
        const auto* mod = emu.mod_manager.map_module(image, emu.log);

        ASSERT_NE(mod, nullptr);
        EXPECT_EQ(mod->path, module_manager::module_identity(image));
        EXPECT_EQ(mod->size_of_image, replacement->size_of_image);
        EXPECT_EQ(mod->entry_point - mod->image_base, replacement->entry_point - replacement->image_base);
    }
}
