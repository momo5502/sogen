#include <cstdint>
#include <cinttypes>
#include <cstring>
#include <string>
#include <fstream>
#include <thread>
#include <atomic>
#include <vector>
#include <optional>
#include <filesystem>
#include <string_view>
#include <array>

#include "../../common/utils/finally.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <intrin.h>

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windns.h>
#include <shlobj.h>
#include <combaseapi.h>
#include <knownfolders.h>
#include <sddl.h>

using namespace std::literals;

// Externally visible and potentially modifiable state
// to trick compiler optimizations
__declspec(dllexport) bool do_the_task = true;

namespace
{
    struct tls_struct
    {
        DWORD num = 1337;

        tls_struct()
        {
            num = GetCurrentThreadId();
        }
    };

    thread_local tls_struct tls_var{};

    // getenv is broken right now :(
    std::string read_env(const char* env)
    {
        std::array<char, 0x1000> buffer{};
        if (!GetEnvironmentVariableA(env, buffer.data(), static_cast<DWORD>(buffer.size())))
        {
            return {};
        }

        return buffer.data();
    }

    bool test_threads()
    {
        constexpr auto thread_count = 5ULL;

        std::atomic<uint64_t> counter{0};

        std::vector<std::thread> threads{};
        threads.reserve(thread_count);

        for (auto i = 0ULL; i < thread_count; ++i)
        {
            threads.emplace_back([&counter] {
                ++counter;
                std::this_thread::yield();
                ++counter;
                // Host scheduling/cpu performance can have impact on emulator scheduling
                // std::this_thread::sleep_for(std::chrono::milliseconds(100));
                ++counter;
            });
        }

        for (auto& t : threads)
        {
            t.join();
        }

        return counter == (thread_count * 3ULL);
    }

    bool test_threads_winapi()
    {
        struct ctx_t
        {
            int iterations;
            int result;
        };

        static LPTHREAD_START_ROUTINE thread_proc = [](LPVOID lpParameter) -> DWORD {
            ctx_t& c = *static_cast<ctx_t*>(lpParameter);
            c.result = 0;
            for (int i = 1; i <= c.iterations; i++)
            {
                ++c.result;
            }
            return 0;
        };

        constexpr int thread_count = 5;
        std::array<HANDLE, thread_count> threads = {};
        std::array<ctx_t, thread_count> ctxs = {};

        for (int i = 0; i < thread_count; i++)
        {
            ctxs[i] = {.iterations = 5 * (i + 1), .result = 0};
            threads[i] = CreateThread(nullptr, 0, thread_proc, &ctxs[i], 0, nullptr);
            if (!threads[i])
            {
                return false;
            }
        }

        WaitForMultipleObjects(thread_count, threads.data(), TRUE, INFINITE);

        const std::array<int, thread_count> expected_results = {5, 10, 15, 20, 25};
        for (int i = 0; i < thread_count; i++)
        {
            if (ctxs[i].result != expected_results[i])
            {
                return false;
            }
            CloseHandle(threads[i]);
        }

        return true;
    }

    bool test_tls()
    {
        std::atomic_bool kill{false};
        std::atomic_uint32_t successes{0};
        constexpr uint32_t thread_count = 2;

        std::vector<std::thread> ts{};
        kill = false;

        ts.reserve(thread_count);
        for (size_t i = 0; i < thread_count; ++i)
        {
            ts.emplace_back([&] {
                while (!kill)
                {
                    std::this_thread::yield();
                }

                if (tls_var.num == GetCurrentThreadId())
                {
                    ++successes;
                }
            });
        }

        LoadLibraryA("d3dcompiler_47.dll");
        LoadLibraryA("dsound.dll");
        LoadLibraryA("comctl32.dll");
        /*LoadLibraryA("d3d9.dll");
        LoadLibraryA("dxgi.dll");
        LoadLibraryA("wlanapi.dll");*/

        kill = true;

        for (auto& t : ts)
        {
            if (t.joinable())
            {
                t.join();
            }
        }

        return successes == thread_count;
    }

    bool test_env()
    {
        const auto computername = read_env("COMPUTERNAME");

        SetEnvironmentVariableA("BLUB", "LUL");

        const auto blub = read_env("BLUB");

        return !computername.empty() && blub == "LUL";
    }

    bool test_file_path_io(const std::filesystem::path& filename)
    {
        std::error_code ec{};
        const auto absolute_file = absolute(filename, ec);
        (void)absolute_file;

        if (ec)
        {
            puts("Getting absolute path failed");
            return false;
        }

        const auto canonical_file = canonical(filename, ec);
        (void)canonical_file;

        if (ec)
        {
            puts("Getting canonical path failed");
            return false;
        }

        return true;
    }

