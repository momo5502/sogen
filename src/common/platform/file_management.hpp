#pragma once

#define ACCESS_MASK              DWORD
#define DEVICE_TYPE              DWORD

#define FILE_DEVICE_DISK         0x00000007
#define FILE_DEVICE_CONSOLE      0x00000050

#define FILE_SUPERSEDE           0x00000000
#define FILE_OPEN                0x00000001
#define FILE_CREATE              0x00000002
#define FILE_OPEN_IF             0x00000003
#define FILE_OVERWRITE           0x00000004
#define FILE_OVERWRITE_IF        0x00000005
#define FILE_MAXIMUM_DISPOSITION 0x00000005

#ifndef OS_WINDOWS
#define GENERIC_READ    0x80000000
#define GENERIC_WRITE   0x40000000
#define GENERIC_EXECUTE 0x20000000
#define GENERIC_ALL     0x10000000

#undef DELETE
#define DELETE                    0x00010000
#define READ_CONTROL              0x00020000
#define WRITE_DAC                 0x00040000
#define WRITE_OWNER               0x00080000
#define SYNCHRONIZE               0x00100000
#define STANDARD_RIGHTS_REQUIRED  0x000f0000

#define FILE_READ_DATA            0x0001 /* file & pipe */
#define FILE_LIST_DIRECTORY       0x0001 /* directory */
#define FILE_WRITE_DATA           0x0002 /* file & pipe */
#define FILE_ADD_FILE             0x0002 /* directory */
#define FILE_APPEND_DATA          0x0004 /* file */
#define FILE_ADD_SUBDIRECTORY     0x0004 /* directory */
#define FILE_CREATE_PIPE_INSTANCE 0x0004 /* named pipe */
#define FILE_READ_EA              0x0008 /* file & directory */
#define FILE_READ_PROPERTIES      FILE_READ_EA
#define FILE_WRITE_EA             0x0010 /* file & directory */
#define FILE_WRITE_PROPERTIES     FILE_WRITE_EA
#define FILE_EXECUTE              0x0020 /* file */
#define FILE_TRAVERSE             0x0020 /* directory */
#define FILE_DELETE_CHILD         0x0040 /* directory */
#define FILE_READ_ATTRIBUTES      0x0080 /* all */
#define FILE_WRITE_ATTRIBUTES     0x0100 /* all */
#define FILE_ALL_ACCESS           (STANDARD_RIGHTS_REQUIRED | SYNCHRONIZE | 0x1ff)

#endif

#define FILE_DIRECTORY_FILE            0x00000001
#define FILE_WRITE_THROUGH             0x00000002
#define FILE_SEQUENTIAL_ONLY           0x00000004
#define FILE_NO_INTERMEDIATE_BUFFERING 0x00000008

#define FILE_SYNCHRONOUS_IO_ALERT      0x00000010
#define FILE_SYNCHRONOUS_IO_NONALERT   0x00000020
#define FILE_NON_DIRECTORY_FILE        0x00000040
#define FILE_CREATE_TREE_CONNECTION    0x00000080

#define FILE_ATTRIBUTE_NORMAL          0x00000080

#define PS_ATTRIBUTE_NUMBER_MASK       0x0000ffff
#define PS_ATTRIBUTE_THREAD            0x00010000 // may be used with thread creation
#define PS_ATTRIBUTE_INPUT             0x00020000 // input only
#define PS_ATTRIBUTE_ADDITIVE          0x00040000 // "accumulated" e.g. bitmasks, counters, etc.

#define SL_RESTART_SCAN                0x01
#define SL_RETURN_SINGLE_ENTRY         0x02
#define SL_NO_CURSOR_UPDATE            0x10

#define SEC_IMAGE                      0x01000000

