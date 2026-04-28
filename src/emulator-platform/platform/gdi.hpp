#pragma once
namespace sogen
{

    struct GDI_HANDLE_ENTRY64
    {
        union
        {
            EmulatorTraits<Emu64>::PVOID Object;
            EmulatorTraits<Emu64>::PVOID NextFree;
        };

        union
        {
            struct
            {
                USHORT ProcessId;
                USHORT Lock : 1;
                USHORT Count : 15;
            };

            ULONG Value;
        } Owner;

        USHORT Unique;
        UCHAR Type;
        UCHAR Flags;
        EmulatorTraits<Emu64>::PVOID UserPointer;
    };

    struct GDI_HANDLE_ENTRY32
    {
        uint32_t Object;
        uint32_t OwnerValue;
        USHORT Unique;
        UCHAR Type;
        UCHAR Flags;
        uint32_t UserPointer;
    };
    static_assert(sizeof(GDI_HANDLE_ENTRY32) == 0x10);

    #define GDI_MAX_HANDLE_COUNT 0xFFFF // 0x4000

    struct GDI_SHARED_MEMORY64
    {
        GDI_HANDLE_ENTRY64 Handles[GDI_MAX_HANDLE_COUNT];
        char pad[0xC8];
        uint64_t Objects[0x20];
        uint64_t Data[0x200]; // ?
    };
    static_assert(offsetof(GDI_SHARED_MEMORY64, Objects) == 0x1800B0);

    struct EMU_D3DKMT_ADAPTERINFO
    {
        UINT32 hAdapter;
        LUID AdapterLuid;
        UINT32 NumOfSources;
        BOOL bPrecisePresentRegionsPreferred;
    };

    struct EMU_D3DKMT_ENUMADAPTERS2
    {
        UINT32 NumAdapters;
        UINT64 pAdapters;
    };

    struct EMU_D3DKMT_ENUMADAPTERS_FILTER
    {
        UINT64 Value;
    };

    struct EMU_D3DKMT_ENUMADAPTERS3
    {
        EMU_D3DKMT_ENUMADAPTERS_FILTER Filter;
        UINT32 NumAdapters;
        UINT32 Padding;
        UINT64 pAdapters;
    };
    static_assert(sizeof(EMU_D3DKMT_ENUMADAPTERS3) == 0x18);

    struct EMU_D3DKMT_GET_PROPERTIES
    {
        UINT32 PropertyId;
        UINT32 Size;
        UINT64 Reserved;
        UINT64 pBuffer;
        UINT64 Reserved2[2];
    };

