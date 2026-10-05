#include <winsock2.h>
#include <ws2tcpip.h>

#include "capture.hpp"
#include "iat_hook.hpp"
#include "ksecdd_provider_probe.hpp"

#include <windows.h>
#include <security.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#if !defined(_M_X64)
#error This program must be compiled for Windows x64.
#endif

namespace
{

    constexpr std::string_view kHost = "api.github.com";
    constexpr std::string_view kPort = "443";
    constexpr size_t kFirstServerBytes = 1024;
    constexpr size_t kReceiveChunkBytes = 16 * 1024;
    constexpr DWORD kSocketTimeoutMilliseconds = 15000;

    struct RTL_OSVERSIONINFOW_LOCAL
    {
        ULONG dwOSVersionInfoSize;
        ULONG dwMajorVersion;
        ULONG dwMinorVersion;
        ULONG dwBuildNumber;
        ULONG dwPlatformId;
        WCHAR szCSDVersion[128];
    };

    using RtlGetVersionFn = LONG(WINAPI*)(RTL_OSVERSIONINFOW_LOCAL*);

    struct VersionInfo
    {
        ULONG major = 0;
        ULONG minor = 0;
        ULONG build = 0;
    };

    class Lifecycle
    {
      public:
        void Add(std::string event)
        {
            events_.push_back(std::move(event));
        }

        bool Write(const std::filesystem::path& output, std::string& error) const
        {
            std::ofstream file(output / "lifecycle.jsonl", std::ios::trunc);
            if (!file)
            {
                error = "cannot create lifecycle.jsonl";
                return false;
            }
            for (const auto& event : events_)
            {
                file << event << '\n';
            }
            if (!file)
            {
                error = "write failed for lifecycle.jsonl";
                return false;
            }
            return true;
        }

      private:
        std::vector<std::string> events_;
    };

    struct NetworkState
    {
        SOCKET socket = INVALID_SOCKET;
        size_t sendSequence = 0;
        size_t receiveSequence = 0;
        uint64_t sentBytes = 0;
        uint64_t receivedBytes = 0;
        std::string address;
        std::string family;
    };

    VersionInfo GetWindowsVersion()
    {
        VersionInfo result;
        auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
        if (fn)
        {
            RTL_OSVERSIONINFOW_LOCAL vi{};
            vi.dwOSVersionInfoSize = sizeof(vi);
            if (fn(&vi) == 0)
            {
                result.major = vi.dwMajorVersion;
                result.minor = vi.dwMinorVersion;
                result.build = vi.dwBuildNumber;
            }
        }
        return result;
    }

