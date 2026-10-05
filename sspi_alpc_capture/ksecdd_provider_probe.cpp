#include "ksecdd_provider_probe.hpp"

#include "capture.hpp"

#include <array>
#include <cstring>
#include <span>
#include <vector>

namespace
{
    constexpr NTSTATUS kStatusSuccess = static_cast<NTSTATUS>(0x00000000L);
    constexpr NTSTATUS kStatusPending = static_cast<NTSTATUS>(0x00000103L);
    constexpr ULONG kProviderIoctl = 0x00390400;

    struct NativeFunctions
    {
        capture::NtOpenFileFn openFile;
        capture::NtDeviceIoControlFileFn deviceIoControlFile;
        capture::NtCloseFn close;
    };

    NativeFunctions ResolveNativeFunctions()
    {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        return {
            reinterpret_cast<capture::NtOpenFileFn>(GetProcAddress(ntdll, "NtOpenFile")),
            reinterpret_cast<capture::NtDeviceIoControlFileFn>(GetProcAddress(ntdll, "NtDeviceIoControlFile")),
            reinterpret_cast<capture::NtCloseFn>(GetProcAddress(ntdll, "NtClose")),
        };
    }

    bool CaptureRequest(const NativeFunctions& functions, HANDLE device, const std::span<const uint8_t> request,
                        const ULONG initialOutputSize, const char* label, std::string& error)
    {
        constexpr size_t maximumOutputSize = 64 * 1024;
        constexpr NTSTATUS statusBufferOverflow = static_cast<NTSTATUS>(0x80000005L);

        std::vector<uint8_t> response(initialOutputSize);
        NTSTATUS status = kStatusSuccess;
        for (;;)
        {
            IO_STATUS_BLOCK ioStatus{};
            status = functions.deviceIoControlFile(device, nullptr, nullptr, nullptr, &ioStatus, kProviderIoctl,
                                                   const_cast<uint8_t*>(request.data()), static_cast<ULONG>(request.size()),
                                                   response.data(), static_cast<ULONG>(response.size()));
            if (status == kStatusPending)
            {
                status = static_cast<NTSTATUS>(WaitForSingleObject(device, INFINITE) == WAIT_OBJECT_0 ? ioStatus.Status : GetLastError());
            }
            else if (status == kStatusSuccess)
            {
                status = ioStatus.Status;
            }

            if (status != statusBufferOverflow || response.size() == maximumOutputSize)
            {
                break;
            }

            const size_t nextSize = response.size() * 2;
            response.resize(nextSize < maximumOutputSize ? nextSize : maximumOutputSize);
        }

        if (status != kStatusSuccess)
        {
            error = std::string{"KsecDD request failed for "} + label + " with " + capture::HexStatus(status);
            return false;
        }
        return true;
    }

    template <size_t NameLength>
    bool CaptureProvider(const NativeFunctions& functions, HANDLE device, const std::array<uint8_t, 48>& header,
                         const wchar_t (&provider)[NameLength], const ULONG initialOutputSize, const char* label, std::string& error)
    {
        constexpr size_t requestSize = (48 + sizeof(provider) + 7) & ~size_t{7};
        static_assert(requestSize <= 112);

        std::array<uint8_t, 112> request{};
        std::memcpy(request.data(), header.data(), header.size());
        std::memcpy(request.data() + header.size(), provider, sizeof(provider));
        return CaptureRequest(functions, device, std::span<const uint8_t>{request.data(), requestSize}, initialOutputSize, label, error);
    }
}

bool RunKsecDdProviderProbe(std::string& error)
{
    const NativeFunctions functions = ResolveNativeFunctions();
    if (!functions.openFile || !functions.deviceIoControlFile || !functions.close)
    {
        error = "required ntdll device functions are unavailable";
        return false;
    }

    wchar_t deviceBuffer[] = L"\\Device\\KsecDD";
    UNICODE_STRING deviceName{};
    deviceName.Buffer = deviceBuffer;
    deviceName.Length = static_cast<USHORT>((std::size(deviceBuffer) - 1) * sizeof(wchar_t));
    deviceName.MaximumLength = static_cast<USHORT>(std::size(deviceBuffer) * sizeof(wchar_t));

    OBJECT_ATTRIBUTES attributes{};
    InitializeObjectAttributes(&attributes, &deviceName, OBJ_CASE_INSENSITIVE, nullptr, nullptr);

    IO_STATUS_BLOCK ioStatus{};
    HANDLE device = nullptr;
    const NTSTATUS openStatus =
        functions.openFile(&device, FILE_READ_DATA | FILE_WRITE_DATA | SYNCHRONIZE, &attributes, &ioStatus,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
    if (openStatus != kStatusSuccess)
    {
        error = "NtOpenFile(\\Device\\KsecDD) failed";
        return false;
    }

    constexpr std::array<uint8_t, 48> dsaHeader = {
        0x4d, 0x3c, 0x2b, 0x1a, 0x00, 0x00, 0x02, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
    };
    constexpr std::array<uint8_t, 48> rsaHeader = {
        0x4d, 0x3c, 0x2b, 0x1a, 0x00, 0x00, 0x02, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
    };
    constexpr std::array<uint8_t, 48> enumerateProvidersHeader = {
        0x4d, 0x3c, 0x2b, 0x1a, 0x00, 0x00, 0x02, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
    };
    constexpr std::array<uint8_t, 48> enumerateProvidersAlternateHeader = {
        0x4d, 0x3c, 0x2b, 0x1a, 0x00, 0x00, 0x02, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0x02, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
    };
    constexpr std::array<uint8_t, 48> keyStorageHeader = {
        0x4d, 0x3c, 0x2b, 0x1a, 0x00, 0x00, 0x02, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0x01, 0x00, 0x01, 0x00, 0x76, 0x00, 0x69, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x48, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    constexpr wchar_t keyStorageAlgorithm[] = L"KEY_STORAGE";
    constexpr wchar_t keyStorageProvider[] = L"Microsoft Software Key Storage Provider";
    static_assert(keyStorageHeader.size() + sizeof(keyStorageAlgorithm) + sizeof(keyStorageProvider) == 152);
    std::array<uint8_t, 152> keyStorageRequest{};
    std::memcpy(keyStorageRequest.data(), keyStorageHeader.data(), keyStorageHeader.size());
    std::memcpy(keyStorageRequest.data() + keyStorageHeader.size(), keyStorageAlgorithm, sizeof(keyStorageAlgorithm));
    std::memcpy(keyStorageRequest.data() + keyStorageHeader.size() + sizeof(keyStorageAlgorithm), keyStorageProvider,
                sizeof(keyStorageProvider));

    bool succeeded = true;
    succeeded = CaptureProvider(functions, device, dsaHeader, L"DSA", 384, "DSA", error) && succeeded;
    succeeded = CaptureProvider(functions, device, rsaHeader, L"RSA", 384, "RSA", error) && succeeded;
    succeeded = CaptureRequest(functions, device, enumerateProvidersHeader, 384, "provider enumeration", error) && succeeded;
    succeeded =
        CaptureRequest(functions, device, enumerateProvidersAlternateHeader, 384, "alternate provider enumeration", error) && succeeded;
    succeeded = CaptureRequest(functions, device, keyStorageRequest, 384, "Microsoft Software Key Storage Provider", error) && succeeded;

    functions.close(device);
    return succeeded;
}