    enum class KMTQAITYPE : UINT32
    {
        KMTQAITYPE_UMDRIVERPRIVATE = 0,
        KMTQAITYPE_UMDRIVERNAME = 1,
        KMTQAITYPE_UMOPENGLINFO = 2,
        KMTQAITYPE_GETSEGMENTSIZE = 3,
        KMTQAITYPE_ADAPTERGUID = 4,
        KMTQAITYPE_FLIPQUEUEINFO = 5,
        KMTQAITYPE_ADAPTERADDRESS = 6,
        KMTQAITYPE_SETWORKINGSETINFO = 7,
        KMTQAITYPE_ADAPTERREGISTRYINFO = 8,
        KMTQAITYPE_CURRENTDISPLAYMODE = 9,
        KMTQAITYPE_MODELIST = 10,
        KMTQAITYPE_CHECKDRIVERUPDATESTATUS = 11,
        KMTQAITYPE_VIRTUALADDRESSINFO = 12,
        KMTQAITYPE_DRIVERVERSION = 13,
        KMTQAITYPE_ADAPTERTYPE = 15,
        KMTQAITYPE_OUTPUTDUPLCONTEXTSCOUNT = 16,
        KMTQAITYPE_WDDM_1_2_CAPS = 17,
        KMTQAITYPE_UMD_DRIVER_VERSION = 18,
        KMTQAITYPE_DIRECTFLIP_SUPPORT = 19,
        KMTQAITYPE_MULTIPLANEOVERLAY_SUPPORT = 20,
        KMTQAITYPE_DLIST_DRIVER_NAME = 21,
        KMTQAITYPE_WDDM_1_3_CAPS = 22,
        KMTQAITYPE_MULTIPLANEOVERLAY_HUD_SUPPORT = 23,
        KMTQAITYPE_WDDM_2_0_CAPS = 24,
        KMTQAITYPE_NODEMETADATA = 25,
        KMTQAITYPE_CPDRIVERNAME = 26,
        KMTQAITYPE_XBOX = 27,
        KMTQAITYPE_INDEPENDENTFLIP_SUPPORT = 28,
        KMTQAITYPE_MIRACASTCOMPANIONDRIVERNAME = 29,
        KMTQAITYPE_PHYSICALADAPTERCOUNT = 30,
        KMTQAITYPE_PHYSICALADAPTERDEVICEIDS = 31,
        KMTQAITYPE_DRIVERCAPS_EXT = 32,
        KMTQAITYPE_QUERY_MIRACAST_DRIVER_TYPE = 33,
        KMTQAITYPE_QUERY_GPUMMU_CAPS = 34,
        KMTQAITYPE_QUERY_MULTIPLANEOVERLAY_DECODE_SUPPORT = 35,
        KMTQAITYPE_QUERY_HW_PROTECTION_TEARDOWN_COUNT = 36,
        KMTQAITYPE_QUERY_ISBADDRIVERFORHWPROTECTIONDISABLED = 37,
        KMTQAITYPE_MULTIPLANEOVERLAY_SECONDARY_SUPPORT = 38,
        KMTQAITYPE_INDEPENDENTFLIP_SECONDARY_SUPPORT = 39,
        KMTQAITYPE_PANELFITTER_SUPPORT = 40,
        KMTQAITYPE_PHYSICALADAPTERPNPKEY = 41,
        KMTQAITYPE_GETSEGMENTGROUPSIZE = 42,
        KMTQAITYPE_MPO3DDI_SUPPORT = 43,
        KMTQAITYPE_HWDRM_SUPPORT = 44,
        KMTQAITYPE_MPOKERNELCAPS_SUPPORT = 45,
        KMTQAITYPE_MULTIPLANEOVERLAY_STRETCH_SUPPORT = 46,
        KMTQAITYPE_GET_DEVICE_VIDPN_OWNERSHIP_INFO = 47,
        KMTQAITYPE_QUERYREGISTRY = 48,
        KMTQAITYPE_KMD_DRIVER_VERSION = 49,
        KMTQAITYPE_BLOCKLIST_KERNEL = 50,
        KMTQAITYPE_BLOCKLIST_RUNTIME = 51,
        KMTQAITYPE_ADAPTERGUID_RENDER = 52,
        KMTQAITYPE_ADAPTERADDRESS_RENDER = 53,
        KMTQAITYPE_ADAPTERREGISTRYINFO_RENDER = 54,
        KMTQAITYPE_CHECKDRIVERUPDATESTATUS_RENDER = 55,
        KMTQAITYPE_DRIVERVERSION_RENDER = 56,
        KMTQAITYPE_ADAPTERTYPE_RENDER = 57,
        KMTQAITYPE_WDDM_1_2_CAPS_RENDER = 58,
        KMTQAITYPE_WDDM_1_3_CAPS_RENDER = 59,
        KMTQAITYPE_QUERY_ADAPTER_UNIQUE_GUID = 60,
        KMTQAITYPE_NODEPERFDATA = 61,
        KMTQAITYPE_ADAPTERPERFDATA = 62,
        KMTQAITYPE_ADAPTERPERFDATA_CAPS = 63,
        KMTQUITYPE_GPUVERSION = 64,
        KMTQAITYPE_DRIVER_DESCRIPTION = 65,
        KMTQAITYPE_DRIVER_DESCRIPTION_RENDER = 66,
        KMTQAITYPE_SCANOUT_CAPS = 67,
        KMTQAITYPE_PARAVIRTUALIZATION_RENDER = 68,
        KMTQAITYPE_SERVICENAME = 69,
        KMTQAITYPE_WDDM_2_7_CAPS = 70,
        KMTQAITYPE_DISPLAY_UMDRIVERNAME = 71,
        KMTQAITYPE_TRACKEDWORKLOAD_SUPPORT = 72,
        KMTQAITYPE_HYBRID_DLIST_DLL_SUPPORT = 73,
        KMTQAITYPE_DISPLAY_CAPS = 74,
        KMTQAITYPE_WDDM_2_9_CAPS = 75,
        KMTQAITYPE_CROSSADAPTERRESOURCE_SUPPORT = 76,
        KMTQAITYPE_WDDM_3_0_CAPS = 77,
    };