typedef enum _FSINFOCLASS
{
    FileFsVolumeInformation = 1, // q: FILE_FS_VOLUME_INFORMATION
    FileFsLabelInformation,      // s: FILE_FS_LABEL_INFORMATION (requires FILE_WRITE_DATA to volume)
    FileFsSizeInformation,       // q: FILE_FS_SIZE_INFORMATION
    FileFsDeviceInformation,     // q: FILE_FS_DEVICE_INFORMATION
    FileFsAttributeInformation,  // q: FILE_FS_ATTRIBUTE_INFORMATION
    FileFsControlInformation,
    // q, s: FILE_FS_CONTROL_INFORMATION  (q: requires FILE_READ_DATA; s: requires FILE_WRITE_DATA to volume)
    FileFsFullSizeInformation,   // q: FILE_FS_FULL_SIZE_INFORMATION
    FileFsObjectIdInformation,   // q; s: FILE_FS_OBJECTID_INFORMATION (s: requires FILE_WRITE_DATA to volume)
    FileFsDriverPathInformation, // q: FILE_FS_DRIVER_PATH_INFORMATION
    FileFsVolumeFlagsInformation,
    // q; s: FILE_FS_VOLUME_FLAGS_INFORMATION (q: requires FILE_READ_ATTRIBUTES; s: requires FILE_WRITE_ATTRIBUTES to
    // volume) // 10
    FileFsSectorSizeInformation,   // q: FILE_FS_SECTOR_SIZE_INFORMATION // since WIN8
    FileFsDataCopyInformation,     // q: FILE_FS_DATA_COPY_INFORMATION
    FileFsMetadataSizeInformation, // q: FILE_FS_METADATA_SIZE_INFORMATION // since THRESHOLD
    FileFsFullSizeInformationEx,   // q: FILE_FS_FULL_SIZE_INFORMATION_EX // since REDSTONE5
    FileFsGuidInformation,         // q: FILE_FS_GUID_INFORMATION // since 23H2
    FileFsMaximumInformation
} FSINFOCLASS, *PFSINFOCLASS;

typedef enum _FSINFOCLASS FS_INFORMATION_CLASS;