    std::wstring ModulePath(HMODULE module)
    {
        if (!module)
        {
            return {};
        }
        std::wstring buffer(32768, L'\0');
        const DWORD length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0 || length >= buffer.size())
        {
            return {};
        }
        buffer.resize(length);
        return buffer;
    }

    std::string Utf8(const std::wstring& text)
    {
        if (text.empty())
        {
            return {};
        }
        const int size =
            WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        if (size <= 0)
        {
            return {};
        }
        std::string out(static_cast<size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
        return out;
    }

    std::string FileVersion(const std::wstring& path)
    {
        DWORD ignored = 0;
        const DWORD bytes = GetFileVersionInfoSizeW(path.c_str(), &ignored);
        if (!bytes)
        {
            return {};
        }
        std::vector<uint8_t> data(bytes);
        if (!GetFileVersionInfoW(path.c_str(), 0, bytes, data.data()))
        {
            return {};
        }
        VS_FIXEDFILEINFO* info = nullptr;
        UINT infoBytes = 0;
        if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &infoBytes) || !info || infoBytes < sizeof(*info))
        {
            return {};
        }
        std::ostringstream os;
        os << HIWORD(info->dwFileVersionMS) << '.' << LOWORD(info->dwFileVersionMS) << '.' << HIWORD(info->dwFileVersionLS) << '.'
           << LOWORD(info->dwFileVersionLS);
        return os.str();
    }

    bool WriteEnvironment(const std::filesystem::path& output, const VersionInfo& version, HMODULE rpcrt4, std::string& error)
    {
        HMODULE sspicli = GetModuleHandleW(L"sspicli.dll");
        const std::wstring rpcPath = ModulePath(rpcrt4);
        const std::wstring sspiPath = ModulePath(sspicli);
        std::ofstream file(output / "environment.json", std::ios::trunc);
        if (!file)
        {
            error = "cannot create environment.json";
            return false;
        }
        file << "{\n"
             << "  \"windows\": {\"major\": " << version.major << ", \"minor\": " << version.minor << ", \"build\": " << version.build
             << "},\n"
             << "  \"process_architecture\": \"x86-64\",\n"
             << "  \"pid\": " << GetCurrentProcessId() << ",\n"
             << "  \"rpcrt4\": {\"path\": \"" << capture::JsonEscape(Utf8(rpcPath)) << "\", \"base_address\": \"0x" << std::hex
             << reinterpret_cast<uintptr_t>(rpcrt4) << std::dec << "\", \"file_version\": \"" << capture::JsonEscape(FileVersion(rpcPath))
             << "\"},\n"
             << "  \"sspicli\": {\"loaded\": " << (sspicli ? "true" : "false") << ", \"path\": \"" << capture::JsonEscape(Utf8(sspiPath))
             << "\", \"base_address\": \"0x" << std::hex << reinterpret_cast<uintptr_t>(sspicli) << std::dec << "\", \"file_version\": \""
             << capture::JsonEscape(FileVersion(sspiPath)) << "\"}\n"
             << "}\n";
        if (!file)
        {
            error = "write failed for environment.json";
            return false;
        }
        return true;
    }

    std::string NumberedPath(std::string_view directory, std::string_view stem, size_t sequence, std::string_view suffix = ".bin")
    {
        std::ostringstream os;
        os << directory << '/' << stem << '_' << std::setw(4) << std::setfill('0') << sequence << suffix;
        return os.str();
    }

    bool WriteBytes(const std::filesystem::path& output, const std::string& relative, const uint8_t* data, size_t size, std::string& error)
    {
        const auto path = output / std::filesystem::path(relative);
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec)
        {
            error = "cannot create " + path.parent_path().string() + ": " + ec.message();
            return false;
        }
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
        {
            error = "cannot create " + path.string();
            return false;
        }
        if (size != 0)
        {
            file.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
        }
        if (!file)
        {
            error = "write failed for " + path.string();
            return false;
        }
        return true;
    }

    bool WriteBytes(const std::filesystem::path& output, const std::string& relative, const std::vector<uint8_t>& data, std::string& error)
    {
        return WriteBytes(output, relative, data.data(), data.size(), error);
    }

    std::string StatusJson(SECURITY_STATUS status)
    {
        std::ostringstream os;
        os << "\"status\":\"" << capture::HexStatus(status) << "\",\"status_name\":\""
           << capture::JsonEscape(capture::SecurityStatusName(status)) << '"';
        return os.str();
    }

    std::string BufferDescriptors(const SecBuffer* buffers, size_t count, const uint8_t* base, size_t size)
    {
        std::ostringstream os;
        os << '[';
        const uintptr_t begin = reinterpret_cast<uintptr_t>(base);
        const uintptr_t end = begin + size;
        for (size_t index = 0; index < count; ++index)
        {
            if (index != 0)
            {
                os << ',';
            }
            const uintptr_t pointer = reinterpret_cast<uintptr_t>(buffers[index].pvBuffer);
            os << "{\"type\":" << buffers[index].BufferType << ",\"length\":" << buffers[index].cbBuffer << ",\"offset\":";
            if (buffers[index].pvBuffer && base && pointer >= begin && pointer <= end)
            {
                os << pointer - begin;
            }
            else
            {
                os << "null";
            }
            os << '}';
        }
        os << ']';
        return os.str();
    }

    void PrintHook(const char* name, const iat::HookResult& hook, void* replacement)
    {
        std::cout << "[IAT] " << name << "\n"
                  << "      original = " << hook.callable_original << "\n"
                  << "      hook     = " << replacement << "\n"
                  << "      normal slots = " << hook.normal_matches << ", delay slots = " << hook.delay_matches << "\n";
        for (const auto& diagnostic : hook.diagnostics)
        {
            std::cout << "      note: " << diagnostic << "\n";
        }
    }

    void PrintSecurityResult(const char* api, SECURITY_STATUS status)
    {
        std::cout << api << ": " << capture::HexStatus(status);
        const std::string name = capture::SecurityStatusName(status);
        if (!name.empty())
        {
            std::cout << ' ' << name;
        }
        std::cout << "\n";
    }

    bool ConnectSocket(NetworkState& network, std::string& error)
    {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        addrinfo* addresses = nullptr;
        const int resolveStatus = getaddrinfo(kHost.data(), kPort.data(), &hints, &addresses);
        if (resolveStatus != 0)
        {
            error = "getaddrinfo failed: " + std::to_string(resolveStatus);
            return false;
        }

        for (const addrinfo* address = addresses; address; address = address->ai_next)
        {
            SOCKET candidate = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
            if (candidate == INVALID_SOCKET)
            {
                continue;
            }
            setsockopt(candidate, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&kSocketTimeoutMilliseconds),
                       sizeof(kSocketTimeoutMilliseconds));
            setsockopt(candidate, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&kSocketTimeoutMilliseconds),
                       sizeof(kSocketTimeoutMilliseconds));
            if (connect(candidate, address->ai_addr, static_cast<int>(address->ai_addrlen)) == 0)
            {
                char host[NI_MAXHOST]{};
                if (getnameinfo(address->ai_addr, static_cast<socklen_t>(address->ai_addrlen), host, sizeof(host), nullptr, 0,
                                NI_NUMERICHOST) != 0)
                {
                    std::snprintf(host, sizeof(host), "<unknown>");
                }
                network.socket = candidate;
                network.address = host;
                network.family = address->ai_family == AF_INET ? "IPv4" : (address->ai_family == AF_INET6 ? "IPv6" : "other");
                break;
            }
            closesocket(candidate);
        }
        freeaddrinfo(addresses);
        if (network.socket == INVALID_SOCKET)
        {
            error = "connect failed for every resolved address: " + std::to_string(WSAGetLastError());
            return false;
        }
        std::cout << "Connected: " << network.address << " (" << network.family << ")\n";
        return true;
    }

    bool SendBytes(NetworkState& network, const std::filesystem::path& output, Lifecycle& lifecycle, const uint8_t* data, size_t size,
                   std::string_view phase, std::string& error)
    {
        size_t offset = 0;
        while (offset < size)
        {
            const int request = static_cast<int>(std::min(size - offset, static_cast<size_t>(std::numeric_limits<int>::max())));
            const int sent = send(network.socket, reinterpret_cast<const char*>(data + offset), request, 0);
            if (sent == SOCKET_ERROR || sent == 0)
            {
                error = "send failed: " + std::to_string(WSAGetLastError());
                return false;
            }
            const std::string file = NumberedPath("network", "send", ++network.sendSequence);
            if (!WriteBytes(output, file, data + offset, static_cast<size_t>(sent), error))
            {
                return false;
            }
            network.sentBytes += static_cast<size_t>(sent);
            lifecycle.Add("{\"event\":\"network\",\"direction\":\"send\",\"file\":\"" + file + "\",\"length\":" + std::to_string(sent) +
                          ",\"phase\":\"" + std::string(phase) + "\"}");
            offset += static_cast<size_t>(sent);
        }
        return true;
    }

    bool ReceiveBytes(NetworkState& network, const std::filesystem::path& output, Lifecycle& lifecycle, size_t requested, bool exact,
                      std::string_view phase, std::vector<uint8_t>& bytes, std::string& error)
    {
        bytes.clear();
        bytes.reserve(requested);
        do
        {
            std::array<uint8_t, kReceiveChunkBytes> buffer{};
            const size_t remaining = requested - bytes.size();
            const int amount = static_cast<int>(std::min(remaining, buffer.size()));
            const int received = recv(network.socket, reinterpret_cast<char*>(buffer.data()), amount, 0);
            if (received == SOCKET_ERROR)
            {
                error = "recv failed: " + std::to_string(WSAGetLastError());
                return false;
            }
            if (received == 0)
            {
                error = "peer closed the connection";
                return false;
            }
            bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + received);
        } while (exact && bytes.size() < requested);

        const std::string file = NumberedPath("network", "recv", ++network.receiveSequence);
        if (!WriteBytes(output, file, bytes, error))
        {
            return false;
        }
        network.receivedBytes += bytes.size();
        lifecycle.Add("{\"event\":\"network\",\"direction\":\"recv\",\"file\":\"" + file + "\",\"length\":" + std::to_string(bytes.size()) +
                      ",\"phase\":\"" + std::string(phase) + "\"}");
        return true;
    }

    bool AppendReceived(NetworkState& network, const std::filesystem::path& output, Lifecycle& lifecycle, size_t requested, bool exact,
                        std::string_view phase, std::vector<uint8_t>& pending, std::string& error)
    {
        std::vector<uint8_t> received;
        if (!ReceiveBytes(network, output, lifecycle, requested, exact, phase, received, error))
        {
            return false;
        }
        pending.insert(pending.end(), received.begin(), received.end());
        return true;
    }

    bool CopyExtra(const SecBuffer* buffers, size_t count, const std::vector<uint8_t>& input, std::vector<uint8_t>& extra,
                   std::string& error)
    {
        extra.clear();
        for (size_t index = 0; index < count; ++index)
        {
            if ((buffers[index].BufferType & ~SECBUFFER_ATTRMASK) != SECBUFFER_EXTRA)
            {
                continue;
            }
            if (buffers[index].cbBuffer > input.size())
            {
                error = "SSPI returned an oversized SECBUFFER_EXTRA region";
                return false;
            }

            const auto* inputBegin = input.data();
            const auto* inputEnd = inputBegin + input.size();
            const auto* begin = static_cast<const uint8_t*>(buffers[index].pvBuffer);
            if (!begin)
            {
                // Schannel can report only the trailing byte count in an originally
                // empty buffer, leaving pvBuffer null on current Windows builds.
                begin = inputEnd - buffers[index].cbBuffer;
            }
            else
            {
                const uintptr_t beginAddress = reinterpret_cast<uintptr_t>(begin);
                const uintptr_t inputAddress = reinterpret_cast<uintptr_t>(inputBegin);
                if (beginAddress < inputAddress || beginAddress > inputAddress + input.size() ||
                    buffers[index].cbBuffer > inputAddress + input.size() - beginAddress)
                {
                    error = "SSPI returned an invalid SECBUFFER_EXTRA region";
                    return false;
                }
            }
            extra.assign(begin, begin + buffers[index].cbBuffer);
            return true;
        }
        return true;
    }

    ULONG MissingBytes(const SecBuffer* buffers, size_t count)
    {
        for (size_t index = 0; index < count; ++index)
        {
            if ((buffers[index].BufferType & ~SECBUFFER_ATTRMASK) == SECBUFFER_MISSING)
            {
                return buffers[index].cbBuffer;
            }
        }
        return 0;
    }

} // namespace