    struct EMU_D3DKMT_QUERYADAPTERINFO
    {
        UINT32 hAdapter;
        KMTQAITYPE Type;
        UINT64 pPrivateDriverData;
        UINT32 PrivateDriverDataSize;
    };

    struct EMU_D3DKMT_CREATEDEVICE
    {
        UINT64 hAdapter;
        UINT32 Flags;
        UINT32 hDevice;
        UINT64 pCommandBuffer;
        UINT32 CommandBufferSize;
        UINT64 pAllocationList;
        UINT32 AllocationListSize;
        UINT64 pPatchLocationList;
        UINT32 PatchLocationListSize;
    };

    struct EMU_D3DKMT_ESCAPE
    {
        UINT32 hAdapter;
        UINT32 hDevice;
        UINT32 Type;
        UINT32 Flags;
        UINT64 pPrivateDriverData;
        UINT32 PrivateDriverDataSize;
        UINT32 hContext;
    };

    struct EMU_D3DKMT_CREATECONTEXT
    {
        UINT32 hDevice;
        UINT32 NodeOrdinal;
        UINT32 EngineAffinity;
        UINT32 Flags;
        UINT64 pPrivateDriverData;
        UINT32 PrivateDriverDataSize;
        UINT32 ClientHint;
        UINT32 hContext;
        UINT64 pCommandBuffer;
        UINT32 CommandBufferSize;
        UINT64 pAllocationList;
        UINT32 AllocationListSize;
        UINT64 pPatchLocationList;
        UINT32 PatchLocationListSize;
        UINT64 CommandBuffer;
    };

    struct EMU_D3DDDI_ALLOCATIONINFO
    {
        UINT32 hAllocation;
        UINT64 pSystemMem;
        UINT64 pPrivateDriverData;
        UINT32 PrivateDriverDataSize;
        UINT32 VidPnSourceId;
        UINT32 Flags;
    };

    struct EMU_D3DKMT_CREATEALLOCATION
    {
        UINT32 hDevice;
        UINT32 hResource;
        UINT32 hGlobalShare;
        UINT64 pPrivateRuntimeData;
        UINT32 PrivateRuntimeDataSize;
        UINT64 pPrivateDriverData;
        UINT32 PrivateDriverDataSize;
        UINT32 NumAllocations;
        UINT64 pAllocationInfo;
        UINT32 Flags;
        UINT64 hPrivateRuntimeResourceHandle;
    };

    struct EMU_D3DKMT_LOCK
    {
        UINT32 hDevice;
        UINT32 hAllocation;
        UINT32 PrivateDriverData;
        UINT32 NumPages;
        UINT64 pPages;
        UINT64 pData;
        UINT32 Flags;
        UINT64 GpuVirtualAddress;
    };

    struct EMU_D3DKMT_OPENADAPTERFROMHDC
    {
        HDC hDc;
        UINT32 hAdapter;
        LUID AdapterLuid;
        UINT VidPnSourceId;
    };

    // NOLINTEND(modernize-use-using,cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays,cppcoreguidelines-use-enum-class)
} // namespace sogen