typedef enum _FILE_INFORMATION_CLASS
{
    FileDirectoryInformation = 1,
    // q: FILE_DIRECTORY_INFORMATION (requires FILE_LIST_DIRECTORY) (NtQueryDirectoryFile[Ex])
    FileFullDirectoryInformation,
    // q: FILE_FULL_DIR_INFORMATION (requires FILE_LIST_DIRECTORY) (NtQueryDirectoryFile[Ex])
    FileBothDirectoryInformation,
    // q: FILE_BOTH_DIR_INFORMATION (requires FILE_LIST_DIRECTORY) (NtQueryDirectoryFile[Ex])
    FileBasicInformation,
    // q; s: FILE_BASIC_INFORMATION (q: requires FILE_READ_ATTRIBUTES; s: requires FILE_WRITE_ATTRIBUTES)
    FileStandardInformation,      // q: FILE_STANDARD_INFORMATION, FILE_STANDARD_INFORMATION_EX
    FileInternalInformation,      // q: FILE_INTERNAL_INFORMATION
    FileEaInformation,            // q: FILE_EA_INFORMATION
    FileAccessInformation,        // q: FILE_ACCESS_INFORMATION
    FileNameInformation,          // q: FILE_NAME_INFORMATION
    FileRenameInformation,        // s: FILE_RENAME_INFORMATION (requires DELETE) // 10
    FileLinkInformation,          // s: FILE_LINK_INFORMATION
    FileNamesInformation,         // q: FILE_NAMES_INFORMATION (requires FILE_LIST_DIRECTORY) (NtQueryDirectoryFile[Ex])
    FileDispositionInformation,   // s: FILE_DISPOSITION_INFORMATION (requires DELETE)
    FilePositionInformation,      // q; s: FILE_POSITION_INFORMATION
    FileFullEaInformation,        // FILE_FULL_EA_INFORMATION
    FileModeInformation,          // q; s: FILE_MODE_INFORMATION
    FileAlignmentInformation,     // q: FILE_ALIGNMENT_INFORMATION
    FileAllInformation,           // q: FILE_ALL_INFORMATION (requires FILE_READ_ATTRIBUTES)
    FileAllocationInformation,    // s: FILE_ALLOCATION_INFORMATION (requires FILE_WRITE_DATA)
    FileEndOfFileInformation,     // s: FILE_END_OF_FILE_INFORMATION (requires FILE_WRITE_DATA) // 20
    FileAlternateNameInformation, // q: FILE_NAME_INFORMATION
    FileStreamInformation,        // q: FILE_STREAM_INFORMATION
    FilePipeInformation,
    // q; s: FILE_PIPE_INFORMATION (q: requires FILE_READ_ATTRIBUTES; s: requires FILE_WRITE_ATTRIBUTES)
    FilePipeLocalInformation, // q: FILE_PIPE_LOCAL_INFORMATION (requires FILE_READ_ATTRIBUTES)
    FilePipeRemoteInformation,
    // q; s: FILE_PIPE_REMOTE_INFORMATION (q: requires FILE_READ_ATTRIBUTES; s: requires FILE_WRITE_ATTRIBUTES)
    FileMailslotQueryInformation, // q: FILE_MAILSLOT_QUERY_INFORMATION
    FileMailslotSetInformation,   // s: FILE_MAILSLOT_SET_INFORMATION
    FileCompressionInformation,   // q: FILE_COMPRESSION_INFORMATION
    FileObjectIdInformation,   // q: FILE_OBJECTID_INFORMATION (requires FILE_LIST_DIRECTORY) (NtQueryDirectoryFile[Ex])
    FileCompletionInformation, // s: FILE_COMPLETION_INFORMATION // 30
    FileMoveClusterInformation, // s: FILE_MOVE_CLUSTER_INFORMATION (requires FILE_WRITE_DATA)
    FileQuotaInformation,       // q: FILE_QUOTA_INFORMATION (requires FILE_LIST_DIRECTORY) (NtQueryDirectoryFile[Ex])
    FileReparsePointInformation,
    // q: FILE_REPARSE_POINT_INFORMATION (requires FILE_LIST_DIRECTORY) (NtQueryDirectoryFile[Ex])
    FileNetworkOpenInformation,  // q: FILE_NETWORK_OPEN_INFORMATION (requires FILE_READ_ATTRIBUTES)
    FileAttributeTagInformation, // q: FILE_ATTRIBUTE_TAG_INFORMATION (requires FILE_READ_ATTRIBUTES)
    FileTrackingInformation,     // s: FILE_TRACKING_INFORMATION (requires FILE_WRITE_DATA)
    FileIdBothDirectoryInformation,
    // q: FILE_ID_BOTH_DIR_INFORMATION (requires FILE_LIST_DIRECTORY) (NtQueryDirectoryFile[Ex])
    FileIdFullDirectoryInformation,
    // q: FILE_ID_FULL_DIR_INFORMATION (requires FILE_LIST_DIRECTORY) (NtQueryDirectoryFile[Ex])
    FileValidDataLengthInformation,
    // s: FILE_VALID_DATA_LENGTH_INFORMATION (requires FILE_WRITE_DATA and/or SeManageVolumePrivilege)
    FileShortNameInformation, // s: FILE_NAME_INFORMATION (requires DELETE) // 40
    FileIoCompletionNotificationInformation,
    // q; s: FILE_IO_COMPLETION_NOTIFICATION_INFORMATION (q: requires FILE_READ_ATTRIBUTES) // since VISTA
    FileIoStatusBlockRangeInformation, // s: FILE_IOSTATUSBLOCK_RANGE_INFORMATION (requires SeLockMemoryPrivilege)
    FileIoPriorityHintInformation,
    // q; s: FILE_IO_PRIORITY_HINT_INFORMATION, FILE_IO_PRIORITY_HINT_INFORMATION_EX (q: requires FILE_READ_DATA)
    FileSfioReserveInformation,         // q; s: FILE_SFIO_RESERVE_INFORMATION (q: requires FILE_READ_DATA)
    FileSfioVolumeInformation,          // q: FILE_SFIO_VOLUME_INFORMATION (requires FILE_READ_ATTRIBUTES)
    FileHardLinkInformation,            // q: FILE_LINKS_INFORMATION
    FileProcessIdsUsingFileInformation, // q: FILE_PROCESS_IDS_USING_FILE_INFORMATION (requires FILE_READ_ATTRIBUTES)
    FileNormalizedNameInformation,      // q: FILE_NAME_INFORMATION
    FileNetworkPhysicalNameInformation, // q: FILE_NETWORK_PHYSICAL_NAME_INFORMATION
    FileIdGlobalTxDirectoryInformation,
    // q: FILE_ID_GLOBAL_TX_DIR_INFORMATION (requires FILE_LIST_DIRECTORY) (NtQueryDirectoryFile[Ex]) // since WIN7 //
    // 50
    FileIsRemoteDeviceInformation, // q: FILE_IS_REMOTE_DEVICE_INFORMATION (requires FILE_READ_ATTRIBUTES)
    FileUnusedInformation,
    FileNumaNodeInformation,                // q: FILE_NUMA_NODE_INFORMATION
    FileStandardLinkInformation,            // q: FILE_STANDARD_LINK_INFORMATION
    FileRemoteProtocolInformation,          // q: FILE_REMOTE_PROTOCOL_INFORMATION
    FileRenameInformationBypassAccessCheck, // (kernel-mode only); s: FILE_RENAME_INFORMATION // since WIN8
    FileLinkInformationBypassAccessCheck,   // (kernel-mode only); s: FILE_LINK_INFORMATION
    FileVolumeNameInformation,              // q: FILE_VOLUME_NAME_INFORMATION
    FileIdInformation,                      // q: FILE_ID_INFORMATION
    FileIdExtdDirectoryInformation,
    // q: FILE_ID_EXTD_DIR_INFORMATION (requires FILE_LIST_DIRECTORY) (NtQueryDirectoryFile[Ex]) // 60
    FileReplaceCompletionInformation, // s: FILE_COMPLETION_INFORMATION // since WINBLUE
    FileHardLinkFullIdInformation,    // q: FILE_LINK_ENTRY_FULL_ID_INFORMATION // FILE_LINKS_FULL_ID_INFORMATION
    FileIdExtdBothDirectoryInformation,
    // q: FILE_ID_EXTD_BOTH_DIR_INFORMATION (requires FILE_LIST_DIRECTORY) (NtQueryDirectoryFile[Ex]) // since THRESHOLD
    FileDispositionInformationEx,             // s: FILE_DISPOSITION_INFO_EX (requires DELETE) // since REDSTONE
    FileRenameInformationEx,                  // s: FILE_RENAME_INFORMATION_EX
    FileRenameInformationExBypassAccessCheck, // (kernel-mode only); s: FILE_RENAME_INFORMATION_EX
    FileDesiredStorageClassInformation,
    // q; s: FILE_DESIRED_STORAGE_CLASS_INFORMATION (q: requires FILE_READ_ATTRIBUTES; s: requires
    // FILE_WRITE_ATTRIBUTES) // since REDSTONE2
    FileStatInformation,            // q: FILE_STAT_INFORMATION (requires FILE_READ_ATTRIBUTES)
    FileMemoryPartitionInformation, // s: FILE_MEMORY_PARTITION_INFORMATION // since REDSTONE3
    FileStatLxInformation,
    // q: FILE_STAT_LX_INFORMATION (requires FILE_READ_ATTRIBUTES and FILE_READ_EA) // since REDSTONE4 // 70
    FileCaseSensitiveInformation,
    // q; s: FILE_CASE_SENSITIVE_INFORMATION (q: requires FILE_READ_ATTRIBUTES; s: requires FILE_WRITE_ATTRIBUTES)
    FileLinkInformationEx,                  // s: FILE_LINK_INFORMATION_EX // since REDSTONE5
    FileLinkInformationExBypassAccessCheck, // (kernel-mode only); s: FILE_LINK_INFORMATION_EX
    FileStorageReserveIdInformation,
    // q; s: FILE_STORAGE_RESERVE_ID_INFORMATION (q: requires FILE_READ_ATTRIBUTES; s: requires FILE_WRITE_ATTRIBUTES)
    FileCaseSensitiveInformationForceAccessCheck, // q; s: FILE_CASE_SENSITIVE_INFORMATION
    FileKnownFolderInformation,
    // q; s: FILE_KNOWN_FOLDER_INFORMATION (q: requires FILE_READ_ATTRIBUTES; s: requires FILE_WRITE_ATTRIBUTES) //
    // since WIN11
    FileStatBasicInformation,              // since 23H2
    FileId64ExtdDirectoryInformation,      // FILE_ID_64_EXTD_DIR_INFORMATION
    FileId64ExtdBothDirectoryInformation,  // FILE_ID_64_EXTD_BOTH_DIR_INFORMATION
    FileIdAllExtdDirectoryInformation,     // FILE_ID_ALL_EXTD_DIR_INFORMATION
    FileIdAllExtdBothDirectoryInformation, // FILE_ID_ALL_EXTD_BOTH_DIR_INFORMATION
    FileStreamReservationInformation,      // FILE_STREAM_RESERVATION_INFORMATION // since 24H2
    FileMupProviderInfo,                   // MUP_PROVIDER_INFORMATION
    FileMaximumInformation
} FILE_INFORMATION_CLASS, *PFILE_INFORMATION_CLASS;