int main()
{
    const std::filesystem::path output = L"capture";
    std::cout << "SSPI ALPC capture utility\n\n";
    const VersionInfo windows = GetWindowsVersion();
    std::cout << "Windows build: " << windows.major << '.' << windows.minor << '.' << windows.build << "\n";

    std::string error;
    if (!capture::PrepareOutputDirectory(output, error))
    {
        std::cerr << "[FATAL] " << error << "\n";
        return 2;
    }

    HMODULE rpcrt4 = LoadLibraryW(L"rpcrt4.dll");
    if (!rpcrt4)
    {
        std::cerr << "[FATAL] LoadLibraryW(rpcrt4.dll) failed, Win32 error " << GetLastError() << "\n";
        return 3;
    }
    std::cout << "[IAT] rpcrt4.dll = " << Utf8(ModulePath(rpcrt4)) << "\n";

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    capture::RealNtAlpcConnectPortEx = reinterpret_cast<capture::NtAlpcConnectPortExFn>(GetProcAddress(ntdll, "NtAlpcConnectPortEx"));
    capture::RealNtAlpcSendWaitReceivePort =
        reinterpret_cast<capture::NtAlpcSendWaitReceivePortFn>(GetProcAddress(ntdll, "NtAlpcSendWaitReceivePort"));
    if (!capture::RealNtAlpcConnectPortEx || !capture::RealNtAlpcSendWaitReceivePort)
    {
        std::cerr << "[FATAL] one or both required ALPC exports are absent from ntdll.dll.\n";
        return 4;
    }

    auto connectHook =
        iat::HookImport(rpcrt4, "ntdll.dll", "NtAlpcConnectPortEx", reinterpret_cast<void*>(&capture::HookNtAlpcConnectPortEx));
    PrintHook("NtAlpcConnectPortEx", connectHook, reinterpret_cast<void*>(&capture::HookNtAlpcConnectPortEx));
    if (!connectHook.ok())
    {
        std::string restoreError;
        iat::RestoreImports(connectHook, &restoreError);
        std::cerr << "[FATAL] required NtAlpcConnectPortEx IAT hook was not installed; "
                     "no inline-hook fallback will be attempted.\n";
        if (!restoreError.empty())
        {
            std::cerr << "[IAT] " << restoreError << "\n";
        }
        return 4;
    }
    capture::RealNtAlpcConnectPortEx = reinterpret_cast<capture::NtAlpcConnectPortExFn>(connectHook.callable_original);

    auto sendHook =
        iat::HookImport(rpcrt4, "ntdll.dll", "NtAlpcSendWaitReceivePort", reinterpret_cast<void*>(&capture::HookNtAlpcSendWaitReceivePort));
    PrintHook("NtAlpcSendWaitReceivePort", sendHook, reinterpret_cast<void*>(&capture::HookNtAlpcSendWaitReceivePort));
    if (!sendHook.ok())
    {
        std::string restoreError;
        iat::RestoreImports(connectHook, &restoreError);
        std::cerr << "[FATAL] required NtAlpcSendWaitReceivePort IAT hook was not installed; "
                     "no inline-hook fallback will be attempted.\n";
        if (!restoreError.empty())
        {
            std::cerr << "[IAT] " << restoreError << "\n";
        }
        return 5;
    }
    capture::RealNtAlpcSendWaitReceivePort = reinterpret_cast<capture::NtAlpcSendWaitReceivePortFn>(sendHook.callable_original);

    if (!capture::StartDeviceCapture(error))
    {
        std::string restoreSendError;
        std::string restoreConnectError;
        iat::RestoreImports(sendHook, &restoreSendError);
        iat::RestoreImports(connectHook, &restoreConnectError);
        std::cerr << "[FATAL] device capture hooks could not be installed: " << error << "\n";
        return 5;
    }

    HMODULE bcrypt = LoadLibraryW(L"bcrypt.dll");
    HMODULE bcryptPrimitives = LoadLibraryW(L"bcryptprimitives.dll");
    if (!bcrypt || !bcryptPrimitives)
    {
        const DWORD loadError = GetLastError();
        if (bcryptPrimitives)
        {
            FreeLibrary(bcryptPrimitives);
        }
        if (bcrypt)
        {
            FreeLibrary(bcrypt);
        }
        std::string deviceRestoreError;
        std::string restoreSendError;
        std::string restoreConnectError;
        capture::StopDeviceCapture(deviceRestoreError);
        iat::RestoreImports(sendHook, &restoreSendError);
        iat::RestoreImports(connectHook, &restoreConnectError);
        std::cerr << "[FATAL] cryptography providers could not be preloaded, Win32 error " << loadError << "\n";
        return 5;
    }

    std::cout << "[PROBE] capturing DSA, RSA, provider enumeration, and key storage from KsecDD\n";
    const bool providerProbeSucceeded = RunKsecDdProviderProbe(error);
    if (providerProbeSucceeded)
    {
        std::cout << "[PROBE] KsecDD provider responses captured\n";
    }
    else
    {
        std::cerr << "[ERROR] KsecDD provider probe failed: " << error << "\n";
    }

    Lifecycle lifecycle;
    NetworkState network;
    WSADATA winsock{};
    bool winsockStarted = false;
    CredHandle credential{};
    CtxtHandle context{};
    TimeStamp expiry{};
    bool credentialCreated = false;
    bool contextCreated = false;
    int exitCode = providerProbeSucceeded ? 0 : 6;

    do
    {
        if (!providerProbeSucceeded)
        {
            break;
        }
        if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0)
        {
            error = "WSAStartup failed: " + std::to_string(WSAGetLastError());
            exitCode = 6;
            break;
        }
        winsockStarted = true;
        if (!ConnectSocket(network, error))
        {
            exitCode = 6;
            break;
        }
        lifecycle.Add("{\"event\":\"connect\",\"host\":\"api.github.com\","
                      "\"port\":443,\"address\":\"" +
                      capture::JsonEscape(network.address) + "\",\"family\":\"" + network.family + "\"}");

        const SECURITY_STATUS acquireStatus =
            AcquireCredentialsHandleA(nullptr, const_cast<SEC_CHAR*>("Microsoft Unified Security Protocol Provider"), SECPKG_CRED_OUTBOUND,
                                      nullptr, nullptr, nullptr, nullptr, &credential, &expiry);
        PrintSecurityResult("AcquireCredentialsHandleA", acquireStatus);
        lifecycle.Add("{\"event\":\"AcquireCredentialsHandleA\"," + StatusJson(acquireStatus) + "}");
        if (acquireStatus != SEC_E_OK)
        {
            error = "AcquireCredentialsHandleA failed with " + capture::HexStatus(acquireStatus);
            exitCode = 6;
            break;
        }
        credentialCreated = true;

        const ULONG requested = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY | ISC_REQ_EXTENDED_ERROR |
                                ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM;
        std::vector<uint8_t> pendingInput;
        bool firstServerRead = true;
        ULONG contextAttributes = 0;
        size_t iscStep = 0;
        SECURITY_STATUS initializeStatus = SEC_I_CONTINUE_NEEDED;

        while (initializeStatus != SEC_E_OK)
        {
            ++iscStep;
            std::string inputFile;
            std::array<SecBuffer, 2> inputBuffers{};
            SecBufferDesc inputDescriptor{};
            SecBufferDesc* input = nullptr;
            std::string inputBefore = "[]";
            if (iscStep != 1)
            {
                inputFile = NumberedPath("sspi", "isc", iscStep, "_input.bin");
                if (!WriteBytes(output, inputFile, pendingInput, error))
                {
                    exitCode = 6;
                    break;
                }
                inputBuffers[0].BufferType = SECBUFFER_TOKEN;
                inputBuffers[0].cbBuffer = static_cast<ULONG>(pendingInput.size());
                inputBuffers[0].pvBuffer = pendingInput.data();
                inputBuffers[1].BufferType = SECBUFFER_EMPTY;
                inputDescriptor.ulVersion = SECBUFFER_VERSION;
                inputDescriptor.cBuffers = static_cast<ULONG>(inputBuffers.size());
                inputDescriptor.pBuffers = inputBuffers.data();
                input = &inputDescriptor;
                inputBefore = BufferDescriptors(inputBuffers.data(), inputBuffers.size(), pendingInput.data(), pendingInput.size());
            }

            SecBuffer outputBuffer{};
            outputBuffer.BufferType = SECBUFFER_TOKEN;
            SecBufferDesc outputDescriptor{};
            outputDescriptor.ulVersion = SECBUFFER_VERSION;
            outputDescriptor.cBuffers = 1;
            outputDescriptor.pBuffers = &outputBuffer;
            initializeStatus =
                InitializeSecurityContextA(&credential, contextCreated ? &context : nullptr, const_cast<SEC_CHAR*>(kHost.data()), requested,
                                           0, SECURITY_NATIVE_DREP, input, 0, &context, &outputDescriptor, &contextAttributes, &expiry);
            const SECURITY_STATUS iscStatus = initializeStatus;
            if (!contextCreated && (iscStatus == SEC_E_OK || iscStatus == SEC_I_CONTINUE_NEEDED || iscStatus == SEC_I_COMPLETE_NEEDED ||
                                    iscStatus == SEC_I_COMPLETE_AND_CONTINUE))
            {
                contextCreated = true;
            }

            const std::string inputAfter =
                input ? BufferDescriptors(inputBuffers.data(), inputBuffers.size(), pendingInput.data(), pendingInput.size()) : "[]";
            std::string completeEvent;
            if (iscStatus == SEC_I_COMPLETE_NEEDED || iscStatus == SEC_I_COMPLETE_AND_CONTINUE)
            {
                const SECURITY_STATUS completeStatus = CompleteAuthToken(&context, &outputDescriptor);
                PrintSecurityResult("CompleteAuthToken", completeStatus);
                completeEvent =
                    "{\"event\":\"CompleteAuthToken\",\"step\":" + std::to_string(iscStep) + "," + StatusJson(completeStatus) + "}";
                if (completeStatus != SEC_E_OK)
                {
                    error = "CompleteAuthToken failed with " + capture::HexStatus(completeStatus);
                    exitCode = 6;
                }
                initializeStatus = iscStatus == SEC_I_COMPLETE_NEEDED ? SEC_E_OK : SEC_I_CONTINUE_NEEDED;
            }

            std::string outputFile;
            if (outputBuffer.pvBuffer && outputBuffer.cbBuffer != 0)
            {
                outputFile = NumberedPath("sspi", "isc", iscStep, "_output_token.bin");
                if (!WriteBytes(output, outputFile, static_cast<const uint8_t*>(outputBuffer.pvBuffer), outputBuffer.cbBuffer, error))
                {
                    exitCode = 6;
                }
            }

            std::ostringstream iscEvent;
            iscEvent << "{\"event\":\"InitializeSecurityContextA\",\"step\":" << iscStep << ',' << StatusJson(iscStatus)
                     << ",\"context_attributes\":" << contextAttributes << ",\"input_file\":";
            if (inputFile.empty())
            {
                iscEvent << "null";
            }
            else
            {
                iscEvent << '"' << inputFile << '"';
            }
            iscEvent << ",\"input_buffers_before\":" << inputBefore << ",\"input_buffers_after\":" << inputAfter << ",\"output_file\":";
            if (outputFile.empty())
            {
                iscEvent << "null";
            }
            else
            {
                iscEvent << '"' << outputFile << '"';
            }
            iscEvent << ",\"output_length\":" << outputBuffer.cbBuffer << '}';
            lifecycle.Add(iscEvent.str());
            if (!completeEvent.empty())
            {
                lifecycle.Add(std::move(completeEvent));
            }

            PrintSecurityResult("InitializeSecurityContextA", iscStatus);
            std::cout << "  step " << iscStep << ", attributes 0x" << std::hex << contextAttributes << std::dec << ", input "
                      << pendingInput.size() << " bytes, output " << outputBuffer.cbBuffer << " bytes\n";

            if (exitCode == 0 && outputBuffer.pvBuffer && outputBuffer.cbBuffer != 0 &&
                !SendBytes(network, output, lifecycle, static_cast<const uint8_t*>(outputBuffer.pvBuffer), outputBuffer.cbBuffer,
                           "handshake", error))
            {
                exitCode = 6;
            }
            if (outputBuffer.pvBuffer)
            {
                FreeContextBuffer(outputBuffer.pvBuffer);
            }
            if (exitCode != 0)
            {
                break;
            }

            if (initializeStatus == SEC_E_INCOMPLETE_MESSAGE)
            {
                const ULONG missing = MissingBytes(inputBuffers.data(), inputBuffers.size());
                if (!AppendReceived(network, output, lifecycle, missing != 0 ? missing : kReceiveChunkBytes, missing != 0, "handshake",
                                    pendingInput, error))
                {
                    exitCode = 6;
                    break;
                }
                continue;
            }

            if (initializeStatus != SEC_E_OK && initializeStatus != SEC_I_CONTINUE_NEEDED)
            {
                error = "InitializeSecurityContextA failed with " + capture::HexStatus(initializeStatus);
                exitCode = 6;
                break;
            }

            std::vector<uint8_t> extra;
            if (input && !CopyExtra(inputBuffers.data(), inputBuffers.size(), pendingInput, extra, error))
            {
                exitCode = 6;
                break;
            }
            pendingInput = std::move(extra);
            if (initializeStatus == SEC_E_OK)
            {
                break;
            }
            if (pendingInput.empty())
            {
                const size_t requestedBytes = firstServerRead ? kFirstServerBytes : kReceiveChunkBytes;
                if (!AppendReceived(network, output, lifecycle, requestedBytes, firstServerRead, "handshake", pendingInput, error))
                {
                    exitCode = 6;
                    break;
                }
                firstServerRead = false;
            }
        }
        if (exitCode != 0)
        {
            break;
        }

        SecPkgContext_StreamSizes streamSizes{};
        const SECURITY_STATUS queryStatus = QueryContextAttributesA(&context, SECPKG_ATTR_STREAM_SIZES, &streamSizes);
        PrintSecurityResult("QueryContextAttributesA(STREAM_SIZES)", queryStatus);
        std::cout << "  header " << streamSizes.cbHeader << ", trailer " << streamSizes.cbTrailer << ", maximum message "
                  << streamSizes.cbMaximumMessage << ", buffers " << streamSizes.cBuffers << ", block " << streamSizes.cbBlockSize << "\n";
        lifecycle.Add("{\"event\":\"QueryContextAttributesA\",\"attribute\":"
                      "\"SECPKG_ATTR_STREAM_SIZES\"," +
                      StatusJson(queryStatus) + ",\"header\":" + std::to_string(streamSizes.cbHeader) + ",\"trailer\":" +
                      std::to_string(streamSizes.cbTrailer) + ",\"maximum_message\":" + std::to_string(streamSizes.cbMaximumMessage) +
                      ",\"buffers\":" + std::to_string(streamSizes.cBuffers) +
                      ",\"block_size\":" + std::to_string(streamSizes.cbBlockSize) + "}");
        if (queryStatus != SEC_E_OK)
        {
            error = "QueryContextAttributesA failed with " + capture::HexStatus(queryStatus);
            exitCode = 6;
            break;
        }

        constexpr std::string_view request = "GET / HTTP/1.1\r\n"
                                             "Host: api.github.com\r\n"
                                             "User-Agent: sspi-alpc-capture/1.0\r\n"
                                             "Accept: */*\r\n"
                                             "Connection: close\r\n"
                                             "\r\n";
        const std::string plaintextFile = "sspi/http_request_plaintext.bin";
        if (!WriteBytes(output, plaintextFile, reinterpret_cast<const uint8_t*>(request.data()), request.size(), error))
        {
            exitCode = 6;
            break;
        }

        std::vector<uint8_t> encrypted(streamSizes.cbHeader + request.size() + streamSizes.cbTrailer);
        std::copy(request.begin(), request.end(), encrypted.begin() + streamSizes.cbHeader);
        std::array<SecBuffer, 4> encryptBuffers{};
        encryptBuffers[0] = {streamSizes.cbHeader, SECBUFFER_STREAM_HEADER, encrypted.data()};
        encryptBuffers[1] = {static_cast<ULONG>(request.size()), SECBUFFER_DATA, encrypted.data() + streamSizes.cbHeader};
        encryptBuffers[2] = {streamSizes.cbTrailer, SECBUFFER_STREAM_TRAILER, encrypted.data() + streamSizes.cbHeader + request.size()};
        encryptBuffers[3].BufferType = SECBUFFER_EMPTY;
        SecBufferDesc encryptDescriptor{SECBUFFER_VERSION, static_cast<ULONG>(encryptBuffers.size()), encryptBuffers.data()};
        const std::string encryptBefore =
            BufferDescriptors(encryptBuffers.data(), encryptBuffers.size(), encrypted.data(), encrypted.size());
        const SECURITY_STATUS encryptStatus = EncryptMessage(&context, 0, &encryptDescriptor, 0);
        const std::string encryptAfter =
            BufferDescriptors(encryptBuffers.data(), encryptBuffers.size(), encrypted.data(), encrypted.size());
        PrintSecurityResult("EncryptMessage", encryptStatus);

        std::vector<uint8_t> ciphertext;
        for (size_t index = 0; index < 3; ++index)
        {
            const auto* begin = static_cast<const uint8_t*>(encryptBuffers[index].pvBuffer);
            ciphertext.insert(ciphertext.end(), begin, begin + encryptBuffers[index].cbBuffer);
        }
        const std::string ciphertextFile = NumberedPath("sspi", "encrypt", 1, "_ciphertext.bin");
        if (encryptStatus == SEC_E_OK && !WriteBytes(output, ciphertextFile, ciphertext, error))
        {
            exitCode = 6;
        }
        lifecycle.Add("{\"event\":\"EncryptMessage\"," + StatusJson(encryptStatus) + ",\"plaintext_file\":\"" + plaintextFile +
                      "\",\"plaintext_length\":" + std::to_string(request.size()) + ",\"buffers_before\":" + encryptBefore +
                      ",\"buffers_after\":" + encryptAfter + ",\"output_file\":\"" + ciphertextFile +
                      "\",\"output_length\":" + std::to_string(ciphertext.size()) + "}");
        std::cout << "  plaintext " << request.size() << " bytes, ciphertext " << ciphertext.size() << " bytes\n";
        if (encryptStatus != SEC_E_OK)
        {
            error = "EncryptMessage failed with " + capture::HexStatus(encryptStatus);
            exitCode = 6;
        }
        if (exitCode != 0 || !SendBytes(network, output, lifecycle, ciphertext.data(), ciphertext.size(), "application", error))
        {
            exitCode = 6;
            break;
        }

        std::vector<uint8_t> encryptedPending;
        std::vector<uint8_t> decryptedPlaintext;
        size_t decryptStep = 0;
        while (decryptedPlaintext.empty())
        {
            if (encryptedPending.empty() &&
                !AppendReceived(network, output, lifecycle, kReceiveChunkBytes, false, "application", encryptedPending, error))
            {
                exitCode = 6;
                break;
            }
            ++decryptStep;
            const std::string decryptInputFile = NumberedPath("sspi", "decrypt", decryptStep, "_input.bin");
            if (!WriteBytes(output, decryptInputFile, encryptedPending, error))
            {
                exitCode = 6;
                break;
            }

            std::array<SecBuffer, 4> decryptBuffers{};
            decryptBuffers[0] = {static_cast<ULONG>(encryptedPending.size()), SECBUFFER_DATA, encryptedPending.data()};
            for (size_t index = 1; index < decryptBuffers.size(); ++index)
            {
                decryptBuffers[index].BufferType = SECBUFFER_EMPTY;
            }
            SecBufferDesc decryptDescriptor{SECBUFFER_VERSION, static_cast<ULONG>(decryptBuffers.size()), decryptBuffers.data()};
            const std::string decryptBefore =
                BufferDescriptors(decryptBuffers.data(), decryptBuffers.size(), encryptedPending.data(), encryptedPending.size());
            const SECURITY_STATUS decryptStatus = DecryptMessage(&context, &decryptDescriptor, 0, nullptr);
            const std::string decryptAfter =
                BufferDescriptors(decryptBuffers.data(), decryptBuffers.size(), encryptedPending.data(), encryptedPending.size());

            std::string plaintextOutputFile;
            if (decryptStatus == SEC_E_OK)
            {
                for (const auto& buffer : decryptBuffers)
                {
                    if ((buffer.BufferType & ~SECBUFFER_ATTRMASK) == SECBUFFER_DATA && buffer.cbBuffer != 0)
                    {
                        const auto* begin = static_cast<const uint8_t*>(buffer.pvBuffer);
                        decryptedPlaintext.assign(begin, begin + buffer.cbBuffer);
                        plaintextOutputFile = NumberedPath("sspi", "decrypt", decryptStep, "_plaintext.bin");
                        if (!WriteBytes(output, plaintextOutputFile, decryptedPlaintext, error))
                        {
                            exitCode = 6;
                        }
                        break;
                    }
                }
            }

            std::ostringstream decryptEvent;
            decryptEvent << "{\"event\":\"DecryptMessage\",\"step\":" << decryptStep << ',' << StatusJson(decryptStatus)
                         << ",\"input_file\":\"" << decryptInputFile << "\",\"input_length\":" << encryptedPending.size()
                         << ",\"buffers_before\":" << decryptBefore << ",\"buffers_after\":" << decryptAfter << ",\"plaintext_file\":";
            if (plaintextOutputFile.empty())
            {
                decryptEvent << "null";
            }
            else
            {
                decryptEvent << '"' << plaintextOutputFile << '"';
            }
            decryptEvent << ",\"plaintext_length\":" << decryptedPlaintext.size() << '}';
            lifecycle.Add(decryptEvent.str());
            PrintSecurityResult("DecryptMessage", decryptStatus);
            std::cout << "  step " << decryptStep << ", input " << encryptedPending.size() << " bytes, plaintext "
                      << decryptedPlaintext.size() << " bytes\n";
            if (exitCode != 0)
            {
                break;
            }

            if (decryptStatus == SEC_E_INCOMPLETE_MESSAGE)
            {
                if (!AppendReceived(network, output, lifecycle, kReceiveChunkBytes, false, "application", encryptedPending, error))
                {
                    exitCode = 6;
                    break;
                }
                continue;
            }
            if (decryptStatus != SEC_E_OK)
            {
                error = "DecryptMessage failed with " + capture::HexStatus(decryptStatus);
                exitCode = 6;
                break;
            }

            std::vector<uint8_t> extra;
            if (!CopyExtra(decryptBuffers.data(), decryptBuffers.size(), encryptedPending, extra, error))
            {
                exitCode = 6;
                break;
            }
            encryptedPending = std::move(extra);
        }
        if (exitCode != 0)
        {
            break;
        }

        std::string diagnostic(decryptedPlaintext.begin(), decryptedPlaintext.end());
        std::replace_if(
            diagnostic.begin(), diagnostic.end(),
            [](unsigned char character) { return character != '\r' && character != '\n' && (character < 0x20 || character > 0x7e); }, '.');
        if (diagnostic.size() > 512)
        {
            diagnostic.resize(512);
        }
        std::cout << "First decrypted plaintext:\n" << diagnostic << "\n";
    } while (false);

    if (exitCode != 0)
    {
        std::cerr << "[ERROR] " << error << "\n";
    }

    if (contextCreated)
    {
        const SECURITY_STATUS status = DeleteSecurityContext(&context);
        PrintSecurityResult("DeleteSecurityContext", status);
        lifecycle.Add("{\"event\":\"DeleteSecurityContext\"," + StatusJson(status) + "}");
        if (status != SEC_E_OK && exitCode == 0)
        {
            exitCode = 6;
        }
    }
    if (credentialCreated)
    {
        const SECURITY_STATUS status = FreeCredentialsHandle(&credential);
        PrintSecurityResult("FreeCredentialsHandle", status);
        lifecycle.Add("{\"event\":\"FreeCredentialsHandle\"," + StatusJson(status) + "}");
        if (status != SEC_E_OK && exitCode == 0)
        {
            exitCode = 6;
        }
    }
    if (network.socket != INVALID_SOCKET)
    {
        closesocket(network.socket);
        lifecycle.Add("{\"event\":\"closesocket\"}");
    }
    if (winsockStarted)
    {
        WSACleanup();
        lifecycle.Add("{\"event\":\"WSACleanup\"}");
    }

    FreeLibrary(bcryptPrimitives);
    FreeLibrary(bcrypt);

    std::string deviceRestoreError;
    if (!capture::StopDeviceCapture(deviceRestoreError))
    {
        std::cerr << "[WARN] one or more device-capture hooks could not be restored cleanly: " << deviceRestoreError << "\n";
        if (exitCode == 0)
        {
            exitCode = 6;
        }
    }

    std::string restoreError1;
    std::string restoreError2;
    const bool restoreSend = iat::RestoreImports(sendHook, &restoreError1);
    const bool restoreConnect = iat::RestoreImports(connectHook, &restoreError2);
    if (!restoreSend || !restoreConnect)
    {
        std::cerr << "[WARN] one or more IAT slots could not be restored cleanly: " << restoreError1 << ' ' << restoreError2 << "\n";
        if (exitCode == 0)
        {
            exitCode = 6;
        }
    }

    bool outputOk = true;
    if (!WriteEnvironment(output, windows, rpcrt4, error))
    {
        std::cerr << "[ERROR] " << error << "\n";
        outputOk = false;
    }
    if (!capture::Flush(output, error))
    {
        std::cerr << "[ERROR] capture flush failed: " << error << "\n";
        outputOk = false;
    }
    lifecycle.Add("{\"event\":\"capture_complete\",\"network_sent\":" + std::to_string(network.sentBytes) + ",\"network_received\":" +
                  std::to_string(network.receivedBytes) + ",\"alpc_transactions\":" + std::to_string(capture::TransactionCount()) +
                  ",\"device_io_transactions\":" + std::to_string(capture::DeviceIoCount()) + ",\"exit_code\":" + std::to_string(exitCode) +
                  "}");
    if (!lifecycle.Write(output, error))
    {
        std::cerr << "[ERROR] " << error << "\n";
        outputOk = false;
    }

    std::cout << "\nCapture complete:\n"
              << "    capture/capture.jsonl\n"
              << "    capture/device_io.jsonl\n"
              << "    capture/lifecycle.jsonl\n"
              << "    network sent " << network.sentBytes << " bytes, received " << network.receivedBytes << " bytes\n"
              << "    " << capture::TransactionCount() << " lsasspirpc transaction(s)\n"
              << "    " << capture::DeviceIoCount() << " KsecDD/CNG device I/O transaction(s)\n";
    if (capture::SspiPortHandle.load() == 0)
    {
        std::cerr << "[WARN] no successful connection to \\RPC Control\\lsasspirpc "
                     "was observed. Hooks were installed, but this Windows build/run "
                     "did not expose the expected connection.\n";
    }
    return outputOk ? exitCode : 6;
}
