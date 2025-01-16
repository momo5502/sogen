#include <cstdio>
#include "platform/platform.hpp"
#include "utils/compression.hpp"
#include "utils/io.hpp"
#include <vector>
#include <iostream>

void print_apiset(PAPI_SET_NAMESPACE apiSetMap);
void create_header_file(const std::vector<uint8_t>& data);

__forceinline PVOID GetCurrentProcessPeb()
{
#ifdef _WIN64
    return (PVOID)__readgsqword(0x60);
#else
    return (PVOID)__readfsdword(0x30);
#endif
}

int main()
{
    printf("Dump API-SET\n");
    printf("------------\n\n");

    const auto peb = (PPEB64)GetCurrentProcessPeb();
    const auto apiSetMap = (PAPI_SET_NAMESPACE)(peb->ApiSetMap);

    printf("APISET: 0x%p\n", apiSetMap);
    printf("Version: %d\n", apiSetMap->Version);
    printf("Size: %08X\n", apiSetMap->Size);
    printf("Flags: %08X\n", apiSetMap->Flags);
    printf("Count: %d\n", apiSetMap->Count);
    printf("EntryOffset: %08X\n", apiSetMap->EntryOffset);
    printf("HashOffset: %08X\n", apiSetMap->HashOffset);
    printf("HashFactor: %08X\n", apiSetMap->HashFactor);
    // print_apiset(apiSetMap);

    // Compress the API-SET binary blob
    const auto* dataPtr = reinterpret_cast<const uint8_t*>(apiSetMap);
    std::vector<uint8_t> buffer(dataPtr, dataPtr + apiSetMap->Size);
    auto compressed = utils::compression::zlib::compress(buffer);
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

void print_apiset(PAPI_SET_NAMESPACE apiSetMap)
{
    for (ULONG i = 0; i < apiSetMap->Count; i++)
    {
        auto entry = (PAPI_SET_NAMESPACE_ENTRY)((ULONG_PTR)apiSetMap + apiSetMap->EntryOffset +
                                                i * sizeof(API_SET_NAMESPACE_ENTRY));
        // printf("  Flags: %08X\n", entry->Flags);
        // printf("  NameOffset: %08X\n", entry->NameOffset);
        // printf("  NameLength: %08X\n", entry->NameLength);
        // printf("  HashedLength: %08X\n", entry->HashedLength);
        // printf("  ValueOffset: %08X\n", entry->ValueOffset);
        // printf("  ValueCount: %08X\n", entry->ValueCount);

        std::wstring name((wchar_t*)((ULONG_PTR)apiSetMap + entry->NameOffset), entry->NameLength / sizeof(wchar_t));
        printf("-----------\n[%05d]: Contract Name: %ls\n", i, name.data());

        for (ULONG x = 0; x < entry->ValueCount; x++)
        {
            auto value =
                (PAPI_SET_VALUE_ENTRY)((ULONG_PTR)apiSetMap + entry->ValueOffset + x * sizeof(API_SET_VALUE_ENTRY));
            // printf("  Value %d\n", x);
            // printf("    Flags: %08X\n", value->Flags);
            // printf("    NameOffset: %08X\n", value->NameOffset);
            // printf("    NameLength: %08X\n", value->NameLength);
            // printf("    ValueOffset: %08X\n", value->ValueOffset);
            // printf("    ValueLength: %08X\n", value->ValueLength);

            std::wstring hostName((wchar_t*)((ULONG_PTR)apiSetMap + value->NameOffset),
                                  value->NameLength / sizeof(wchar_t));
            std::wstring altName((wchar_t*)((ULONG_PTR)apiSetMap + value->ValueOffset),
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
    fopen_s(&output, "api-set.h", "w");
    if (!output)
    {
        printf("Failed to create output file\n");
        return;
    }

    fprintf(output, "#pragma once\n\n");
    fprintf(output, "#include <stdint.h>\n\n");
    fprintf(output, "const uint8_t api_set_blob[] = {\n");
    for (ULONG i = 0; i < data.size(); i++)
    {
        fprintf(output, "0x%02X, ", data[i]);
        if (i % 16 == 15)
        {
            fprintf(output, "\n");
        }
    }

    fprintf(output, "};\n");
    fclose(output);
}