#include "iat_hook.hpp"

#include <delayimp.h>

#include <cstddef>
#include <cstring>
#include <sstream>

namespace iat {
namespace {

struct ImageView {
    unsigned char* base = nullptr;
    size_t size = 0;

    template <typename T>
    T* rva(DWORD value, size_t count = 1) const noexcept {
        if (value == 0 || count > (size / sizeof(T))) {
            return nullptr;
        }
        const size_t bytes = count * sizeof(T);
        if (value > size || bytes > size - value) {
            return nullptr;
        }
        return reinterpret_cast<T*>(base + value);
    }

    const char* stringRva(DWORD value) const noexcept {
        const char* p = rva<char>(value);
        if (!p) return nullptr;
        const size_t remaining = size - static_cast<size_t>(
            reinterpret_cast<const unsigned char*>(p) - base);
        return std::memchr(p, '\0', remaining) ? p : nullptr;
    }
};

bool BuildImageView(HMODULE module, ImageView& out, std::string& error) {
    if (!module) {
        error = "module handle is null";
        return false;
    }
    auto* base = reinterpret_cast<unsigned char*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
        error = "invalid DOS header";
        return false;
    }
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        error = "invalid or non-x64 NT headers";
        return false;
    }
    out.base = base;
    out.size = nt->OptionalHeader.SizeOfImage;
    if (out.size < sizeof(IMAGE_DOS_HEADER)) {
        error = "invalid image size";
        return false;
    }
    return true;
}

IMAGE_NT_HEADERS64* NtHeaders(const ImageView& image) noexcept {
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image.base);
    return reinterpret_cast<IMAGE_NT_HEADERS64*>(image.base + dos->e_lfanew);
}

bool PatchSlot(void** slot, void* replacement, Patch& patch, std::string& error) {
    if (!slot || !replacement) {
        error = "invalid IAT slot or replacement address";
        return false;
    }
    DWORD oldProtect = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        std::ostringstream os;
        os << "VirtualProtect(IAT writable) failed, Win32 error " << GetLastError();
        error = os.str();
        return false;
    }
    patch.slot = slot;
    patch.previous = InterlockedExchangePointer(
        reinterpret_cast<void* volatile*>(slot), replacement);
    DWORD ignored = 0;
    const BOOL restored = VirtualProtect(slot, sizeof(void*), oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
    if (!restored) {
        std::ostringstream os;
        os << "IAT was changed, but restoring its page protection failed, Win32 error "
           << GetLastError();
        error = os.str();
        return false;
    }
    return true;
}

void RollBack(std::vector<Patch>& patches) noexcept {
    for (auto it = patches.rbegin(); it != patches.rend(); ++it) {
        DWORD oldProtect = 0;
        if (it->slot && VirtualProtect(it->slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
            InterlockedExchangePointer(reinterpret_cast<void* volatile*>(it->slot), it->previous);
            DWORD ignored = 0;
            VirtualProtect(it->slot, sizeof(void*), oldProtect, &ignored);
        }
    }
    patches.clear();
}

bool IsMatchingNameThunk(ULONGLONG thunkValue,
                         const ImageView& image,
                         const char* requested) noexcept {
    if (IMAGE_SNAP_BY_ORDINAL64(thunkValue)) return false;
    if (thunkValue > MAXDWORD) return false;
    auto* ibn = image.rva<IMAGE_IMPORT_BY_NAME>(static_cast<DWORD>(thunkValue));
    if (!ibn) return false;
    const auto nameRva = static_cast<DWORD>(thunkValue) +
                         static_cast<DWORD>(offsetof(IMAGE_IMPORT_BY_NAME, Name));
    const char* name = image.stringRva(nameRva);
    return name && std::strcmp(name, requested) == 0;
}

}  // namespace

