#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace iat
{

    struct Patch
    {
        void** slot = nullptr;
        void* previous = nullptr;
    };

    struct HookResult
    {
        void* callable_original = nullptr;
        std::vector<Patch> patches;
        std::vector<std::string> diagnostics;
        unsigned normal_matches = 0;
        unsigned delay_matches = 0;
        bool saw_matching_normal_descriptor = false;
        bool saw_matching_delay_descriptor = false;

        [[nodiscard]] bool ok() const noexcept
        {
            return callable_original != nullptr && !patches.empty();
        }
    };

    // Replaces every matching normal and delay-import IAT slot in module. For a
    // delay import, callable_original is resolved directly from importedDll so the
    // hook never calls the delay-loader thunk as if it were the native function.
    HookResult HookImport(HMODULE module, const char* importedDll, const char* importedFunction, void* replacement);

    bool RestoreImports(HookResult& result, std::string* error = nullptr) noexcept;

} // namespace iat
