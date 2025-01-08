#pragma once

typedef enum _KEY_INFORMATION_CLASS
{
    KeyBasicInformation,          // KEY_BASIC_INFORMATION
    KeyNodeInformation,           // KEY_NODE_INFORMATION
    KeyFullInformation,           // KEY_FULL_INFORMATION
    KeyNameInformation,           // KEY_NAME_INFORMATION
    KeyCachedInformation,         // KEY_CACHED_INFORMATION
    KeyFlagsInformation,          // KEY_FLAGS_INFORMATION
    KeyVirtualizationInformation, // KEY_VIRTUALIZATION_INFORMATION
    KeyHandleTagsInformation,     // KEY_HANDLE_TAGS_INFORMATION
    KeyTrustInformation,          // KEY_TRUST_INFORMATION
    KeyLayerInformation,          // KEY_LAYER_INFORMATION
    MaxKeyInfoClass
} KEY_INFORMATION_CLASS;

typedef enum _KEY_VALUE_INFORMATION_CLASS
{
    KeyValueBasicInformation,   // KEY_VALUE_BASIC_INFORMATION
    KeyValueFullInformation,    // KEY_VALUE_FULL_INFORMATION
    KeyValuePartialInformation, // KEY_VALUE_PARTIAL_INFORMATION
    KeyValueFullInformationAlign64,
    KeyValuePartialInformationAlign64, // KEY_VALUE_PARTIAL_INFORMATION_ALIGN64
    KeyValueLayerInformation,          // KEY_VALUE_LAYER_INFORMATION
    MaxKeyValueInfoClass
} KEY_VALUE_INFORMATION_CLASS;

struct KEY_NAME_INFORMATION
{
    std::uint32_t NameLength;
    char16_t Name[1];
};

typedef struct _KEY_FULL_INFORMATION
{
    LARGE_INTEGER LastWriteTime;
    ULONG TitleIndex;
    ULONG ClassOffset;
    ULONG ClassLength;
    ULONG SubKeys;
    ULONG MaxNameLength;
    ULONG MaxClassLength;
    ULONG Values;
    ULONG MaxValueNameLength;
    ULONG MaxValueDataLength;
    char16_t Class[1];
} KEY_FULL_INFORMATION, *PKEY_FULL_INFORMATION;

struct KEY_HANDLE_TAGS_INFORMATION
{
    ULONG HandleTags;
};

struct KEY_VALUE_BASIC_INFORMATION
{
    ULONG TitleIndex;
    ULONG Type;
    ULONG NameLength;
    char16_t Name[1];
};

struct KEY_VALUE_PARTIAL_INFORMATION
{
    ULONG TitleIndex;
    ULONG Type;
    ULONG DataLength;
    UCHAR Data[1];
};

struct KEY_VALUE_FULL_INFORMATION
{
    ULONG TitleIndex;
    ULONG Type;
    ULONG DataOffset;
    ULONG DataLength;
    ULONG NameLength;
    char16_t Name[1];
};