typedef enum _OBJECT_INFORMATION_CLASS
{
    ObjectBasicInformation,         // q: OBJECT_BASIC_INFORMATION
    ObjectNameInformation,          // q: OBJECT_NAME_INFORMATION
    ObjectTypeInformation,          // q: OBJECT_TYPE_INFORMATION
    ObjectTypesInformation,         // q: OBJECT_TYPES_INFORMATION
    ObjectHandleFlagInformation,    // qs: OBJECT_HANDLE_FLAG_INFORMATION
    ObjectSessionInformation,       // s: void // change object session // (requires SeTcbPrivilege)
    ObjectSessionObjectInformation, // s: void // change object session // (requires SeTcbPrivilege)
    MaxObjectInfoClass
} OBJECT_INFORMATION_CLASS;

typedef enum _HARDERROR_RESPONSE_OPTION
{
    OptionAbortRetryIgnore,
    OptionOk,
    OptionOkCancel,
    OptionRetryCancel,
    OptionYesNo,
    OptionYesNoCancel,
    OptionShutdownSystem,
    OptionOkNoWait,
    OptionCancelTryContinue
} HARDERROR_RESPONSE_OPTION;

typedef enum _HARDERROR_RESPONSE
{
    ResponseReturnToCaller,
    ResponseNotHandled,
    ResponseAbort,
    ResponseCancel,
    ResponseIgnore,
    ResponseNo,
    ResponseOk,
    ResponseRetry,
    ResponseYes,
    ResponseTryAgain,
    ResponseContinue
} HARDERROR_RESPONSE;

