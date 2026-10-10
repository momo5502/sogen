#include "emulation_test_utils.hpp"
#include "module/module_mapping.hpp"

namespace sogen::test
{
    namespace
    {
        // profapi.dll exports every function by ordinal only.
        constexpr auto ordinal_only_module = R"(C:\Windows\System32\profapi.dll)";

        mapped_module& map_ordinal_only_module(windows_emulator& emu)
        {
            auto* mod = emu.mod_manager.map_module(ordinal_only_module, emu.log);
            if (!mod)
            {
                throw std::runtime_error("Failed to map profapi.dll");
            }

            return *mod;
        }

        IMAGE_EXPORT_DIRECTORY read_export_directory(const memory_manager& memory, const mapped_module& mod)
        {
            const auto dos_header = memory.read_memory<PEDosHeader_t>(mod.image_base);
            const auto nt_headers = memory.read_memory<PENTHeaders_t<uint64_t>>(mod.image_base + dos_header.e_lfanew);
            const auto& entry = nt_headers.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
            return memory.read_memory<IMAGE_EXPORT_DIRECTORY>(mod.image_base + entry.VirtualAddress);
        }

        size_t count_used_export_slots(const memory_manager& memory, const mapped_module& mod, const IMAGE_EXPORT_DIRECTORY& directory)
        {
            size_t used_slots = 0;
            for (DWORD slot = 0; slot < directory.NumberOfFunctions; ++slot)
            {
                const auto rva = memory.read_memory<DWORD>(mod.image_base + directory.AddressOfFunctions + slot * sizeof(DWORD));
                if (rva != 0)
                {
                    ++used_slots;
                }
            }

            return used_slots;
        }
    }

    TEST(ModuleExportsTest, OrdinalOnlyExportsAreCollected)
    {
        auto emu = create_empty_emulator();
        const auto& mod = map_ordinal_only_module(emu);

        const auto directory = read_export_directory(emu.memory, mod);
        ASSERT_EQ(directory.NumberOfNames, 0u);
        ASSERT_GT(directory.NumberOfFunctions, 0u);

        ASSERT_EQ(mod.exports.size(), count_used_export_slots(emu.memory, mod, directory));

        for (const auto& symbol : mod.exports)
        {
            EXPECT_EQ(symbol.name, "#" + std::to_string(symbol.ordinal));
            EXPECT_GE(symbol.ordinal, directory.Base);
            EXPECT_LT(symbol.ordinal, directory.Base + directory.NumberOfFunctions);
            EXPECT_EQ(symbol.address, mod.image_base + symbol.rva);
            EXPECT_EQ(mod.find_export(symbol.name), symbol.address);
            EXPECT_TRUE(mod.address_names.contains(symbol.address));
        }
    }

    TEST(ModuleExportsTest, MemoryMappedModuleCollectsSameExportsAsFileMappedModule)
    {
        auto emu = create_empty_emulator();
        const auto& file_mapped = map_ordinal_only_module(emu);
        ASSERT_FALSE(file_mapped.exports.empty());

        const auto memory_mapped =
            map_module_from_memory<uint64_t>(emu.memory, file_mapped.image_base, file_mapped.size_of_image, file_mapped.module_path);

        ASSERT_EQ(memory_mapped.exports.size(), file_mapped.exports.size());

        for (size_t i = 0; i < file_mapped.exports.size(); ++i)
        {
            const auto& expected = file_mapped.exports[i];
            const auto& actual = memory_mapped.exports[i];
            EXPECT_EQ(actual.name, expected.name);
            EXPECT_EQ(actual.ordinal, expected.ordinal);
            EXPECT_EQ(actual.rva, expected.rva);
            EXPECT_EQ(actual.address, expected.address);
        }

        EXPECT_EQ(memory_mapped.address_names, file_mapped.address_names);
    }
} // namespace sogen::test
