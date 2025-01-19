#include <cstdio>
#include "platform/platform.hpp"
#include "utils/compression.hpp"
#include "utils/io.hpp"
#include <vector>

void print_apiset(PAPI_SET_NAMESPACE api_set_map);
void create_header_file(const std::vector<uint8_t>& data);

__forceinline PVOID GetCurrentProcessPeb()
{
#ifdef _WIN64
    return reinterpret_cast<PVOID>(__readgsqword(0x60));
#else
    return reinterpret_cast<PVOID>(__readfsdword(0x30));
#endif
}

int main()
{
    printf("Dump API-SET\n");
    printf("------------\n\n");

    const auto peb = static_cast<PPEB64>(GetCurrentProcessPeb());
    const auto api_set_map = peb->ApiSetMap;

    printf("APISET: 0x%p\n", api_set_map);
    printf("Version: %d\n", api_set_map->Version);
    printf("Size: %08X\n", api_set_map->Size);
    printf("Flags: %08X\n", api_set_map->Flags);
    printf("Count: %d\n", api_set_map->Count);
    printf("EntryOffset: %08X\n", api_set_map->EntryOffset);
    printf("HashOffset: %08X\n", api_set_map->HashOffset);
    printf("HashFactor: %08X\n", api_set_map->HashFactor);
    // print_apiset(apiSetMap);

    // Compress the API-SET binary blob
    const auto* data_ptr = reinterpret_cast<const uint8_t*>(api_set_map);
    const std::vector<uint8_t> buffer(data_ptr, data_ptr + api_set_map->Size);
    const auto compressed = utils::compression::zlib::compress(buffer);
    if (compressed.empty())
    {
        printf("Failed to compress API-SET\n");
        return 1;
    }

    // Dump the API-SET binary blob to disk
    utils::io::write_file("api-set.bin", compressed, false);
    printf("\nWrote API-SET to api-set.bin\n");
    // create_header_file(compressed);

    return 0;
}

void print_apiset(PAPI_SET_NAMESPACE api_set_map)
{
    for (ULONG i = 0; i < api_set_map->Count; i++)
    {
        const auto entry = reinterpret_cast<PAPI_SET_NAMESPACE_ENTRY>(
            reinterpret_cast<ULONG_PTR>(api_set_map) + api_set_map->EntryOffset + i * sizeof(API_SET_NAMESPACE_ENTRY));

        // printf("  Flags: %08X\n", entry->Flags);
        // printf("  NameOffset: %08X\n", entry->NameOffset);
        // printf("  NameLength: %08X\n", entry->NameLength);
        // printf("  HashedLength: %08X\n", entry->HashedLength);
        // printf("  ValueOffset: %08X\n", entry->ValueOffset);
        // printf("  ValueCount: %08X\n", entry->ValueCount);

        std::wstring name(reinterpret_cast<wchar_t*>(reinterpret_cast<ULONG_PTR>(api_set_map) + entry->NameOffset),
                          entry->NameLength / sizeof(wchar_t));
        printf("-----------\n[%05d]: Contract Name: %ls\n", i, name.data());

        for (ULONG x = 0; x < entry->ValueCount; x++)
        {
            const auto value = reinterpret_cast<PAPI_SET_VALUE_ENTRY>(
                reinterpret_cast<ULONG_PTR>(api_set_map) + entry->ValueOffset + x * sizeof(API_SET_VALUE_ENTRY));
            // printf("  Value %d\n", x);
            // printf("    Flags: %08X\n", value->Flags);
            // printf("    NameOffset: %08X\n", value->NameOffset);
            // printf("    NameLength: %08X\n", value->NameLength);
            // printf("    ValueOffset: %08X\n", value->ValueOffset);
            // printf("    ValueLength: %08X\n", value->ValueLength);

            std::wstring hostName(
                reinterpret_cast<wchar_t*>(reinterpret_cast<ULONG_PTR>(api_set_map) + value->NameOffset),
                value->NameLength / sizeof(wchar_t));
            std::wstring altName(
                reinterpret_cast<wchar_t*>(reinterpret_cast<ULONG_PTR>(api_set_map) + value->ValueOffset),
                value->ValueLength / sizeof(wchar_t));
            printf("    HostName: %ls - AltName: %ls\n", hostName.empty() ? L"<none>" : hostName.data(),
                   altName.empty() ? L"<none>" : altName.data());
        }
    }
}

// Internal
void create_header_file(const std::vector<uint8_t>& data)
{
    FILE* output;
    (void)fopen_s(&output, "api-set.h", "w");
    if (!output)
    {
        printf("Failed to create output file\n");
        return;
    }

    (void)fprintf(output, "#pragma once\n\n");
    (void)fprintf(output, "#include <stdint.h>\n\n");
    (void)fprintf(output, "const uint8_t api_set_blob[] = {\n");
    for (ULONG i = 0; i < data.size(); i++)
    {
        (void)fprintf(output, "0x%02X, ", data[i]);
        if (i % 16 == 15)
        {
            (void)fprintf(output, "\n");
        }
    }

    (void)fprintf(output, "};\n");
    (void)fclose(output);
}