typedef USHORT RTL_ATOM;

template <typename Traits>
struct IO_STATUS_BLOCK
{
    union
    {
        NTSTATUS Status;
        typename Traits::PVOID Pointer;
    };

    typename Traits::ULONG_PTR Information;
};

template <typename Traits>
struct OBJECT_ATTRIBUTES
{
    ULONG Length;
    typename Traits::HANDLE RootDirectory;
    EMULATOR_CAST(typename Traits::PVOID, UNICODE_STRING*) ObjectName;
    ULONG Attributes;
    typename Traits::PVOID SecurityDescriptor;       // PSECURITY_DESCRIPTOR;
    typename Traits::PVOID SecurityQualityOfService; // PSECURITY_QUALITY_OF_SERVICE
};

typedef struct _FILE_FS_DEVICE_INFORMATION
{
    DEVICE_TYPE DeviceType;
    ULONG Characteristics;
} FILE_FS_DEVICE_INFORMATION, *PFILE_FS_DEVICE_INFORMATION;

typedef struct _FILE_POSITION_INFORMATION
{
    LARGE_INTEGER CurrentByteOffset;
} FILE_POSITION_INFORMATION, *PFILE_POSITION_INFORMATION;

typedef struct _FILE_STANDARD_INFORMATION
{
    LARGE_INTEGER AllocationSize;
    LARGE_INTEGER EndOfFile;
    ULONG NumberOfLinks;
    BOOLEAN DeletePending;
    BOOLEAN Directory;
} FILE_STANDARD_INFORMATION, *PFILE_STANDARD_INFORMATION;