HookResult HookImport(HMODULE module,
                      const char* importedDll,
                      const char* importedFunction,
                      void* replacement) {
    HookResult result;
    if (!importedDll || !importedFunction || !replacement) {
        result.diagnostics.emplace_back("invalid HookImport argument");
        return result;
    }

    ImageView image;
    std::string error;
    if (!BuildImageView(module, image, error)) {
        result.diagnostics.push_back(error);
        return result;
    }
    auto* nt = NtHeaders(image);

    const auto importDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (importDir.VirtualAddress && importDir.Size >= sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
        auto* desc = image.rva<IMAGE_IMPORT_DESCRIPTOR>(importDir.VirtualAddress);
        const size_t maxDesc = importDir.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);
        if (!desc) {
            result.diagnostics.emplace_back("normal import directory is out of image bounds");
        } else {
            for (size_t d = 0; d < maxDesc && desc[d].Name; ++d) {
                const char* dllName = image.stringRva(desc[d].Name);
                if (!dllName || _stricmp(dllName, importedDll) != 0) continue;
                result.saw_matching_normal_descriptor = true;
                const DWORD lookupRva = desc[d].OriginalFirstThunk;
                if (!lookupRva) {
                    result.diagnostics.emplace_back(
                        "matching normal import descriptor has no OriginalFirstThunk; "
                        "names cannot be matched safely");
                    continue;
                }
                auto* lookup = image.rva<IMAGE_THUNK_DATA64>(lookupRva);
                auto* iatThunk = image.rva<IMAGE_THUNK_DATA64>(desc[d].FirstThunk);
                if (!lookup || !iatThunk) {
                    result.diagnostics.emplace_back("matching normal import thunk table is out of bounds");
                    continue;
                }
                const size_t lookupIndex = static_cast<size_t>(
                    reinterpret_cast<unsigned char*>(lookup) - image.base);
                const size_t iatIndex = static_cast<size_t>(
                    reinterpret_cast<unsigned char*>(iatThunk) - image.base);
                const size_t maxLookup = (image.size - lookupIndex) / sizeof(IMAGE_THUNK_DATA64);
                const size_t maxIat = (image.size - iatIndex) / sizeof(IMAGE_THUNK_DATA64);
                const size_t limit = (maxLookup < maxIat) ? maxLookup : maxIat;
                for (size_t i = 0; i < limit && lookup[i].u1.AddressOfData; ++i) {
                    if (!IsMatchingNameThunk(lookup[i].u1.AddressOfData, image, importedFunction)) continue;
                    Patch patch;
                    if (!PatchSlot(reinterpret_cast<void**>(&iatThunk[i].u1.Function),
                                   replacement, patch, error)) {
                        if (patch.slot) result.patches.push_back(patch);
                        result.diagnostics.push_back(error);
                        RollBack(result.patches);
                        result.callable_original = nullptr;
                        return result;
                    }
                    if (!result.callable_original) result.callable_original = patch.previous;
                    result.patches.push_back(patch);
                    ++result.normal_matches;
                }
            }
        }
    } else {
        result.diagnostics.emplace_back("image has no normal import directory");
    }

    const auto delayDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (delayDir.VirtualAddress && delayDir.Size >= sizeof(ImgDelayDescr)) {
        auto* desc = image.rva<ImgDelayDescr>(delayDir.VirtualAddress);
        const size_t maxDesc = delayDir.Size / sizeof(ImgDelayDescr);
        if (!desc) {
            result.diagnostics.emplace_back("delay-import directory is out of image bounds");
        } else {
            for (size_t d = 0; d < maxDesc && desc[d].rvaDLLName; ++d) {
                if ((desc[d].grAttrs & dlattrRva) == 0) {
                    result.diagnostics.emplace_back(
                        "encountered legacy VA-form delay descriptor; unsupported safely on x64");
                    continue;
                }
                const char* dllName = image.stringRva(desc[d].rvaDLLName);
                if (!dllName || _stricmp(dllName, importedDll) != 0) continue;
                result.saw_matching_delay_descriptor = true;
                auto* lookup = image.rva<IMAGE_THUNK_DATA64>(desc[d].rvaINT);
                auto* iatThunk = image.rva<IMAGE_THUNK_DATA64>(desc[d].rvaIAT);
                if (!lookup || !iatThunk) {
                    result.diagnostics.emplace_back("matching delay-import thunk table is out of bounds");
                    continue;
                }
                const size_t lookupIndex = static_cast<size_t>(
                    reinterpret_cast<unsigned char*>(lookup) - image.base);
                const size_t iatIndex = static_cast<size_t>(
                    reinterpret_cast<unsigned char*>(iatThunk) - image.base);
                const size_t maxLookup = (image.size - lookupIndex) / sizeof(IMAGE_THUNK_DATA64);
                const size_t maxIat = (image.size - iatIndex) / sizeof(IMAGE_THUNK_DATA64);
                const size_t limit = (maxLookup < maxIat) ? maxLookup : maxIat;
                for (size_t i = 0; i < limit && lookup[i].u1.AddressOfData; ++i) {
                    if (!IsMatchingNameThunk(lookup[i].u1.AddressOfData, image, importedFunction)) continue;
                    HMODULE imported = GetModuleHandleA(importedDll);
                    if (!imported) imported = LoadLibraryA(importedDll);
                    void* native = imported
                        ? reinterpret_cast<void*>(GetProcAddress(imported, importedFunction))
                        : nullptr;
                    if (!native) {
                        std::ostringstream os;
                        os << "GetProcAddress(" << importedDll << ", " << importedFunction
                           << ") failed for delay import, Win32 error " << GetLastError();
                        result.diagnostics.push_back(os.str());
                        RollBack(result.patches);
                        result.callable_original = nullptr;
                        return result;
                    }
                    Patch patch;
                    if (!PatchSlot(reinterpret_cast<void**>(&iatThunk[i].u1.Function),
                                   replacement, patch, error)) {
                        if (patch.slot) result.patches.push_back(patch);
                        result.diagnostics.push_back(error);
                        RollBack(result.patches);
                        result.callable_original = nullptr;
                        return result;
                    }
                    if (!result.callable_original) result.callable_original = native;
                    result.patches.push_back(patch);
                    ++result.delay_matches;
                }
            }
        }
    } else {
        result.diagnostics.emplace_back("image has no delay-import directory");
    }

    if (result.patches.empty()) {
        std::ostringstream os;
        os << "requested import " << importedDll << '!' << importedFunction
           << " was not found (matching normal descriptor="
           << (result.saw_matching_normal_descriptor ? "yes" : "no")
           << ", matching delay descriptor="
           << (result.saw_matching_delay_descriptor ? "yes" : "no") << ')';
        result.diagnostics.push_back(os.str());
        result.callable_original = nullptr;
    }
    return result;
}

bool RestoreImports(HookResult& result, std::string* error) noexcept {
    bool allOk = true;
    for (auto it = result.patches.rbegin(); it != result.patches.rend(); ++it) {
        DWORD oldProtect = 0;
        if (!it->slot || !VirtualProtect(it->slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
            allOk = false;
            if (error) *error = "VirtualProtect failed while restoring an IAT slot";
            continue;
        }
        InterlockedExchangePointer(reinterpret_cast<void* volatile*>(it->slot), it->previous);
        DWORD ignored = 0;
        if (!VirtualProtect(it->slot, sizeof(void*), oldProtect, &ignored)) {
            allOk = false;
            if (error) *error = "page protection restore failed while restoring an IAT slot";
        }
        FlushInstructionCache(GetCurrentProcess(), it->slot, sizeof(void*));
    }
    result.patches.clear();
    return allOk;
}

}  // namespace iat
