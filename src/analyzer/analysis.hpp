#pragma once

#include <set>
#include <string>

struct mapped_module;
class module_manager;
class windows_emulator;

using string_set = std::set<std::string, std::less<>>;

struct analysis_settings
{
    bool concise_logging{false};
    bool verbose_logging{false};
    bool silent{false};
    bool buffer_stdout{false};

    string_set modules{};
    string_set ignored_functions{};
};

struct accessed_import
{
    uint64_t address{};
    uint32_t thread_id{};
    uint64_t access_rip{};
    uint64_t access_inst_count{};
    std::string accessor_module{};
    std::string import_name{};
    std::string import_module{};
};

struct analysis_context
{
    const analysis_settings* settings{};
    windows_emulator* win_emu{};

    std::string output{};
    bool has_reached_main{false};

    std::vector<accessed_import> accessed_imports{};
};

inline void add_functions_with_arity(std::unordered_map<std::string_view, size_t>& map, int arity,
                                     std::initializer_list<std::string_view> names)
{
    for (auto name : names)
        map[name] = arity;
}

inline std::unordered_map<std::string_view, size_t> function_argument_count = [] {
    std::unordered_map<std::string_view, size_t> map;

    add_functions_with_arity(map, 1,
                             {"LoadLibraryA", "LoadLibraryW", "LoadLibrary", "GetModuleHandleA", "GetModuleHandleW",
                              "GetModuleHandle", "FreeLibrary"});

    add_functions_with_arity(map, 2,
                             {"lstrcmpA", "lstrcmpW", "lstrcmpiA", "lstrcmpiW", "lstrcmp", "lstrcmpi", "SetWindowTextA",
                              "SetWindowTextW", "GetWindowTextA", "GetWindowTextW", "GetProcAddress", "FindNextFileA",
                              "FindNextFileW", "SetWindowText", "GetWindowText", "FindNextFile"});

    add_functions_with_arity(map, 3,
                             {"GetEnvironmentVariableA", "GetEnvironmentVariableW", "ExpandEnvironmentStringsA",
                              "ExpandEnvironmentStringsW", "GetPrivateProfileStringA", "GetPrivateProfileStringW",
                              "WritePrivateProfileStringA", "WritePrivateProfileStringW", "ExpandEnvironmentStrings",
                              "GetEnvironmentVariable", "LoadLibraryExA", "LoadLibraryExW", "LoadLibraryEx",
                              "NtOpenKey"});

    add_functions_with_arity(map, 4,
                             {"MessageBoxA", "MessageBoxW", "CreateProcessA", "CreateProcessW", "ShellExecuteA",
                              "ShellExecuteW", "SendMessageA", "SendMessageW", "PostMessageA", "PostMessageW",
                              "MessageBox", "CreateProcess", "ShellExecute", "SendMessage", "PostMessage",
                              "LdrLoadDll"});

    add_functions_with_arity(map, 5,
                             {"CreateDirectoryExA", "CreateDirectoryExW", "CreateDirectoryEx", "NtProtectVirtualMemory",
                              "NtQueryInformationProcess"});

    add_functions_with_arity(map, 6, 
                             {"CreateProcessAsUserA", "CreateProcessAsUserW", "CreateProcessAsUser", "NtQueryValueKey"});

    add_functions_with_arity(map, 11, {"NtCreateUserProcess"});

    return map;
}();

inline bool is_ambiguous_string_function(std::string_view name)
{
    static const std::unordered_set<std::string_view> ambiguous = {
        "lstrcmp",
        "lstrcmpi",
        "LoadLibrary",
        "ExpandEnvironmentStrings",
        "GetEnvironmentVariable",
        "CreateFile",
        "DeleteFile",
        "FindFirstFile",
        "FindNextFile",
        "GetFileAttributes",
        "MoveFile",
        "CopyFile",
        "MessageBox",
        "SetWindowText",
        "SendMessage",
        "PostMessage",
        "CreateDirectory",
        "RemoveDirectory",
        "GetWindowText",
        "CreateProcess",
        "ShellExecute",
        "GetModuleHandle",
        "GetFullPathName"
        // Add more as needed
    };
        
    if (name.ends_with("Ex"))
    {
        name.remove_suffix(2);
    }

    if (name.ends_with("A") || name.ends_with("W"))
    {
        name.remove_suffix(1);
    }

    return ambiguous.contains(name);
}

inline const std::unordered_set<std::string_view> ansi_only_functions = {
    "GetProcAddress",   "RegisterClassA",           "AddAtomA",
    "GetCommandLineA",  "GetPrivateProfileStringA", "WritePrivateProfileStringA",
    "OpenFileMappingA", "CreateFileMappingA",       "lstrcpyA",
    "FreeLibrary"};

inline bool is_unicode_function(std::string_view name)
{
    if (ansi_only_functions.contains(name))
        return false;
    if (name.ends_with("W") || name.ends_with("ExW"))
        return true;
    if (name.ends_with("A") || name.ends_with("ExA"))
        return false;
    if (is_ambiguous_string_function(name))
        return true;
    return true; // default: Windows prefers Unicode
}

inline const std::unordered_set<std::string> kOsMods = {
    "ntdll.dll",        "kernel32.dll",       "kernelbase.dll", "user32.dll",   "gdi32.dll",     "advapi32.dll",
    "ws2_32.dll",       "wininet.dll",        "winhttp.dll",    "iphlpapi.dll", "rpcrt4.dll",    "sechost.dll",
    "bcrypt.dll",       "crypt32.dll",        "combase.dll",    "ole32.dll",    "oleaut32.dll",  "shell32.dll",
    "shlwapi.dll",      "psapi.dll",          "version.dll",    "dbghelp.dll",  "msvcp_win.dll", "ucrtbase.dll",
    "vcruntime140.dll", "vcruntime140_1.dll", "msvcrt.dll",     "win32u.dll"};

void register_analysis_callbacks(analysis_context& c);
mapped_module* get_module_if_interesting(module_manager& manager, const string_set& modules, uint64_t address);