typedef struct _FILE_NAME_INFORMATION
{
    ULONG FileNameLength;
    char16_t FileName[1];
} FILE_NAME_INFORMATION, *PFILE_NAME_INFORMATION;

typedef struct _FILE_BASIC_INFORMATION
{
    LARGE_INTEGER CreationTime;   // Specifies the time that the file was created.
    LARGE_INTEGER LastAccessTime; // Specifies the time that the file was last accessed.
    LARGE_INTEGER LastWriteTime;  // Specifies the time that the file was last written to.
    LARGE_INTEGER ChangeTime;     // Specifies the last time the file was changed.
    ULONG FileAttributes;         // Specifies one or more FILE_ATTRIBUTE_XXX flags.
} FILE_BASIC_INFORMATION, *PFILE_BASIC_INFORMATION;

typedef struct _FILE_DIRECTORY_INFORMATION
{
    ULONG NextEntryOffset;
    ULONG FileIndex;
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    LARGE_INTEGER EndOfFile;
    LARGE_INTEGER AllocationSize;
    ULONG FileAttributes;
    ULONG FileNameLength;
    char16_t FileName[1];
} FILE_DIRECTORY_INFORMATION, *PFILE_DIRECTORY_INFORMATION;

typedef struct _FILE_FULL_DIR_INFORMATION
{
    ULONG NextEntryOffset;
    ULONG FileIndex;
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    LARGE_INTEGER EndOfFile;
    LARGE_INTEGER AllocationSize;
    ULONG FileAttributes;
    ULONG FileNameLength;
    ULONG EaSize;
    char16_t FileName[1];
} FILE_FULL_DIR_INFORMATION, *PFILE_FULL_DIR_INFORMATION;

typedef struct _FILE_BOTH_DIR_INFORMATION
{
    ULONG NextEntryOffset;
    ULONG FileIndex;
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    LARGE_INTEGER EndOfFile;
    LARGE_INTEGER AllocationSize;
    ULONG FileAttributes;
    ULONG FileNameLength;
    ULONG EaSize;
    char ShortNameLength;
    char16_t ShortName[12];
    char16_t FileName[1];
} FILE_BOTH_DIR_INFORMATION, *PFILE_BOTH_DIR_INFORMATION;

#ifndef OS_WINDOWS
typedef BOOLEAN SECURITY_CONTEXT_TRACKING_MODE, *PSECURITY_CONTEXT_TRACKING_MODE;
typedef struct _SECURITY_QUALITY_OF_SERVICE
{
    DWORD Length;
    SECURITY_IMPERSONATION_LEVEL ImpersonationLevel;
    SECURITY_CONTEXT_TRACKING_MODE ContextTrackingMode;
    BOOLEAN EffectiveOnly;
} SECURITY_QUALITY_OF_SERVICE, *P_SECURITY_QUALITY_OF_SERVICE;

#endif

typedef struct _PORT_VIEW64
{
    ULONG Length;
    EMULATOR_CAST(std::uint64_t, HANDLE) SectionHandle;
    ULONG SectionOffset;
    EMULATOR_CAST(std::int64_t, SIZE_T) ViewSize;
    EmulatorTraits<Emu64>::PVOID ViewBase;
    EmulatorTraits<Emu64>::PVOID ViewRemoteBase;
} PORT_VIEW64, *PPORT_VIEW64;

typedef struct _REMOTE_PORT_VIEW64
{
    ULONG Length;
    EMULATOR_CAST(std::int64_t, SIZE_T) ViewSize;
    EmulatorTraits<Emu64>::PVOID ViewBase;
} REMOTE_PORT_VIEW64, *PREMOTE_PORT_VIEW64;
