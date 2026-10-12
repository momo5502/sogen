#include "file_signing.hpp"
#include <array>

#ifdef OS_WINDOWS
#include <windows.h>
#include <io.h>
#endif

namespace sogen::utils
{
#ifdef OS_WINDOWS
    namespace
    {
        template <typename Function>
        Function resolve_nt_function(const char* name)
        {
            return reinterpret_cast<Function>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), name)));
        }
    }
#endif

    NTSTATUS compare_signing_levels(const UCHAR current_level, const UCHAR required_level)
    {
#ifdef OS_WINDOWS
        using compare_type = LONG(NTAPI*)(UCHAR, UCHAR);
        static const auto compare = resolve_nt_function<compare_type>("NtCompareSigningLevels");
        if (compare != nullptr)
        {
            return compare(current_level, required_level);
        }
#endif
        // Signing levels form a policy lattice, not an ordered integer scale. Custom slots
        // use the default Windows policy here; a Windows host supplies its own active policy above.
        constexpr std::array<uint16_t, 16> allowed_levels{0x0001, 0x0003, 0x020f, 0x000b, 0x021f, 0x027b, 0x025b, 0x028f,
                                                          0x037f, 0x0003, 0x0003, 0x0b7f, 0x13ff, 0x0003, 0x53ff, 0x0003};
        constexpr NTSTATUS invalid_image_hash = 0xc0000428;
        return (allowed_levels[current_level & 0xf] & (1u << (required_level & 0xf))) != 0 ? STATUS_SUCCESS : invalid_image_hash;
    }

    NTSTATUS query_cached_signing_level(const int file_descriptor, cached_signing_level& result)
    {
#ifdef OS_WINDOWS
        using query_type = LONG(NTAPI*)(HANDLE, ULONG*, UCHAR*, UCHAR*, ULONG*, ULONG*);
        static const auto query = resolve_nt_function<query_type>("NtGetCachedSigningLevel");
        if (query == nullptr)
        {
            return STATUS_NOT_SUPPORTED;
        }
        auto* const handle = reinterpret_cast<HANDLE>(_get_osfhandle(file_descriptor));
        ULONG size{};
        auto status = static_cast<NTSTATUS>(query(handle, &result.flags, &result.level, nullptr, &size, &result.thumbprint_algorithm));
        if (status != STATUS_BUFFER_TOO_SMALL && status != STATUS_SUCCESS)
        {
            return status;
        }
        result.thumbprint.resize(size);
        status = static_cast<NTSTATUS>(query(handle, &result.flags, &result.level, reinterpret_cast<UCHAR*>(result.thumbprint.data()),
                                             &size, &result.thumbprint_algorithm));
        if (status == STATUS_SUCCESS)
        {
            result.thumbprint.resize(size);
        }
        return status;
#else
        static_cast<void>(file_descriptor);
        static_cast<void>(result);
        return STATUS_NOT_FOUND;
#endif
    }
}