    bool test_io()
    {
        const std::filesystem::path filename1 = "a.txt";
        const std::filesystem::path filename2 = "A.tXt";

        FILE* fp{};
        (void)fopen_s(&fp, filename1.string().c_str(), "wb");

        if (!fp)
        {
            puts("Bad file");
            return false;
        }

        const std::string text = "Blub";

        (void)fwrite(text.data(), 1, text.size(), fp);
        (void)fclose(fp);

        if (!test_file_path_io(filename1))
        {
            return false;
        }

        std::ifstream t(filename2);
        t.seekg(0, std::ios::end);
        const size_t size = static_cast<size_t>(t.tellg());
        std::string buffer(size, ' ');
        t.seekg(0);
        t.read(buffer.data(), static_cast<std::streamsize>(size));

        return text == buffer;
    }

    bool test_file_locking()
    {
        const auto filename = std::filesystem::absolute("a.txt");
        constexpr DWORD pending_byte = 0x40000000UL;

        const auto cleanup_file = utils::finally([&] { DeleteFileW(filename.c_str()); });

        HANDLE first = CreateFileW(filename.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (first == INVALID_HANDLE_VALUE)
        {
            puts("Failed to create first lock handle");
            return false;
        }

        const auto cleanup_first = utils::finally([&] { CloseHandle(first); });

        OVERLAPPED first_lock{};
        first_lock.Offset = pending_byte;
        if (!LockFileEx(first, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &first_lock))
        {
            puts("Failed to acquire first file lock");
            return false;
        }

        if (!UnlockFileEx(first, 0, 1, 0, &first_lock))
        {
            puts("Failed to unlock first file lock");
            return false;
        }

        OVERLAPPED second_lock{};
        second_lock.Offset = pending_byte + 1;
        if (!LockFileEx(first, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &second_lock))
        {
            puts("Failed to reacquire file lock");
            return false;
        }

        if (!UnlockFileEx(first, 0, 1, 0, &second_lock))
        {
            puts("Failed to unlock reacquired file lock");
            return false;
        }

        return true;
    }

    bool test_working_directory()
    {
        std::error_code ec{};

        const auto current_dir = std::filesystem::current_path(ec);
        if (ec)
        {
            puts("Failed to get current path");
            return false;
        }

        const std::filesystem::path sys32 = "C:/windows/system32";
        current_path(sys32, ec);

        if (ec)
        {
            puts("Failed to update working directory");
            return false;
        }

        const auto new_current_dir = std::filesystem::current_path();
        if (sys32 != new_current_dir)
        {
            puts("Updated directory is wrong!");
            return false;
        }

        if (!std::ifstream("ntdll.dll"))
        {
            puts("Working directory is not active!");
            return false;
        }

        current_path(current_dir);
        return std::filesystem::current_path() == current_dir;
    }

    bool test_dir_io()
    {
        size_t count = 0;

        for (auto i : std::filesystem::directory_iterator(R"(C:\Windows\System32\)"))
        {
            ++count;
            if (count > 30)
            {
                return true;
            }
        }

        return count > 30;
    }

    std::optional<std::string> read_registry_string(const HKEY root, const char* path, const char* value)
    {
        HKEY key{};
        if (RegOpenKeyExA(root, path, 0, KEY_READ, &key) != ERROR_SUCCESS)
        {
            return std::nullopt;
        }

        std::array<char, MAX_PATH> data{};
        auto length = static_cast<DWORD>(data.size());
        const auto res = RegQueryValueExA(key, value, nullptr, nullptr, reinterpret_cast<uint8_t*>(data.data()), &length);

        if (RegCloseKey(key) != ERROR_SUCCESS)
        {
            return std::nullopt;
        }

        if (res != ERROR_SUCCESS)
        {
            return std::nullopt;
        }

        if (length == 0)
        {
            return "";
        }

        return {std::string(data.data(), std::min(static_cast<size_t>(length - 1), data.size()))};
    }

    std::optional<std::vector<std::string>> get_all_registry_keys(const HKEY root, const char* path)
    {
        HKEY key{};
        if (RegOpenKeyExA(root, path, 0, KEY_READ | KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        {
            return std::nullopt;
        }

        std::vector<std::string> keys;
        std::vector<char> name_buffer(MAX_PATH + 1);

        for (DWORD i = 0;; ++i)
        {
            auto name_buffer_len = static_cast<DWORD>(name_buffer.size());
            const LSTATUS status = RegEnumKeyExA(key, i, name_buffer.data(), &name_buffer_len, nullptr, nullptr, nullptr, nullptr);
            if (status == ERROR_SUCCESS)
            {
                keys.emplace_back(name_buffer.data(), name_buffer_len);
            }
            else if (status == ERROR_NO_MORE_ITEMS)
            {
                break;
            }
            else
            {
                keys.clear();
                break;
            }
        }

        if (keys.empty())
        {
            RegCloseKey(key);
            return std::nullopt;
        }

        if (RegCloseKey(key) != ERROR_SUCCESS)
        {
            return std::nullopt;
        }

        return keys;
    }

    std::optional<std::vector<std::string>> get_all_registry_values(const HKEY root, const char* path)
    {
        HKEY key{};
        if (RegOpenKeyExA(root, path, 0, KEY_READ | KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        {
            return std::nullopt;
        }

        std::vector<std::string> values;
        std::vector<char> name_buffer(MAX_PATH + 1);

        for (DWORD i = 0;; ++i)
        {
            auto name_buffer_len = static_cast<DWORD>(name_buffer.size());
            const auto status = RegEnumValueA(key, i, name_buffer.data(), &name_buffer_len, nullptr, nullptr, nullptr, nullptr);
            if (status == ERROR_SUCCESS)
            {
                values.emplace_back(name_buffer.data(), name_buffer_len);
            }
            else if (status == ERROR_NO_MORE_ITEMS)
            {
                break;
            }
            else
            {
                values.clear();
                break;
            }
        }

        if (values.empty())
        {
            RegCloseKey(key);
            return std::nullopt;
        }

        if (RegCloseKey(key) != ERROR_SUCCESS)
        {
            return std::nullopt;
        }

        return values;
    }

    bool test_registry()
    {
#ifdef _WIN64
        const std::string_view progDir = "C:\\Program Files";
#else
        const std::string_view progDir = "C:\\Program Files (x86)";
#endif

        // Basic Reading Test
        const auto prog_files_dir =
            read_registry_string(HKEY_LOCAL_MACHINE, R"(SOFTWARE\Microsoft\Windows\CurrentVersion)", "ProgramFilesDir");
        if (!prog_files_dir || *prog_files_dir != progDir)
        {
            return false;
        }

        // WOW64 Redirection Test
        const auto pst_display = read_registry_string(
            HKEY_LOCAL_MACHINE, R"(SOFTWARE\WOW6432Node\Microsoft\Windows NT\CurrentVersion\Time Zones\Pacific Standard Time)", "Display");
        if (!pst_display || pst_display->empty())
        {
            return false;
        }

        // Key Sub-keys Enumeration Test
        const auto subkeys_opt = get_all_registry_keys(HKEY_LOCAL_MACHINE, R"(SOFTWARE\Microsoft\Windows NT\CurrentVersion)");
        if (!subkeys_opt)
        {
            return false;
        }

        bool found_fonts = false;
        for (const auto& key_name : *subkeys_opt)
        {
            if (key_name == "Fonts")
            {
                found_fonts = true;
                break;
            }
        }

        (void)found_fonts;
#ifdef _WIN64
        if (!found_fonts)
        {
            return false;
        }
#endif

        // Key Values Enumeration Test
        const auto values_opt = get_all_registry_values(HKEY_LOCAL_MACHINE, R"(SOFTWARE\Microsoft\Windows NT\CurrentVersion)");
        if (!values_opt)
        {
            return false;
        }

        bool found_product_name = false;
        for (const auto& val_name : *values_opt)
        {
            if (val_name == "ProductName")
            {
                found_product_name = true;
                break;
            }
        }
        if (!found_product_name)
        {
            return false;
        }

        return true;
    }

    bool test_system_info()
    {
        std::array<char, MAX_PATH> sys_dir{};
        if (GetSystemDirectoryA(sys_dir.data(), static_cast<DWORD>(sys_dir.size())) == 0)
        {
            return false;
        }
        if (strlen(sys_dir.data()) != 19)
        {
            return false;
        }

        // TODO: This currently doesn't work.
        /*
        char username[256];
        DWORD username_len = sizeof(username);
        if (!GetUserNameA(username, &username_len))
        {
            return false;
        }
        if (username_len <= 1)
        {
            return false;
        }
        */

        return true;
    }

    bool validate_primary_monitor(MONITORINFOEXA& mi)
    {
        if (std::string_view(mi.szDevice) != R"(\\.\DISPLAY1)")
        {
            return false;
        }

        if (mi.rcMonitor.left != 0 || mi.rcMonitor.top != 0 || mi.rcMonitor.right != 1920 || mi.rcMonitor.bottom != 1080)
        {
            return false;
        }

        if (!(mi.dwFlags & MONITORINFOF_PRIMARY))
        {
            return false;
        }

        return true;
    }

    bool test_monitor_info()
    {
        const POINT pt = {0, 0};
        auto* const hMonitor = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
        if (!hMonitor)
        {
            return false;
        }

        MONITORINFOEXA mi;
        mi.cbSize = sizeof(mi);

        if (!GetMonitorInfoA(hMonitor, &mi))
        {
            return false;
        }

        return validate_primary_monitor(mi);
    }

    BOOL CALLBACK monitor_enum_proc(HMONITOR hMonitor, HDC, LPRECT, LPARAM dwData)
    {
        auto* valid = reinterpret_cast<bool*>(dwData);

        MONITORINFOEXA mi;
        mi.cbSize = sizeof(mi);

        if (!GetMonitorInfoA(hMonitor, &mi))
        {
            return FALSE;
        }

        *valid = validate_primary_monitor(mi);

        return *valid ? TRUE : FALSE;
    }

    bool test_user_callback()
    {
        bool valid = false;
        if (!EnumDisplayMonitors(nullptr, nullptr, monitor_enum_proc, reinterpret_cast<LPARAM>(&valid)))
        {
            return false;
        }

        return valid;
    }

    bool test_time_zone()
    {
        DYNAMIC_TIME_ZONE_INFORMATION current_dtzi = {};
        DWORD result = GetDynamicTimeZoneInformation(&current_dtzi);

        if (result == TIME_ZONE_ID_INVALID)
        {
            return false;
        }

        if (current_dtzi.Bias != -60 || current_dtzi.StandardBias != 0 || current_dtzi.DaylightBias != -60 ||
            current_dtzi.DynamicDaylightTimeDisabled != FALSE)
        {
            return false;
        }

        if (wcscmp(current_dtzi.StandardName, L"W. Europe Standard Time") != 0 ||
            wcscmp(current_dtzi.DaylightName, L"W. Europe Daylight Time") != 0 ||
            wcscmp(current_dtzi.TimeZoneKeyName, L"W. Europe Standard Time") != 0)
        {
            return false;
        }

        if (current_dtzi.StandardDate.wYear != 0 || current_dtzi.StandardDate.wMonth != 10 || current_dtzi.StandardDate.wDayOfWeek != 0 ||
            current_dtzi.StandardDate.wDay != 5 || current_dtzi.StandardDate.wHour != 3 || current_dtzi.StandardDate.wMinute != 0 ||
            current_dtzi.StandardDate.wSecond != 0 || current_dtzi.StandardDate.wMilliseconds != 0)
        {
            return false;
        }

        if (current_dtzi.DaylightDate.wYear != 0 || current_dtzi.DaylightDate.wMonth != 3 || current_dtzi.DaylightDate.wDayOfWeek != 0 ||
            current_dtzi.DaylightDate.wDay != 5 || current_dtzi.DaylightDate.wHour != 2 || current_dtzi.DaylightDate.wMinute != 0 ||
            current_dtzi.DaylightDate.wSecond != 0 || current_dtzi.DaylightDate.wMilliseconds != 0)
        {
            return false;
        }

        return true;
    }

    void throw_exception()
    {
        if (do_the_task)
        {
            throw std::runtime_error("OK");
        }
    }

    bool test_exceptions()
    {
        try
        {
            throw_exception();
            return false;
        }
        catch (const std::exception& e)
        {
            return e.what() == std::string("OK");
        }
    }

    struct wsa_initializer
    {
        wsa_initializer()
        {
            WSADATA wsa_data;
            if (WSAStartup(MAKEWORD(2, 2), &wsa_data))
            {
                throw std::runtime_error("Unable to initialize WSA");
            }
        }

        ~wsa_initializer()
        {
            WSACleanup();
        }
    };

    bool test_dns()
    {
        wsa_initializer _{};
        constexpr auto hostname = "example.com";

        PDNS_RECORDA records = nullptr;
        const auto query_status = DnsQuery_A(hostname, DNS_TYPE_A, DNS_QUERY_STANDARD, nullptr, &records, nullptr);
        if (query_status != ERROR_SUCCESS)
        {
            puts("DnsQuery_A failed");
            return false;
        }

        const auto free_records = utils::finally([&] {
            if (records)
            {
                DnsRecordListFree(records, DnsFreeRecordList);
            }
        });

        auto has_ipv4_record = false;
        for (auto* current = records; current != nullptr; current = current->pNext)
        {
            if (current->wType == DNS_TYPE_A)
            {
                has_ipv4_record = true;
                break;
            }
        }

        if (!has_ipv4_record)
        {
            puts("DnsQuery_A returned no A records");
            return false;
        }

        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        addrinfo* results = nullptr;
        const auto resolve_status = getaddrinfo(hostname, "80", &hints, &results);
        if (resolve_status != 0)
        {
            puts("getaddrinfo failed");
            return false;
        }

        const auto free_results = utils::finally([&] {
            if (results)
            {
                freeaddrinfo(results);
            }
        });

        for (auto* current = results; current != nullptr; current = current->ai_next)
        {
            if (current->ai_family == AF_INET || current->ai_family == AF_INET6)
            {
                return true;
            }
        }

        puts("getaddrinfo returned no usable addresses");
        return false;
    }

    bool test_socket()
    {
        wsa_initializer _{};
        constexpr std::string_view send_data = "Hello World";

        const auto sender = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        const auto receiver = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sender == INVALID_SOCKET || receiver == INVALID_SOCKET)
        {
            puts("Socket creation failed");
            return false;
        }

        sockaddr_in destination{};
        destination.sin_family = AF_INET;
        destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        destination.sin_port = htons(28970);

        if (bind(receiver, reinterpret_cast<sockaddr*>(&destination), sizeof(destination)) == SOCKET_ERROR)
        {
            puts("Failed to bind socket!");
            return false;
        }

        const auto sent_bytes = sendto(sender, send_data.data(), static_cast<int>(send_data.size()), 0,
                                       reinterpret_cast<sockaddr*>(&destination), sizeof(destination));

        if (static_cast<size_t>(sent_bytes) != send_data.size())
        {
            puts("Failed to send data!");
            return false;
        }

        std::array<char, 100> buffer = {};
        sockaddr_in sender_addr{};
        int sender_length = sizeof(sender_addr);

        const auto len = recvfrom(receiver, buffer.data(), static_cast<int>(buffer.size()), 0, reinterpret_cast<sockaddr*>(&sender_addr),
                                  &sender_length);
        const auto ulen = static_cast<size_t>(len);

        if (ulen != send_data.size())
        {
            puts("Failed to receive data!");
            return false;
        }

        return send_data == std::string_view(buffer.data(), ulen);
    }

#ifndef __MINGW64__
    void throw_access_violation()
    {
        if (do_the_task)
        {
            *reinterpret_cast<int*>(1) = 1;
        }
    }

    bool test_access_violation_exception()
    {
        __try
        {
            throw_access_violation();
            return false;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return GetExceptionCode() == STATUS_ACCESS_VIOLATION;
        }
    }

    bool test_ud2_exception(void* address)
    {
        __try
        {
            reinterpret_cast<void (*)()>(address)();
            return false;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return GetExceptionCode() == STATUS_ILLEGAL_INSTRUCTION;
        }
    }

    bool test_unhandled_exception()
    {
        thread_local bool caught{};
        caught = false;

        auto* old = SetUnhandledExceptionFilter([](struct _EXCEPTION_POINTERS* info) -> LONG {
            caught = true;

#ifdef _WIN64
            info->ContextRecord->Rip += 1;
#else
            info->ContextRecord->Eip += 1;
#endif

            return EXCEPTION_CONTINUE_EXECUTION;
        });

        DebugBreak();
        SetUnhandledExceptionFilter(old);

        return caught;
    }

    bool test_illegal_instruction_exception()
    {
        auto* const address = VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (!address)
        {
            return false;
        }

        memcpy(address, "\x0F\x0B", 2); // ud2

        const auto res = test_ud2_exception(address);

        VirtualFree(address, 0x1000, MEM_RELEASE);

        return res;
    }

    INT32 test_guard_page_seh_filter(LPVOID address, DWORD code, struct _EXCEPTION_POINTERS* ep)
    {
        // We are only looking for guard page exceptions.
        if (code != STATUS_GUARD_PAGE_VIOLATION)
        {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        // The number of defined elements in the ExceptionInformation array for
        // a guard page violation should be 2.
        if (ep->ExceptionRecord->NumberParameters != 2)
        {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        // The ExceptionInformation array specifies additional arguments that
        // describe the exception.
        auto* exception_information = ep->ExceptionRecord->ExceptionInformation;

        // If this value is zero, the thread attempted to read the inaccessible
        // data. If this value is 1, the thread attempted to write to an
        // inaccessible address.
        if (exception_information[0] != 1)
        {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        // The second array element specifies the virtual address of the
        // inaccessible data.
        if (exception_information[1] != reinterpret_cast<ULONG_PTR>(address))
        {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        return EXCEPTION_EXECUTE_HANDLER;
    }

    bool test_guard_page_exception()
    {
        SYSTEM_INFO sys_info;
        GetSystemInfo(&sys_info);

        // Allocate a guarded memory region with the length of the system page
        // size.
        auto* addr = static_cast<LPBYTE>(VirtualAlloc(nullptr, sys_info.dwPageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE | PAGE_GUARD));
        if (addr == nullptr)
        {
            puts("Failed to allocate guard page");
            return false;
        }

        bool success = false;

        // We want to access some arbitrary offset into the guarded page, to
        // ensure that ExceptionInformation correctly contains the virtual
        // address of the inaccessible data, not the base address of the region.
        constexpr size_t offset = 10;

        // Trigger a guard page violation
        __try
        {
            addr[offset] = 255;
        }
        // If the filter function returns EXCEPTION_CONTINUE_SEARCH, the
        // exception contains all of the correct information.
        __except (test_guard_page_seh_filter(addr + offset, GetExceptionCode(), GetExceptionInformation()))
        {
            success = true;
        }

        // The page guard should be lifted, so no exception should be raised.
        __try
        {
            // The previous write should not have went through, this is probably
            // superflous.
            if (addr[offset] == 255)
            {
                success = false;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            puts("Failed to read from page after guard exception!");
            success = false;
        }

        // Free the allocated memory
        if (!VirtualFree(addr, 0, MEM_RELEASE))
        {
            puts("Failed to free allocated region");
            success = false;
        }

        return success;
    }

    bool test_native_exceptions()
    {
        return test_access_violation_exception()       //
               && test_illegal_instruction_exception() //
               && test_unhandled_exception()           //
#ifdef _WIN64
               && test_guard_page_exception();
#else
            ;
#endif
    }
#endif

    thread_local bool trap_flag_cleared = false;
    constexpr DWORD TRAP_FLAG_MASK = 0x100;

    LONG NTAPI single_step_handler(PEXCEPTION_POINTERS exception_info)
    {
        if (exception_info->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP)
        {
            PCONTEXT context = exception_info->ContextRecord;
            trap_flag_cleared = (context->EFlags & TRAP_FLAG_MASK) == 0;
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        return EXCEPTION_CONTINUE_SEARCH;
    }

    bool test_interrupts()
    {
        trap_flag_cleared = false;

        PVOID veh_handle = AddVectoredExceptionHandler(1, single_step_handler);
        if (!veh_handle)
        {
            return false;
        }

        __writeeflags(__readeflags() | TRAP_FLAG_MASK);

#ifdef __MINGW64__
        asm("nop");
#else
        __nop();
#endif

        RemoveVectoredExceptionHandler(veh_handle);

        return trap_flag_cleared;
    }

    void print_time()
    {
        const auto epoch_time = std::chrono::system_clock::now().time_since_epoch();
        printf("Time: %" PRId64 "\n", std::chrono::duration_cast<std::chrono::nanoseconds>(epoch_time).count());
    }

    bool test_apis()
    {
        if (VirtualProtect(nullptr, 0, 0, nullptr))
        {
            return false;
        }

        std::array<wchar_t, 0x100> buffer{};
        auto size = static_cast<DWORD>(buffer.size() / 2);
        if (!GetComputerNameExW(ComputerNameNetBIOS, buffer.data(), &size))
        {
            return false;
        }

        PWSTR path{};
        const auto hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path);
        if (FAILED(hr))
        {
            return false;
        }

        CoTaskMemFree(path);
        return true;
    }

    bool test_apc()
    {
        int executions = 0;

        PAPCFUNC apc_func = [](const ULONG_PTR param) {
            *reinterpret_cast<int*>(param) += 1; //
        };

        QueueUserAPC(apc_func, GetCurrentThread(), reinterpret_cast<ULONG_PTR>(&executions));
        QueueUserAPC(apc_func, GetCurrentThread(), reinterpret_cast<ULONG_PTR>(&executions));

        Sleep(1);

        if (executions != 0)
        {
            return false;
        }

        SleepEx(1, TRUE);
        return executions == 2;
    }

    bool test_message_queue()
    {
        static thread_local UINT wnd_proc_num = 0;
        static const UINT wnd_msg_id = WM_APP + 2;

        WNDCLASSEXA wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpszClassName = "TestMsgQueueClass";
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
            if (msg == WM_CREATE)
            {
                auto* const cs = reinterpret_cast<CREATESTRUCTA*>(lp);
                if (cs->lpCreateParams == reinterpret_cast<void*>(0x1337))
                {
                    wnd_proc_num += 1;
                }
            }
            else if (msg == wnd_msg_id)
            {
                if (wp == 123 && lp == 456)
                {
                    wnd_proc_num += 1;
                    return 777;
                }
            }
            return DefWindowProcA(hwnd, msg, wp, lp);
        };

        if (!RegisterClassExA(&wc))
        {
            puts("Failed to register window class");
            return false;
        }

        HWND hwnd = CreateWindowExA(0, wc.lpszClassName, nullptr, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance,
                                    reinterpret_cast<void*>(0x1337));
        if (!hwnd || wnd_proc_num != 1)
        {
            puts("Failed to create message window");
            UnregisterClassA(wc.lpszClassName, wc.hInstance);
            return false;
        }

        const LRESULT send_res = SendMessageA(hwnd, wnd_msg_id, 123, 456);

        if (send_res != 777 || wnd_proc_num != 2)
        {
            puts("SendMessage failed");
            DestroyWindow(hwnd);
            UnregisterClassA(wc.lpszClassName, wc.hInstance);
            return false;
        }

        wnd_proc_num = 0;
        if (!PostMessageA(hwnd, wnd_msg_id, 123, 456))
        {
            puts("PostMessage failed");
            DestroyWindow(hwnd);
            UnregisterClassA(wc.lpszClassName, wc.hInstance);
            return false;
        }

        MSG msg = {};
        if (GetMessageA(&msg, hwnd, 0, 0) <= 0)
        {
            puts("GetMessage failed or returned WM_QUIT unexpectedly");
            DestroyWindow(hwnd);
            UnregisterClassA(wc.lpszClassName, wc.hInstance);
            return false;
        }

        if (msg.message != wnd_msg_id)
        {
            puts("Retrieved message is not the expected custom message");
            DestroyWindow(hwnd);
            UnregisterClassA(wc.lpszClassName, wc.hInstance);
            return false;
        }

        TranslateMessage(&msg);
        DispatchMessageA(&msg);

        if (wnd_proc_num != 1)
        {
            puts("Posted window message did not execute WndProc");
            DestroyWindow(hwnd);
            UnregisterClassA(wc.lpszClassName, wc.hInstance);
            return false;
        }

        constexpr int quit_code = 42;
        PostQuitMessage(quit_code);

        const BOOL quit_result = GetMessageA(&msg, nullptr, 0, 0);
        if (quit_result != 0)
        {
            puts("GetMessage did not return 0 for WM_QUIT");
            DestroyWindow(hwnd);
            UnregisterClassA(wc.lpszClassName, wc.hInstance);
            return false;
        }

        if (msg.message != WM_QUIT)
        {
            puts("Message is not WM_QUIT");
            DestroyWindow(hwnd);
            UnregisterClassA(wc.lpszClassName, wc.hInstance);
            return false;
        }

        if (msg.wParam != quit_code)
        {
            puts("WM_QUIT exit code mismatch");
            DestroyWindow(hwnd);
            UnregisterClassA(wc.lpszClassName, wc.hInstance);
            return false;
        }

        DestroyWindow(hwnd);
        UnregisterClassA(wc.lpszClassName, wc.hInstance);
        return true;
    }

    bool test_private_namespace()
    {
        auto create_boundary_descriptor = [](const wchar_t* name) -> HANDLE {
            auto* hBoundaryDescriptor = CreateBoundaryDescriptorW(name, 0);
            if (hBoundaryDescriptor == nullptr)
            {
                puts("Failed to create boundary descriptor");
                return nullptr;
            }

            PSID pLocalAdmin{};
            if (ConvertStringSidToSidW(L"S-1-1-0", &pLocalAdmin) == FALSE)
            {
                puts("ConvertStringSidToSid failed");
                return nullptr;
            }

            auto res = AddSIDToBoundaryDescriptor(&hBoundaryDescriptor, pLocalAdmin);
            LocalFree(pLocalAdmin);

            if (res == FALSE)
            {
                puts("AddSIDToBoundaryDescriptor failed");
                return nullptr;
            }

            return hBoundaryDescriptor;
        };

        std::array<HANDLE, 2> boundary{};
        std::array<HANDLE, 5> ns{};

        const auto _ = utils::finally([&]() {
            for (auto* elem : boundary)
            {
                if (elem)
                {
                    DeleteBoundaryDescriptor(elem);
                }
            }

            for (auto* elem : ns)
            {
                if (elem)
                {
                    ClosePrivateNamespace(elem, 0);
                }
            }
        });

        boundary[0] = create_boundary_descriptor(L"boundary1");
        if (boundary[0] == nullptr)
        {
            return false;
        }

        ns[0] = CreatePrivateNamespaceW(nullptr, boundary[0], L"ns");
        if (ns[0] == nullptr)
        {
            puts("CreatePrivateNamespaceW failed");
            return false;
        }

        ns[1] = CreatePrivateNamespaceW(nullptr, boundary[0], L"alt_ns");
        if (ns[1] != nullptr)
        {
            puts("CreatePrivateNamespaceW did not refuse to associate another prefix with existing namespace");
            return false;
        }

        if (GetLastError() != ERROR_ALREADY_EXISTS)
        {
            puts("GetLastError did not return ERROR_ALREADY_EXISTS");
            return false;
        }

        boundary[1] = create_boundary_descriptor(L"boundary2");
        if (boundary[1] == nullptr)
        {
            return false;
        }

        ns[2] = CreatePrivateNamespaceW(nullptr, boundary[1], L"ns");
        if (ns[2] != nullptr)
        {
            puts("CreatePrivateNamespaceW did not refuse to create another namespace associated with existing prefix");
            return false;
        }

        if (GetLastError() != ERROR_DUP_NAME)
        {
            puts("GetLastError did not return ERROR_DUP_NAME");
            return false;
        }

        auto* mutex = CreateMutexW(nullptr, FALSE, L"ns\\mutex");
        if (mutex == nullptr)
        {
            puts("CreateMutex failed to create mutex in private namespace");
            return false;
        }

        CloseHandle(mutex);

        ns[3] = OpenPrivateNamespaceW(boundary[0], L"alt_ns");
        if (ns[3] == nullptr)
        {
            puts("OpenPrivateNamespaceW failed to open existing namespace and associate it with different prefix");
            return false;
        }

        ns[4] = OpenPrivateNamespaceW(boundary[0], L"ns");
        if (ns[4])
        {
            puts("OpenPrivateNamespaceW did not refuse to open existing namespace and associate it with existing prefix");
            return false;
        }

        DeleteBoundaryDescriptor(boundary[0]);
        boundary[0] = nullptr;

        if (!ClosePrivateNamespace(ns[3], 0))
        {
            puts("ClosePrivateNamespace failed");
            return false;
        }

        ns[3] = nullptr;

        if (!ClosePrivateNamespace(ns[0], PRIVATE_NAMESPACE_FLAG_DESTROY))
        {
            puts("ClosePrivateNamespace (with destroy flag) failed");
            return false;
        }

        ns[0] = nullptr;

        return true;
    }
}

#define RUN_TEST(func, name)                 \
    {                                        \
        printf("Running test '" name "': "); \
        const auto res = func();             \
        valid &= res;                        \
        puts(res ? "Success" : "Fail");      \
    }

int main(const int argc, const char* argv[])
{
    if (argc == 2 && argv[1] == "-time"sv)
    {
        print_time();
        return 0;
    }

    bool valid = true;

    RUN_TEST(test_io, "I/O")
    RUN_TEST(test_file_locking, "File Locking")
    RUN_TEST(test_dir_io, "Dir I/O")
    RUN_TEST(test_apis, "APIs")
#ifdef _WIN64
    RUN_TEST(test_working_directory, "Working Directory")
#endif
    RUN_TEST(test_registry, "Registry")
    RUN_TEST(test_system_info, "System Info")
    RUN_TEST(test_monitor_info, "Monitor Info")
    RUN_TEST(test_time_zone, "Time Zone")
    RUN_TEST(test_threads, "Threads")
    RUN_TEST(test_threads_winapi, "Threads WinAPI")
    RUN_TEST(test_env, "Environment")
    RUN_TEST(test_exceptions, "Exceptions")
#ifndef __MINGW64__
    RUN_TEST(test_native_exceptions, "Native Exceptions")
#endif
#ifdef _WIN64
    if (!getenv("EMULATOR_ICICLE"))
    {
        RUN_TEST(test_interrupts, "Interrupts")
    }
#endif
    RUN_TEST(test_tls, "TLS")
    RUN_TEST(test_socket, "Socket")
    RUN_TEST(test_dns, "DNS")
    RUN_TEST(test_apc, "APC")
    RUN_TEST(test_user_callback, "User Callback")
#ifdef _WIN64
    RUN_TEST(test_message_queue, "Message Queue")
#endif
    RUN_TEST(test_private_namespace, "Private Namespace")

    return valid ? 0 : 1;
}
