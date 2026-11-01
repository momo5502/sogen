// _GNU_SOURCE exposes the Linux host VM and signal definitions used by this backend.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#define FEX_EMULATOR_IMPL
#include "fex_x86_64_emulator.hpp"
#include "fex_x86_64_common.hpp"

#include "address_utils.hpp"
#include <utils/finally.hpp>

#if !defined(__APPLE__) && !defined(__linux__)
#error "The FEX backend supports only Darwin and Linux hosts"
#endif

// FEX (https://fex-emu.com) is an in-process x86-64 -> AArch64 binary translator. Unlike the
// Unicorn/Icicle/KVM backends it does not manage a sandboxed guest address space: the translated
// guest executes inside the host process with guest VA == host VA. So map_memory() is a real
// mmap(MAP_FIXED) at the guest address and read/write_memory() a direct host memcpy, and - as with
// KVM - there is no per-access or per-instruction instrumentation point, so memory/execution/block
// hooks are accepted for API compatibility but never fire. Guest `syscall` instructions come back
// through a FEXCore::HLE::SyscallHandler that invokes the registered syscall instruction hook.
//
// The functional targets are Darwin on Apple Silicon and Linux (including Android) on AArch64; the
// signal handlers and MMIO fault emulation below support both. Linux currently requires a 4KB host
// page; the 16KB/4KB page reconciliation and MAP_JIT handling are Darwin-only.

#include <cstdlib>
#include <pthread.h>
#include <csignal>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <unistd.h>

#ifdef __linux__
#if !defined(__ANDROID__) || __ANDROID_API__ >= 33
#include <execinfo.h>
#endif
#include <asm/hwcap.h>
#include <dirent.h>
#include <linux/prctl.h>
#include <sys/auxv.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#else
#include <execinfo.h>
#endif

#ifdef __APPLE__
#include <sys/sysctl.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/arm/thread_status.h>
#include <libkern/OSCacheControl.h>
#include <libproc.h>
#endif

#include <atomic>
#include <array>
#include <bit>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <algorithm>
#include <charconv>
#include <ranges>
#include <string_view>

#include <utils/object.hpp>
#include <utils/io.hpp>

// FEXCore embedding headers. These are only available when building against a FEX checkout/install;
// the CMake glue gates this whole target behind SOGEN_ENABLE_FEX so non-ARM builds never reach here.
#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/HostFeatures.h>
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/Utils/LongJump.h>
#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/Utils/AllocatorHooks.h>
#include <FEXCore/Utils/ArchHelpers/Arm64.h>

#if SOGEN_FEX_HYPERVISOR
#include <hvf_arm64_emulator.hpp>

#include <dlfcn.h>
#include <mach-o/loader.h>
#endif

#if SOGEN_FEX_HYPERVISOR
// The host side of a call FEX's code makes from inside the VM: calls `function` with the AAPCS64 argument registers
// x0-x7 and q0-q7 from the frame and stores the x0, x1 and q0-q3 it returns back into it.
struct sogen_fex_host_call_frame
{
    std::array<uint64_t, 8> x;
    alignas(16) std::array<__uint128_t, 8> v;
    uint64_t function;
};

static_assert(offsetof(sogen_fex_host_call_frame, v) == 64 && offsetof(sogen_fex_host_call_frame, function) == 192);

extern "C" void sogen_fex_host_call(sogen_fex_host_call_frame* frame);

// NOLINTNEXTLINE(hicpp-no-assembler)
__asm__(".text\n"
        ".p2align 2\n"
        ".private_extern _sogen_fex_host_call\n"
        "_sogen_fex_host_call:\n"
        "    stp x29, x30, [sp, #-32]!\n"
        "    mov x29, sp\n"
        "    str x19, [sp, #16]\n"
        "    mov x19, x0\n"
        "    ldp q0, q1, [x19, #64]\n"
        "    ldp q2, q3, [x19, #96]\n"
        "    ldp q4, q5, [x19, #128]\n"
        "    ldp q6, q7, [x19, #160]\n"
        "    ldr x16, [x19, #192]\n"
        "    ldp x0, x1, [x19, #0]\n"
        "    ldp x2, x3, [x19, #16]\n"
        "    ldp x4, x5, [x19, #32]\n"
        "    ldp x6, x7, [x19, #48]\n"
        "    blr x16\n"
        "    stp x0, x1, [x19, #0]\n"
        "    stp q0, q1, [x19, #64]\n"
        "    stp q2, q3, [x19, #96]\n"
        "    ldr x19, [sp, #16]\n"
        "    ldp x29, x30, [sp], #32\n"
        "    ret\n");
#endif

namespace sogen::fex
{
    class fex_x86_64_emulator;

    namespace
    {
        constexpr size_t page_size = 0x1000;

        // Coarse enough that streaming accesses to tracked memory fault rarely, fine enough that flushing or invalidating a
        // chunk stays cheap.
        constexpr size_t tracking_chunk_size = 0x10000;

        // The part of the guest address space a WoW64 process's 32-bit code can address.
        constexpr uint64_t low_guest_window_size = 0x100000000ULL;

        // Guest memory above 4GB is placed in one window that every process reserves at the same address, so a saved
        // process finds its addresses free again in a new one, and a run does not depend on where the host's ASLR put
        // its own mappings. Android's 39-bit address space leaves the Linux base alone. macOS reserves [64GB, 448GB)
        // for itself and grows its own anonymous mappings from just below 512GB, so its arena starts at 1TB.
#ifdef __APPLE__
        constexpr uint64_t guest_arena_base = 0x10000000000ULL;
#else
        constexpr uint64_t guest_arena_base = 0x1000000000ULL;
#endif
        constexpr uint64_t guest_arena_size = 0x4000000000ULL;

        constexpr uint64_t guest_address_space_end = 0x00007ffffffeffffULL + 1;

#ifdef __linux__

#ifndef MAP_FIXED_NOREPLACE
        constexpr int MAP_FIXED_NOREPLACE = 0x100000;
#endif

        std::vector<host_reserved_range> read_host_mappings_linux(const uint64_t window_start, const uint64_t window_end)
        {
            std::vector<std::byte> data;
            if (!utils::io::read_file("/proc/self/maps", &data))
            {
                throw std::runtime_error("FEX backend failed to enumerate host mappings");
            }

            const std::string_view maps{reinterpret_cast<const char*>(data.data()), data.size()};

            std::vector<host_reserved_range> ranges;

            for (auto line : maps | std::views::split('\n'))
            {
                const auto dash = std::ranges::find(line, '-');
                if (dash == line.end())
                {
                    continue;
                }

                uint64_t mapping_start, mapping_end;

                const char* begin = std::to_address(line.begin());
                const char* separator = std::to_address(dash);
                const char* end = std::to_address(line.end());

                const auto [p1, ec1] = std::from_chars(begin, separator, mapping_start, 16);
                const auto [p2, ec2] = std::from_chars(separator + 1, end, mapping_end, 16);

                if (ec1 != std::errc{} || ec2 != std::errc{})
                {
                    continue;
                }

                const auto hit_start = std::max(mapping_start, window_start);
                const auto hit_end = std::min(mapping_end, window_end);

                if (hit_start < hit_end)
                {
                    ranges.push_back({
                        .address = hit_start,
                        .size = static_cast<size_t>(hit_end - hit_start),
                    });
                }
            }

            return ranges;
        }

        struct host_mapping_info
        {
            uint64_t start{};
            uint64_t offset{};
            uint32_t device_major{};
            uint32_t device_minor{};
            uint64_t inode{};
            bool shared{};
        };

        std::optional<host_mapping_info> find_host_mapping(const uint64_t address)
        {
            std::vector<std::byte> data;
            if (!utils::io::read_file("/proc/self/maps", &data))
            {
                return std::nullopt;
            }

            const std::string maps{reinterpret_cast<const char*>(data.data()), data.size()};
            size_t line_start = 0;
            while (line_start < maps.size())
            {
                const auto line_end = std::min(maps.find('\n', line_start), maps.size());
                const auto line = maps.substr(line_start, line_end - line_start);
                line_start = line_end + 1;

                unsigned long long start = 0;
                unsigned long long end = 0;
                char permissions[5]{};
                unsigned long long offset = 0;
                unsigned int major = 0;
                unsigned int minor = 0;
                unsigned long long inode = 0;
                if (std::sscanf(line.c_str(), "%llx-%llx %4s %llx %x:%x %llu", &start, &end, permissions, &offset, &major, &minor,
                                &inode) != 7)
                {
                    continue;
                }

                if (address >= start && address < end)
                {
                    return host_mapping_info{
                        .start = start,
                        .offset = offset,
                        .device_major = major,
                        .device_minor = minor,
                        .inode = inode,
                        .shared = permissions[3] == 's',
                    };
                }
            }

            return std::nullopt;
        }

        // Nothing names the descriptor a mapping was made through, and a driver can scope mmap offsets to the open file
        // that created them (a Mali context, for one), so a file that is open more than once is not mapped again.
        int find_open_file(const host_mapping_info& mapping)
        {
            DIR* directory = ::opendir("/proc/self/fd");
            if (!directory)
            {
                return -1;
            }

            int result = -1;
            while (const dirent* entry = ::readdir(directory))
            {
                const int fd = std::atoi(entry->d_name);
                struct stat status{};
                if (entry->d_name[0] == '.' || fd == ::dirfd(directory) || ::fstat(fd, &status) != 0)
                {
                    continue;
                }

                if (status.st_ino == mapping.inode && major(status.st_dev) == mapping.device_major &&
                    minor(status.st_dev) == mapping.device_minor)
                {
                    if (result >= 0)
                    {
                        result = -1;
                        break;
                    }
                    result = fd;
                }
            }

            ::closedir(directory);
            return result;
        }

        // Makes the shared host memory at source also appear at target. A file-backed mapping (a GPU driver's device
        // file, a memfd) is mapped again from its file, which goes through the driver's own mmap and so stays coherent;
        // mremap can only duplicate anonymous shared memory. Private memory cannot be aliased at all.
        bool alias_shared_mapping(const uint64_t source, const size_t size, const uint64_t target)
        {
            const auto mapping = find_host_mapping(source);
            if (!mapping || !mapping->shared)
            {
                return false;
            }

            if (const int fd = mapping->inode != 0 ? find_open_file(*mapping) : -1; fd >= 0)
            {
                const auto file_offset = static_cast<off_t>(mapping->offset + (source - mapping->start));
                return ::mmap(reinterpret_cast<void*>(target), size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, file_offset) !=
                       MAP_FAILED;
            }

            return ::mremap(reinterpret_cast<void*>(source), 0, size, MREMAP_MAYMOVE | MREMAP_FIXED, reinterpret_cast<void*>(target)) !=
                   MAP_FAILED;
        }

        FEXCore::HostFeatures fetch_host_features_linux()
        {
            FEXCore::HostFeatures features{};

            const unsigned long hwcap = ::getauxval(AT_HWCAP);
            const unsigned long hwcap2 = ::getauxval(AT_HWCAP2);

            const auto has = [hwcap](unsigned long flag) { return (hwcap & flag) != 0; };
            const auto has2 = [hwcap2](unsigned long flag) { return (hwcap2 & flag) != 0; };

            features.SupportsAES = has(HWCAP_AES);
            features.SupportsCRC = has(HWCAP_CRC32);
            features.SupportsAtomics = has(HWCAP_ATOMICS);
            features.SupportsRCPC = has(HWCAP_LRCPC);
            features.SupportsTSOImm9 = has(HWCAP_ILRCPC);
            features.SupportsSHA = has(HWCAP_SHA1) && has(HWCAP_SHA2);
            features.SupportsPMULL_128Bit = has(HWCAP_PMULL);
            features.SupportsFCMA = has(HWCAP_FCMA);
            features.SupportsFlagM = has(HWCAP_FLAGM);

            features.SupportsRAND = has2(HWCAP2_RNG);
            features.SupportsFlagM2 = has2(HWCAP2_FLAGM2);
            features.SupportsFRINTTS = has2(HWCAP2_FRINT);
            features.SupportsECV = has2(HWCAP2_ECV);
            features.SupportsAFP = has2(HWCAP2_AFP);
            features.SupportsRPRES = has2(HWCAP2_RPRES);
            features.SupportsWFXT = has2(HWCAP2_WFXT);
            features.SupportsSVEBitPerm = has2(HWCAP2_SVEBITPERM);

#ifdef HWCAP2_CSSC
            features.SupportsCSSC = has2(HWCAP2_CSSC);
#endif

#ifdef HWCAP2_MOPS
            features.SupportsMOPS = has2(HWCAP2_MOPS);
#endif

            features.SupportsSVE128 = has2(HWCAP2_SVE2);
            if (features.SupportsSVE128)
            {
                const int sve_vl = ::prctl(PR_SVE_GET_VL);
                if (sve_vl >= 0)
                {
                    features.SupportsSVE256 = (sve_vl & PR_SVE_VL_LEN_MASK) >= 32;
                }
            }

            // AVX can make FEX emit paired SIMD loads, which MMIO emulation doesn't support yet.
            features.SupportsAVX = false;
            features.SupportsAES256 = features.SupportsAES && features.SupportsAVX;

            const long dcache_line = ::sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
            const long icache_line = ::sysconf(_SC_LEVEL1_ICACHE_LINESIZE);
            features.DCacheLineSize = dcache_line > 0 ? static_cast<uint32_t>(dcache_line) : 64;
            features.ICacheLineSize = icache_line > 0 ? static_cast<uint32_t>(icache_line) : 64;
            features.SupportsCacheMaintenanceOps = true;

            uint64_t dczid = 0;
            __asm__ volatile("mrs %0, dczid_el0" : "=r"(dczid));
            features.SupportsCLZERO = (dczid & (1ULL << 4)) == 0 && (4ULL << (dczid & 0xF)) == 64;

            const long logical_cpus = ::sysconf(_SC_NPROCESSORS_ONLN);
            features.CPUMIDRs.assign(logical_cpus > 0 ? static_cast<size_t>(logical_cpus) : 1, 0u);

            features.SupportsCPUIndexInTPIDRRO = false;

            return features;
        }
#endif

#ifdef __APPLE__
        // FEXCore's JITWriteScope (FEXCore/Utils/AllocatorHooks.h) toggles pthread_jit_write_protect_np
        // per call, not via a nesting counter, so bracketing our own call to Pointers.ExitFunctionLink
        // is not enough: its "not yet compiled" path calls CompileBlock, whose own nested JITWriteScope
        // re-enables write protection before control returns to our still-writing outer call, faulting
        // on the self-modifying store. Interposing pthread_jit_write_protect_np to make it reentrant
        // does not work either - every route to the real implementation from inside the interposer
        // resolves back to the replacement.
        //
        // So the fault is reacted to instead: handle_fault_signal's SEGV_ACCERR branch disables write
        // protection and retries when the faulting PC is FEXCore's own code, not MAP_JIT memory,
        // bounded per fault address so a different bug cannot spin. The bound lives in a fixed array
        // rather than an unordered_map since operator[] can rehash and is unsafe to call from a signal
        // handler that may interrupt an unrelated malloc()/free(). Eight slots suffice: guest execution
        // is cooperative, so at most one address is ever mid-retry.
        struct jit_write_protect_retry_slot
        {
            uint64_t address = 0;
            int count = 0;
            bool used = false;
            uint64_t last_fault_ns = 0;
        };

        constexpr size_t jit_write_protect_retry_slot_count = 8;
        std::array<jit_write_protect_retry_slot, jit_write_protect_retry_slot_count> g_jit_write_protect_retry_slots{};
        size_t g_jit_write_protect_retry_next_evict = 0;

        // A gap at least this long since an address last faulted means the site is being reused
        // healthily rather than spinning, so its retry budget is reset.
        constexpr uint64_t jit_write_protect_retry_reset_window_ns = 100'000'000; // 100ms

        uint64_t monotonic_now_ns()
        {
            struct timespec ts{};
            ::clock_gettime(CLOCK_MONOTONIC, &ts);
            return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL + static_cast<uint64_t>(ts.tv_nsec);
        }

        // Resets the counter when the address has not faulted within the window: otherwise a site that
        // legitimately resolves this race many times over a long run would exhaust its budget and turn a
        // benign race into a hard crash. Async-signal-safe: fixed-array scan, no allocation, and
        // clock_gettime(CLOCK_MONOTONIC) is vDSO-backed.
        int& jit_write_protect_retry_count_for(const uint64_t fault_addr)
        {
            const uint64_t now_ns = monotonic_now_ns();
            jit_write_protect_retry_slot* free_slot = nullptr;
            for (auto& slot : g_jit_write_protect_retry_slots)
            {
                if (slot.used && slot.address == fault_addr)
                {
                    if (now_ns - slot.last_fault_ns > jit_write_protect_retry_reset_window_ns)
                    {
                        slot.count = 0;
                    }
                    slot.last_fault_ns = now_ns;
                    return slot.count;
                }
                if (free_slot == nullptr && !slot.used)
                {
                    free_slot = &slot;
                }
            }

            auto& slot = (free_slot != nullptr)
                             ? *free_slot
                             : g_jit_write_protect_retry_slots[g_jit_write_protect_retry_next_evict++ % jit_write_protect_retry_slot_count];
            slot.address = fault_addr;
            slot.count = 0;
            slot.used = true;
            slot.last_fault_ns = now_ns;
            return slot.count;
        }

        // Apple Silicon's fixed host mmap/mprotect granularity (no way to get 4KB host pages).
        constexpr size_t host_page_size_apple = 0x4000;

        uint64_t host_page_align_down_apple(const uint64_t value)
        {
            return value & ~(host_page_size_apple - 1);
        }

        uint64_t host_page_align_up_apple(const uint64_t value)
        {
            return (value + host_page_size_apple - 1) & ~(host_page_size_apple - 1);
        }
#endif

        // Decodes only what an mmio_region fault needs: destination register, transfer size and
        // extension. The addressing mode is deliberately not decoded - the effective address is already
        // known, being the fault address itself under guest VA == host VA - so only the fields the
        // "Load/store register" class keeps at fixed bit positions across every sub-form are read
        // (size/opc at [31:30]/[23:22], Rt at [4:0]; AArch64 ISA C4.1.3). Stores are not decoded: this
        // backend's only MMIO consumer is read-only.
        struct decoded_arm64_load
        {
            uint32_t size = 0; // bytes: 1, 2, 4, 8, 16
            bool sign_extend = false;
            bool dest_is_64bit = false;
            bool is_vector = false; // true: rt names a SIMD&FP register (Qt), not a GPR
            uint32_t rt = 0;
        };

        std::optional<decoded_arm64_load> decode_arm64_load(const uint32_t insn)
        {
            struct encoding
            {
                uint32_t value;
                uint32_t size;
                bool sign_extend;
                bool dest_is_64bit;
                bool is_vector = false;
            };

            // "Load register (unsigned immediate)" - top 10 bits (size:2, fixed:6, opc:2).
            static constexpr auto unsigned_imm_loads = std::to_array<encoding>({
                {.value = 0x39400000U, .size = 1, .sign_extend = false, .dest_is_64bit = false}, // LDRB
                {.value = 0x39800000U, .size = 1, .sign_extend = true, .dest_is_64bit = true},   // LDRSB, 64-bit dest
                {.value = 0x39C00000U, .size = 1, .sign_extend = true, .dest_is_64bit = false},  // LDRSB, 32-bit dest
                {.value = 0x79400000U, .size = 2, .sign_extend = false, .dest_is_64bit = false}, // LDRH
                {.value = 0x79800000U, .size = 2, .sign_extend = true, .dest_is_64bit = true},   // LDRSH, 64-bit dest
                {.value = 0x79C00000U, .size = 2, .sign_extend = true, .dest_is_64bit = false},  // LDRSH, 32-bit dest
                {.value = 0xB9400000U, .size = 4, .sign_extend = false, .dest_is_64bit = false}, // LDR Wt
                {.value = 0xB9800000U, .size = 4, .sign_extend = true, .dest_is_64bit = true},   // LDRSW
                {.value = 0xF9400000U, .size = 8, .sign_extend = false, .dest_is_64bit = true},  // LDR Xt
            });

            // "Load register (register offset)" - top 22 bits + fixed low bits (opt/S/1/0 = 0x800).
            static constexpr auto reg_offset_loads = std::to_array<encoding>({
                {.value = 0x38600800U, .size = 1, .sign_extend = false, .dest_is_64bit = false}, // LDRB (register)
                {.value = 0x38A00800U, .size = 1, .sign_extend = true, .dest_is_64bit = true},   // LDRSB (register), 64-bit dest
                {.value = 0x38E00800U, .size = 1, .sign_extend = true, .dest_is_64bit = false},  // LDRSB (register), 32-bit dest
                {.value = 0x78600800U, .size = 2, .sign_extend = false, .dest_is_64bit = false}, // LDRH (register)
                {.value = 0x78A00800U, .size = 2, .sign_extend = true, .dest_is_64bit = true},   // LDRSH (register), 64-bit dest
                {.value = 0x78E00800U, .size = 2, .sign_extend = true, .dest_is_64bit = false},  // LDRSH (register), 32-bit dest
                {.value = 0xB8600800U, .size = 4, .sign_extend = false, .dest_is_64bit = false}, // LDR Wt (register)
                {.value = 0xB8A00800U, .size = 4, .sign_extend = true, .dest_is_64bit = true},   // LDRSW (register)
                {.value = 0xF8600800U, .size = 8, .sign_extend = false, .dest_is_64bit = true},  // LDR Xt (register)
            });

            // "Load register (unscaled immediate)" - LDUR plus FEAT_LRCPC2's LDAPUR family. Both
            // use a signed imm9 in bits[20:12], so the same top22+low-bits mask handles them while
            // ignoring the immediate, Rn and Rt.
            static constexpr auto unscaled_loads = std::to_array<encoding>({
                {.value = 0x38400000U, .size = 1, .sign_extend = false, .dest_is_64bit = false},                     // LDURB
                {.value = 0x38800000U, .size = 1, .sign_extend = true, .dest_is_64bit = true},                       // LDURSB, 64-bit dest
                {.value = 0x38C00000U, .size = 1, .sign_extend = true, .dest_is_64bit = false},                      // LDURSB, 32-bit dest
                {.value = 0x78400000U, .size = 2, .sign_extend = false, .dest_is_64bit = false},                     // LDURH
                {.value = 0x78800000U, .size = 2, .sign_extend = true, .dest_is_64bit = true},                       // LDURSH, 64-bit dest
                {.value = 0x78C00000U, .size = 2, .sign_extend = true, .dest_is_64bit = false},                      // LDURSH, 32-bit dest
                {.value = 0xB8400000U, .size = 4, .sign_extend = false, .dest_is_64bit = false},                     // LDUR Wt
                {.value = 0xB8800000U, .size = 4, .sign_extend = true, .dest_is_64bit = true},                       // LDURSW
                {.value = 0xF8400000U, .size = 8, .sign_extend = false, .dest_is_64bit = true},                      // LDUR Xt
                {.value = 0x3CC00000U, .size = 16, .sign_extend = false, .dest_is_64bit = false, .is_vector = true}, // LDUR Qt

                {.value = 0x19400000U, .size = 1, .sign_extend = false, .dest_is_64bit = false}, // LDAPURB
                {.value = 0x19800000U, .size = 1, .sign_extend = true, .dest_is_64bit = true},   // LDAPURSB, 64-bit dest
                {.value = 0x19C00000U, .size = 1, .sign_extend = true, .dest_is_64bit = false},  // LDAPURSB, 32-bit dest
                {.value = 0x59400000U, .size = 2, .sign_extend = false, .dest_is_64bit = false}, // LDAPURH
                {.value = 0x59800000U, .size = 2, .sign_extend = true, .dest_is_64bit = true},   // LDAPURSH, 64-bit dest
                {.value = 0x59C00000U, .size = 2, .sign_extend = true, .dest_is_64bit = false},  // LDAPURSH, 32-bit dest
                {.value = 0x99400000U, .size = 4, .sign_extend = false, .dest_is_64bit = false}, // LDAPUR Wt
                {.value = 0x99800000U, .size = 4, .sign_extend = true, .dest_is_64bit = true},   // LDAPURSW
                {.value = 0xD9400000U, .size = 8, .sign_extend = false, .dest_is_64bit = true},  // LDAPUR Xt
            });

            // "Load-acquire register (LDAPR/LDAPRB/LDAPRH, and the older base LDAR/LDARB/LDARH from
            // the Load/store-exclusive class)" - the forms FEX emits for guest memory reads to model
            // x86's stronger memory ordering on ARM's weaker one. No addressing mode at all (always
            // [Xn], Rm fixed to the 11111 placeholder), so only Rn/Rt vary - mask out the low 10 bits.
            // No signed variants exist for either family.
            static constexpr auto acquire_loads = std::to_array<encoding>({
                {.value = 0x38BFC000U, .size = 1, .sign_extend = false, .dest_is_64bit = false}, // LDAPRB
                {.value = 0x78BFC000U, .size = 2, .sign_extend = false, .dest_is_64bit = false}, // LDAPRH
                {.value = 0xB8BFC000U, .size = 4, .sign_extend = false, .dest_is_64bit = false}, // LDAPR Wt
                {.value = 0xF8BFC000U, .size = 8, .sign_extend = false, .dest_is_64bit = true},  // LDAPR Xt
                {.value = 0x08DFFC00U, .size = 1, .sign_extend = false, .dest_is_64bit = false}, // LDARB
                {.value = 0x48DFFC00U, .size = 2, .sign_extend = false, .dest_is_64bit = false}, // LDARH
                {.value = 0x88DFFC00U, .size = 4, .sign_extend = false, .dest_is_64bit = false}, // LDAR Wt
                {.value = 0xC8DFFC00U, .size = 8, .sign_extend = false, .dest_is_64bit = true},  // LDAR Xt
            });

            // "Load SIMD&FP register (unsigned immediate), 128-bit" - shares unsigned_imm_loads' mask/
            // position (V=1, size=00, opc=11 is the reserved combination meaning a 128-bit Q register
            // rather than a scalar B/H/S/D FP register). FEX uses this for a wide (16-byte) guest read.
            static constexpr auto vector_loads = std::to_array<encoding>({
                {.value = 0x3DC00000U, .size = 16, .sign_extend = false, .dest_is_64bit = false, .is_vector = true}, // LDR Qt
            });

            const uint32_t rt = insn & 0x1FU;
            const uint32_t top10 = insn & 0xFFC00000U;
            const uint32_t top22_fixed_low = insn & 0xFFE00C00U;
            const uint32_t acquire_fixed = insn & 0xFFFFFC00U;

            for (const auto& enc : unsigned_imm_loads)
            {
                if (top10 == enc.value)
                {
                    return decoded_arm64_load{.size = enc.size,
                                              .sign_extend = enc.sign_extend,
                                              .dest_is_64bit = enc.dest_is_64bit,
                                              .is_vector = enc.is_vector,
                                              .rt = rt};
                }
            }

            for (const auto& enc : reg_offset_loads)
            {
                if (top22_fixed_low == enc.value)
                {
                    return decoded_arm64_load{.size = enc.size,
                                              .sign_extend = enc.sign_extend,
                                              .dest_is_64bit = enc.dest_is_64bit,
                                              .is_vector = enc.is_vector,
                                              .rt = rt};
                }
            }

            for (const auto& enc : unscaled_loads)
            {
                if (top22_fixed_low == enc.value)
                {
                    return decoded_arm64_load{.size = enc.size,
                                              .sign_extend = enc.sign_extend,
                                              .dest_is_64bit = enc.dest_is_64bit,
                                              .is_vector = enc.is_vector,
                                              .rt = rt};
                }
            }

            for (const auto& enc : acquire_loads)
            {
                if (acquire_fixed == enc.value)
                {
                    return decoded_arm64_load{.size = enc.size,
                                              .sign_extend = enc.sign_extend,
                                              .dest_is_64bit = enc.dest_is_64bit,
                                              .is_vector = enc.is_vector,
                                              .rt = rt};
                }
            }

            for (const auto& enc : vector_loads)
            {
                if (top10 == enc.value)
                {
                    return decoded_arm64_load{.size = enc.size,
                                              .sign_extend = enc.sign_extend,
                                              .dest_is_64bit = enc.dest_is_64bit,
                                              .is_vector = enc.is_vector,
                                              .rt = rt};
                }
            }

            return std::nullopt;
        }

        // Store-release forms (STLR/STLRB/STLRH and STLUR/STLURB/STLURH), the counterpart to
        // decode_arm64_load's LDAR/LDAPR/LDAPUR handling.
        //
        // Deliberately narrow: adding plain STR/STUR causes a reproducible hang in false-fault
        // emulation, isolated to this table (root cause not yet understood; decode_arm64_load's
        // equivalent plain-load coverage is safe).
        struct decoded_arm64_store
        {
            uint32_t size = 0; // bytes: 1, 2, 4, 8
            uint32_t rt = 0;
        };

        std::optional<decoded_arm64_store> decode_arm64_store(const uint32_t insn)
        {
            struct encoding
            {
                uint32_t value;
                uint32_t size;
            };

            static constexpr auto stores = std::to_array<encoding>({
                {.value = 0x089FFC00U, .size = 1}, // STLRB
                {.value = 0x489FFC00U, .size = 2}, // STLRH
                {.value = 0x889FFC00U, .size = 4}, // STLR Wt
                {.value = 0xC89FFC00U, .size = 8}, // STLR Xt
            });

            // FEAT_LRCPC2 store-release with signed imm9. FEX emits these when SupportsTSOImm9 is
            // enabled; unlike ordinary STUR, they retain release ordering and require natural alignment.
            static constexpr auto unscaled_stores = std::to_array<encoding>({
                {.value = 0x19000000U, .size = 1}, // STLURB
                {.value = 0x59000000U, .size = 2}, // STLURH
                {.value = 0x99000000U, .size = 4}, // STLUR Wt
                {.value = 0xD9000000U, .size = 8}, // STLUR Xt
            });

            const uint32_t rt = insn & 0x1FU;
            const uint32_t fixed = insn & 0xFFFFFC00U;
            const uint32_t unscaled_fixed = insn & 0xFFE00C00U;

            for (const auto& enc : stores)
            {
                if (fixed == enc.value)
                {
                    return decoded_arm64_store{.size = enc.size, .rt = rt};
                }
            }

            for (const auto& enc : unscaled_stores)
            {
                if (unscaled_fixed == enc.value)
                {
                    return decoded_arm64_store{.size = enc.size, .rt = rt};
                }
            }

            return std::nullopt;
        }

        bool is_page_aligned(const uint64_t value)
        {
            return (value & (page_size - 1)) == 0;
        }

        int to_prot(const memory_permission permissions)
        {
            int prot = PROT_NONE;
            if ((permissions & memory_permission::read) != memory_permission::none)
            {
                prot |= PROT_READ;
            }
            if ((permissions & memory_permission::write) != memory_permission::none)
            {
                prot |= PROT_WRITE;
            }
            if ((permissions & memory_permission::exec) != memory_permission::none)
            {
                prot |= PROT_EXEC;
            }
            return prot;
        }

#ifdef __APPLE__
        // Apple Silicon's kernel refuses simultaneous write+exec on any non-MAP_JIT mapping (mprotect
        // fails with EACCES), unlike Linux where guest W^X is advisory. Real PE loaders hit this
        // routinely: map .text RWX to apply relocations, then narrow to RX before executing. Favoring
        // write handles that sequence; it would be wrong for a page genuinely written and executed in
        // the same window without an intervening apply_memory_protection call, which PE loading is not.
        int to_prot_apple(const memory_permission permissions)
        {
            int prot = to_prot(permissions);
            if ((prot & PROT_WRITE) && (prot & PROT_EXEC))
            {
                prot &= ~PROT_EXEC;
            }
            return prot;
        }

        // Arm ARM, ESR_EL1 ISS for data aborts: WnR is set for every cache maintenance fault (CM). x86 checks the
        // CLFLUSH/CLFLUSHOPT/CLWB that FEX emits as DC CIVAC/CVAC like a byte load (Intel SDM vol. 2A, CLFLUSH).
        std::optional<memory_operation> data_abort_operation(const uint64_t esr)
        {
            constexpr uint64_t exception_class_shift = 26;
            constexpr uint64_t exception_class_mask = 0x3F;
            constexpr uint64_t data_abort_from_lower_el = 0x24;
            constexpr uint64_t cache_maintenance = 1ULL << 8;
            constexpr uint64_t write_not_read = 1ULL << 6;

            if (((esr >> exception_class_shift) & exception_class_mask) != data_abort_from_lower_el)
            {
                return std::nullopt;
            }

            if ((esr & cache_maintenance) != 0 || (esr & write_not_read) == 0)
            {
                return memory_operation::read;
            }

            return memory_operation::write;
        }
#endif

        // Bit-for-bit reimplementation of FEXCore::Context::ContextImpl::ReconstructCompactedEFLAGS /
        // SetFlagsFromCompactedEFLAGS (FEXCore's Core.cpp), operating directly on a CPUState instead
        // of a live InternalThreadState. FEXCore's originals unconditionally dereference the Thread
        // pointer to reach CurrentFrame->State, which doesn't exist yet for the staged CPUState the
        // Windows loader populates before the first start()/create_thread(). The NZCV bit positions
        // below (28-31) mirror FEXCore's fixed IR::OpDispatchBuilder::IndexNZCV mapping, an internal
        // compiler detail with no public header.
        uint32_t index_nzcv(unsigned bit_offset)
        {
            switch (bit_offset)
            {
            case FEXCore::X86State::RFLAG_OF_RAW_LOC:
                return 28;
            case FEXCore::X86State::RFLAG_CF_RAW_LOC:
                return 29;
            case FEXCore::X86State::RFLAG_ZF_RAW_LOC:
                return 30;
            case FEXCore::X86State::RFLAG_SF_RAW_LOC:
                return 31;
            default:
                return 0;
            }
        }

        uint32_t reconstruct_compacted_eflags(const FEXCore::Core::CPUState& state)
        {
            uint32_t eflags = 0;

            for (size_t i = 0; i < FEXCore::Core::CPUState::NUM_EFLAG_BITS; ++i)
            {
                switch (i)
                {
                case FEXCore::X86State::RFLAG_CF_RAW_LOC:
                case FEXCore::X86State::RFLAG_PF_RAW_LOC:
                case FEXCore::X86State::RFLAG_AF_RAW_LOC:
                case FEXCore::X86State::RFLAG_TF_RAW_LOC:
                case FEXCore::X86State::RFLAG_ZF_RAW_LOC:
                case FEXCore::X86State::RFLAG_SF_RAW_LOC:
                case FEXCore::X86State::RFLAG_OF_RAW_LOC:
                case FEXCore::X86State::RFLAG_DF_RAW_LOC:
                    break;
                default:
                    eflags |= uint32_t{state.flags[i]} << i;
                    break;
                }
            }

            uint32_t packed_nzcv = 0;
            std::memcpy(&packed_nzcv, &state.flags[FEXCore::X86State::RFLAG_NZCV_LOC], sizeof(packed_nzcv));

            const uint32_t of = (packed_nzcv >> index_nzcv(FEXCore::X86State::RFLAG_OF_RAW_LOC)) & 1;
            uint32_t cf = (packed_nzcv >> index_nzcv(FEXCore::X86State::RFLAG_CF_RAW_LOC)) & 1;
            const uint32_t zf = (packed_nzcv >> index_nzcv(FEXCore::X86State::RFLAG_ZF_RAW_LOC)) & 1;
            const uint32_t sf = (packed_nzcv >> index_nzcv(FEXCore::X86State::RFLAG_SF_RAW_LOC)) & 1;
            cf ^= 1;

            eflags |= of << FEXCore::X86State::RFLAG_OF_RAW_LOC;
            eflags |= cf << FEXCore::X86State::RFLAG_CF_RAW_LOC;
            eflags |= zf << FEXCore::X86State::RFLAG_ZF_RAW_LOC;
            eflags |= sf << FEXCore::X86State::RFLAG_SF_RAW_LOC;

            const uint32_t pf_byte = state.pf_raw & 0xff;
            const uint32_t pf = static_cast<uint32_t>(std::popcount(pf_byte ^ 1u)) & 1;
            eflags |= pf << FEXCore::X86State::RFLAG_PF_RAW_LOC;

            const uint32_t af = ((state.af_raw ^ pf_byte) & (1 << 4)) ? 1 : 0;
            eflags |= af << FEXCore::X86State::RFLAG_AF_RAW_LOC;

            const uint8_t tf_byte = state.flags[FEXCore::X86State::RFLAG_TF_RAW_LOC];
            eflags |= (tf_byte & 1) << FEXCore::X86State::RFLAG_TF_RAW_LOC;

            const uint8_t df_byte = state.flags[FEXCore::X86State::RFLAG_DF_RAW_LOC];
            if (df_byte & 0x80)
            {
                eflags |= 1 << FEXCore::X86State::RFLAG_DF_RAW_LOC;
            }

            return eflags;
        }

        void set_flags_from_compacted_eflags(FEXCore::Core::CPUState& state, uint32_t eflags)
        {
            for (size_t i = 0; i < FEXCore::Core::CPUState::NUM_EFLAG_BITS; ++i)
            {
                switch (i)
                {
                case FEXCore::X86State::RFLAG_OF_RAW_LOC:
                case FEXCore::X86State::RFLAG_CF_RAW_LOC:
                case FEXCore::X86State::RFLAG_ZF_RAW_LOC:
                case FEXCore::X86State::RFLAG_SF_RAW_LOC:
                    break;
                case FEXCore::X86State::RFLAG_AF_RAW_LOC:
                    state.af_raw = (eflags & (1U << i)) ? (1 << 4) : 0;
                    break;
                case FEXCore::X86State::RFLAG_PF_RAW_LOC:
                    state.pf_raw = (eflags & (1U << i)) ? 0 : 1;
                    break;
                case FEXCore::X86State::RFLAG_DF_RAW_LOC:
                    state.flags[i] = (eflags & (1U << i)) ? 0xff : 1;
                    break;
                default:
                    state.flags[i] = (eflags & (1U << i)) ? 1 : 0;
                    break;
                }
            }

            uint32_t packed_nzcv = 0;
            packed_nzcv |=
                (eflags & (1U << FEXCore::X86State::RFLAG_OF_RAW_LOC)) ? 1U << index_nzcv(FEXCore::X86State::RFLAG_OF_RAW_LOC) : 0U;
            packed_nzcv |=
                (eflags & (1U << FEXCore::X86State::RFLAG_CF_RAW_LOC)) ? 0U : 1U << index_nzcv(FEXCore::X86State::RFLAG_CF_RAW_LOC);
            packed_nzcv |=
                (eflags & (1U << FEXCore::X86State::RFLAG_ZF_RAW_LOC)) ? 1U << index_nzcv(FEXCore::X86State::RFLAG_ZF_RAW_LOC) : 0U;
            packed_nzcv |=
                (eflags & (1U << FEXCore::X86State::RFLAG_SF_RAW_LOC)) ? 1U << index_nzcv(FEXCore::X86State::RFLAG_SF_RAW_LOC) : 0U;
            std::memcpy(&state.flags[FEXCore::X86State::RFLAG_NZCV_LOC], &packed_nzcv, sizeof(packed_nzcv));

            state.flags[FEXCore::X86State::RFLAG_RESERVED_LOC] = 1;
            state.flags[FEXCore::X86State::RFLAG_IF_LOC] = 1;
        }

        struct mapped_region
        {
            size_t size = 0;
            memory_permission permissions = memory_permission::none;
            bool owned = true; // false for map_host_memory aliases we must not munmap
        };

        // A chunk of tracked memory is write-protected until its first write, which marks it written, and inaccessible
        // while stale, until its first access brings its cache lines up to date. Guest code and the emulator only access
        // tracked memory on the thread running the guest, which also changes the tracking, and the faults recording those
        // accesses arrive as signals on that thread; the state is atomic for those signals, not for other threads.
        struct tracked_chunk
        {
            std::atomic<bool> written{};
            std::atomic<bool> stale{};
        };

        struct tracked_range
        {
            size_t size = 0;
            std::vector<tracked_chunk> chunks;
        };

        struct chunk_run
        {
            uint64_t address = 0;
            size_t size = 0;

            template <typename F>
            void add(const uint64_t chunk_address, const size_t chunk_size, F&& f)
            {
                if (this->size != 0 && this->address + this->size != chunk_address)
                {
                    this->finish(f);
                }
                if (this->size == 0)
                {
                    this->address = chunk_address;
                }
                this->size += chunk_size;
            }

            template <typename F>
            void finish(F&& f)
            {
                if (this->size != 0)
                {
                    f(this->address, this->size);
                    this->size = 0;
                }
            }
        };

        enum class tracked_access : uint8_t
        {
            none,
            read_only,
            full,
        };

        // The guest runs natively with guest VA == host VA, so there is no per-instruction hook to
        // intercept a specific address. The only MMIO consumer here is KUSER_SHARED_DATA at 0x7ffe0000,
        // which falls inside Darwin's mandatory __PAGEZERO segment and cannot be backed by real memory
        // (confirmed via both mmap(MAP_FIXED) and mach_vm_allocate), so the region is left unmapped and
        // every access faults into handle_mmio_fault. Read-only, matching real Windows: a guest write is
        // left to surface as a normal access violation. Not a general MMIO implementation.
        struct mmio_region
        {
            uint64_t address = 0;
            size_t size = 0;
            mmio_read_callback read_cb;
        };

        struct hook_entry
        {
            x86_hookable_instructions type = x86_hookable_instructions::invalid;
            instruction_hook_callback callback;
        };

#ifdef __APPLE__
        bool sysctl_flag(const char* name)
        {
            int32_t value = 0;
            size_t size = sizeof(value);
            if (::sysctlbyname(name, &value, &size, nullptr, 0) != 0)
            {
                return false;
            }
            return value != 0;
        }

        // Replaces FEXCore's own FetchHostFeatures(), which is Linux-only: it reads MIDR_EL1, neither
        // EL0-readable nor trap-emulated on Darwin. Anything not confirmed present in
        // hw.optional.arm.*, or without a clear ARM-feature mapping, is left false - that costs codegen
        // quality, never correctness.
        FEXCore::HostFeatures fetch_host_features_apple()
        {
            FEXCore::HostFeatures features{};

            uint64_t cache_line_size = 64;
            size_t cache_line_size_len = sizeof(cache_line_size);
            ::sysctlbyname("hw.cachelinesize", &cache_line_size, &cache_line_size_len, nullptr, 0);
            features.DCacheLineSize = static_cast<uint32_t>(cache_line_size);
            features.ICacheLineSize = static_cast<uint32_t>(cache_line_size);
            features.SupportsCacheMaintenanceOps = true;

            features.SupportsAES = sysctl_flag("hw.optional.arm.FEAT_AES");
            features.SupportsCRC = sysctl_flag("hw.optional.arm.FEAT_CRC32");
            features.SupportsAtomics = sysctl_flag("hw.optional.arm.FEAT_LSE");
            features.SupportsRCPC = sysctl_flag("hw.optional.arm.FEAT_LRCPC");
            features.SupportsRAND = sysctl_flag("hw.optional.arm.FEAT_RNG");
            features.SupportsSHA = sysctl_flag("hw.optional.arm.FEAT_SHA1") && sysctl_flag("hw.optional.arm.FEAT_SHA256");
            features.SupportsPMULL_128Bit = sysctl_flag("hw.optional.arm.FEAT_PMULL");
            features.SupportsCSSC = sysctl_flag("hw.optional.arm.FEAT_CSSC");
            features.SupportsFCMA = sysctl_flag("hw.optional.arm.FEAT_FCMA");
            features.SupportsFlagM = sysctl_flag("hw.optional.arm.FEAT_FlagM");
            features.SupportsFlagM2 = sysctl_flag("hw.optional.arm.FEAT_FlagM2");
            features.SupportsRPRES = sysctl_flag("hw.optional.arm.FEAT_RPRES");
            features.SupportsFRINTTS = sysctl_flag("hw.optional.arm.FEAT_FRINTTS");
            features.SupportsECV = sysctl_flag("hw.optional.arm.FEAT_ECV");
            features.SupportsWFXT = sysctl_flag("hw.optional.arm.FEAT_WFxT");
            features.SupportsAFP = sysctl_flag("hw.optional.arm.FEAT_AFP");
            features.SupportsMOPS = sysctl_flag("hw.optional.arm.FEAT_MOPS");

            // No Apple Silicon hardware supports SVE/SVE2 as of this writing.
            features.SupportsSVE128 = false;
            features.SupportsSVE256 = false;
            // AVX can make FEX emit paired SIMD loads, which MMIO emulation doesn't support yet.
            features.SupportsAVX = false;
            features.SupportsAES256 = features.SupportsAES && features.SupportsAVX;
            features.SupportsSVEBitPerm = false;

            // TPIDRRO_EL0 is not confirmed to carry a CPU index on Darwin, and DEF_OP(ProcessorID)
            // treats the non-TPIDRRO fallback as unsupported (matching the Windows/wine precedent), so
            // leaving this false makes a guest RDTSCP/RDPID hard-error rather than read garbage.
            features.SupportsCPUIndexInTPIDRRO = false;

            // FEXCore's CPUID brand-string leaves (0x80000002-4) index PerCPUData, sized from CPUMIDRs;
            // an empty CPUMIDRs null-derefs the leaf's ProductName. Linux populates it from per-core
            // MIDR_EL1, unavailable from EL0 on Darwin, so the host logical-CPU count is reported with a
            // placeholder MIDR of 0: M-series parts are absent from FEXCore's MIDR table anyway, so it
            // resolves to FEXCore's own "Unknown ARM CPU" fallback while keeping the guest-visible core
            // count realistic.
            uint32_t logical_cpus = 1;
            size_t logical_cpus_len = sizeof(logical_cpus);
            if (::sysctlbyname("hw.logicalcpu", &logical_cpus, &logical_cpus_len, nullptr, 0) != 0 || logical_cpus == 0)
            {
                logical_cpus = 1;
            }
            features.CPUMIDRs.assign(logical_cpus, 0u);

            return features;
        }
#endif

        // sogen runs one FEX-backed guest thread per process, so a single active-instance pointer is
        // enough for the signal handler below to reach the emulator's hook tables and thread state.
        // Signal handlers cannot be non-static member functions, hence the indirection.
        fex_x86_64_emulator* g_active_emulator = nullptr;

#if SOGEN_FEX_HYPERVISOR
        // The instance whose VM shares FEX's memory; FEXCore's dontneed hook is a plain function pointer.
        fex_x86_64_emulator* g_hypervisor_emulator = nullptr;
        void drop_from_hypervisor(void* address, size_t size);
#endif

        void fault_signal_handler(int sig, siginfo_t* info, void* raw_ucontext);

        // FEXCore's Break stubs use SIGILL for HLT/UDF and SIGTRAP for BRK, including x86 INT3, so both
        // must reach the same host-fault dispatcher as SIGSEGV/SIGBUS.
        constexpr std::array<int, 4> fault_signals = {SIGSEGV, SIGBUS, SIGILL, SIGTRAP};

        // The handlers installed before the first emulator's, which get the faults this backend does not handle. On
        // Android that is the crash reporter writing a tombstone.
        std::array<struct sigaction, fault_signals.size()> g_previous_fault_actions{};
        bool g_previous_fault_actions_saved = false;

        void install_fault_signal_handlers(fex_x86_64_emulator& emulator)
        {
            // Fault routing is process-global (one sigaction handler set, one active-instance
            // pointer). A second live instance would silently receive the first one's faults, so
            // refuse it outright - reachable e.g. via the Python bindings constructing two emulators.
            if (g_active_emulator != nullptr)
            {
                throw std::runtime_error("Only one FEX emulator instance can be active per process");
            }

            g_active_emulator = &emulator;

            // A dedicated alternate stack, so a second signal arriving while this handler is already
            // running does not nest on the faulting thread's potentially near-exhausted stack.
            static std::array<std::byte, 64 * 1024> alt_stack{};
            stack_t stack{};
            stack.ss_sp = alt_stack.data();
            stack.ss_size = alt_stack.size();
            ::sigaltstack(&stack, nullptr);

            struct sigaction action{};
            action.sa_sigaction = fault_signal_handler;
            action.sa_flags = SA_SIGINFO | SA_ONSTACK;
            sigemptyset(&action.sa_mask);

            // FEXCore's IR "Break" op models x86 HLT/UD2/INT3/INT1/INTO uniformly via distinct native
            // traps chosen per Dispatcher.cpp's GuestSignal_SIG* stubs: HLT/UDF raise SIGILL, BRK raises
            // SIGTRAP. INT3 (DebugBreak()) goes through the SIGTRAP stub, so without a handler here that
            // BRK is an unhandled hardware trap terminating the process, instead of reaching the vector
            // dispatch below, which handles vector 3 correctly.
            for (size_t i = 0; i < fault_signals.size(); ++i)
            {
                ::sigaction(fault_signals[i], &action, g_previous_fault_actions_saved ? nullptr : &g_previous_fault_actions[i]);
            }
            g_previous_fault_actions_saved = true;
        }

#ifdef __APPLE__
        // Arm64JITCore::ExitFunctionLink (JIT.cpp) patches an already-compiled call site once its target
        // block is known, writing straight into a MAP_JIT code buffer without a JITWriteScope, because
        // it is written for Linux, which has no per-thread W^X state. Pointers.ExitFunctionLink is a
        // plain function-pointer slot JIT code calls through (JIT.cpp's InitThreadPointers), so it can
        // be wrapped here instead of patching deps/FEX.
        uint64_t g_original_exit_function_link = 0;

        uint64_t exit_function_link_jit_write_wrapper(FEXCore::Core::CpuStateFrame* frame, void* record)
        {
            using exit_function_link_fn = uint64_t (*)(FEXCore::Core::CpuStateFrame*, void*);
            const auto real = reinterpret_cast<exit_function_link_fn>(g_original_exit_function_link);

            // pthread_jit_write_protect_np is per-thread and exclusive with execute permission on this
            // thread's MAP_JIT pages: leaving it disabled past this call would fault the next guest
            // instruction fetch on this thread, not just widen an otherwise-harmless window.
            ::pthread_jit_write_protect_np(0);
            const uint64_t result = real(frame, record);
            ::pthread_jit_write_protect_np(1);
            return result;
        }

        // Under guest VA == host VA, FEXCore's own internal buffers (obtained via a raw ::mmap(NULL,
        // ...), uncoordinated with sogen's bookkeeping) can land inside the guest's own address space,
        // so an ordinary guest write into a buffer placed there corrupts FEXCore's state (the
        // long-standing __tree_balance_after_insert / AddBlockLink corruption).
        //
        // So one large host arena is reserved up front and registered as a reserved_host_range for a
        // two-way exclusion, and every FEXCore::Allocator::mmap(nullptr, ...) is satisfied from inside
        // it. Non-executable requests use MAP_FIXED; executable (MAP_JIT) ones cannot - Apple rejects
        // MAP_JIT|MAP_FIXED with EINVAL - so the sub-region is unmapped first and MAP_JIT gets the
        // hole's address as a non-fixed hint, reliable since nothing else competes for space inside the
        // arena. The result is verified to land inside the arena and fails loudly rather than silently
        // falling back to an unconstrained mapping that would reintroduce the hazard.
        class fex_internal_arena
        {
          public:
            // Pure VA reservation (PROT_NONE) until sub-regions are committed, sized to exceed any
            // realistic FEXCore-internal need for a full application workload.
            static constexpr size_t arena_size = 0x1'0000'0000ULL; // 4 GiB

            static fex_internal_arena& instance()
            {
                static fex_internal_arena arena;
                return arena;
            }

            // Must run before the first FEXCore-internal allocation (context/CodeBuffer creation) and
            // before the memory manager first queries reserved_host_ranges(). Idempotent. With data_only,
            // executable requests get plain data memory: FEX's code then runs inside a VM, never on the
            // host, and MAP_JIT memory must not reach hv_vm_map (it panics the machine).
            void install(const bool data_only)
            {
                if (this->base_ != 0)
                {
                    if (data_only != this->data_only_)
                    {
                        throw std::runtime_error("In-process and hypervisor FEX cannot share a process");
                    }
                    return;
                }
                this->data_only_ = data_only;

                void* base = ::mmap(nullptr, arena_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                if (base == MAP_FAILED)
                {
                    throw std::runtime_error("Failed to reserve FEXCore-internal host arena");
                }

                this->base_ = reinterpret_cast<uintptr_t>(base);
                this->cursor_ = this->base_;

                FEXCore::Allocator::mmap = &fex_internal_arena::hook_mmap;
                FEXCore::Allocator::munmap = &fex_internal_arena::hook_munmap;
            }

            uintptr_t base() const
            {
                return this->base_;
            }

            bool active() const
            {
                return this->base_ != 0;
            }

            // Told before arena memory is released, so a VM sharing it can drop the pages first.
            void set_release_hook(std::function<void(uint64_t, size_t)> hook)
            {
                std::lock_guard<std::mutex> guard(this->lock_);
                this->release_hook_ = std::move(hook);
            }

          private:
            uintptr_t base_ = 0;
            bool data_only_ = false;
            std::function<void(uint64_t, size_t)> release_hook_{};
            uintptr_t cursor_ = 0; // bump pointer, monotonically increasing within the arena
            std::mutex lock_;

            struct free_block
            {
                uintptr_t addr;
                size_t size;
            };

            std::vector<free_block> free_list_;

            bool owns(const void* p) const
            {
                const auto a = reinterpret_cast<uintptr_t>(p);
                return this->base_ != 0 && a >= this->base_ && a < this->base_ + arena_size;
            }

            // Returns 0 on exhaustion. Caller holds lock_.
            uintptr_t reserve(const size_t size)
            {
                for (auto it = this->free_list_.begin(); it != this->free_list_.end(); ++it)
                {
                    if (it->size >= size)
                    {
                        const auto addr = it->addr;
                        if (it->size > size)
                        {
                            it->addr += size;
                            it->size -= size;
                        }
                        else
                        {
                            this->free_list_.erase(it);
                        }
                        return addr;
                    }
                }

                if (this->cursor_ + size > this->base_ + arena_size)
                {
                    return 0;
                }
                const auto addr = this->cursor_;
                this->cursor_ += size;
                return addr;
            }

            void* allocate(void* addr, size_t length, int prot, int flags, int fd, off_t offset)
            {
                if (this->data_only_)
                {
                    prot &= ~PROT_EXEC;
                    flags &= ~MAP_JIT;
                }

                // Only kernel-choose anonymous requests need steering into the arena. Fixed-address or
                // file-backed requests are honored verbatim - FEXCore issues a fixed-address request
                // only where it deliberately targets a specific base it has already reserved itself.
                if (addr != nullptr || (flags & MAP_ANONYMOUS) == 0)
                {
                    return ::mmap(addr, length, prot, flags, fd, offset);
                }

                const size_t rounded = host_page_align_up_apple(length);

                std::lock_guard<std::mutex> guard(this->lock_);
                const uintptr_t slot = this->reserve(rounded);
                if (slot == 0)
                {
                    fprintf(stderr,
                            "[FEX backend] FATAL: FEXCore-internal host arena exhausted (%zu MiB); "
                            "refusing an unconstrained host mmap that could alias guest memory\n",
                            arena_size >> 20);
                    errno = ENOMEM;
                    return MAP_FAILED;
                }

                if (flags & MAP_JIT)
                {
                    // FEXCore's CodeBuffer sizes its request to include a trailing guard page
                    // (CPUBackend.h's UsableSize(): AllocatedSize - FEX_HOST_PAGE_SIZE) and
                    // mprotect(PROT_NONE)s that page itself. That always fails on Apple Silicon with
                    // EACCES - MAP_JIT protection is fixed at mmap() time and cannot be adjusted
                    // afterwards - which is the "Failed to mprotect last page of code buffer" diagnostic
                    // CPUBackend.cpp logs. A real guard is provided instead: only the leading portion
                    // becomes the executable MAP_JIT mapping and the trailing page stays part of the
                    // arena's permanent PROT_NONE reservation, faulting on any access exactly where
                    // UsableSize() expects the buffer to end. Skipped at or below one host page so a
                    // small allocation (the Dispatcher's guardless buffer) is not shrunk into uselessness.
                    const size_t exec_size = rounded > host_page_size_apple ? rounded - host_page_size_apple : rounded;

                    // MAP_JIT | MAP_FIXED is rejected on Apple, so punch a hole in the arena and place
                    // the executable mapping there via a (non-fixed) address hint the kernel honors.
                    ::munmap(reinterpret_cast<void*>(slot), exec_size);
                    void* result = ::mmap(reinterpret_cast<void*>(slot), exec_size, prot, flags, fd, offset);
                    if (result == reinterpret_cast<void*>(slot))
                    {
                        return result;
                    }

                    // The kernel didn't honor the hint. A mapping outside the arena would reintroduce
                    // the aliasing hazard, so fail loudly instead of handing it back.
                    if (result != MAP_FAILED)
                    {
                        ::munmap(result, exec_size);
                    }
                    // Restore the arena's PROT_NONE reservation over the whole slot (including the guard
                    // portion) so it never becomes an unmapped gap the guest could be handed.
                    if (::mmap(reinterpret_cast<void*>(slot), rounded, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) ==
                        reinterpret_cast<void*>(slot))
                    {
                        this->free_list_.push_back({.addr = slot, .size = rounded});
                    }
                    fprintf(stderr, "[FEX backend] FATAL: could not place MAP_JIT code buffer inside the FEXCore arena\n");
                    errno = ENOMEM;
                    return MAP_FAILED;
                }

                // Non-executable: commit directly over the reserved region.
                return ::mmap(reinterpret_cast<void*>(slot), rounded, prot, flags | MAP_FIXED, fd, offset);
            }

            int release(void* addr, size_t length)
            {
                if (!this->owns(addr))
                {
                    return ::munmap(addr, length);
                }

                const size_t rounded = host_page_align_up_apple(length);

                std::lock_guard<std::mutex> guard(this->lock_);
                if (this->release_hook_)
                {
                    this->release_hook_(reinterpret_cast<uint64_t>(addr), rounded);
                }
                // Return the region to the reserved (PROT_NONE) state so it stays part of the arena's
                // contiguous reservation, and record it for reuse. Arena VA is never returned to the OS.
                void* r = ::mmap(addr, rounded, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
                if (r == addr)
                {
                    this->free_list_.push_back({.addr = reinterpret_cast<uintptr_t>(addr), .size = rounded});
                    return 0;
                }
                return r == MAP_FAILED ? -1 : 0;
            }

            static void* hook_mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset)
            {
                return instance().allocate(addr, length, prot, flags, fd, offset);
            }

            static int hook_munmap(void* addr, size_t length)
            {
                return instance().release(addr, length);
            }
        };
#endif

#ifdef __linux__
        template <typename Context>
        Context* find_aarch64_context(ucontext_t* context, uint32_t magic)
        {
            auto* current = reinterpret_cast<_aarch64_ctx*>(context->uc_mcontext.__reserved);
            const auto* end = context->uc_mcontext.__reserved + sizeof(context->uc_mcontext.__reserved);
            while (reinterpret_cast<const uint8_t*>(current) + sizeof(*current) <= end && current->size >= sizeof(*current))
            {
                if (current->magic == magic && current->size >= sizeof(Context))
                {
                    return reinterpret_cast<Context*>(current);
                }
                current = reinterpret_cast<_aarch64_ctx*>(reinterpret_cast<uint8_t*>(current) + current->size);
            }
            return nullptr;
        }

        bool is_write_fault(ucontext_t* context)
        {
            constexpr uint64_t esr_write_not_read = 1ULL << 6;
            const auto* esr = find_aarch64_context<esr_context>(context, ESR_MAGIC);
            return esr != nullptr && (esr->esr & esr_write_not_read) != 0;
        }

        // arm64 kernels with 4KB pages have 39 or 48 virtual address bits, and ones with 64KB pages 42 or 52, but
        // mmap hands out nothing above 47 bits unless asked to.
        uint64_t find_host_address_end()
        {
            for (const int bits : {47, 42})
            {
                const uint64_t end = uint64_t{1} << bits;
                auto* probe = reinterpret_cast<void*>(end - page_size);
                void* result =
                    ::mmap(probe, page_size, PROT_NONE, MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
                if (result != MAP_FAILED)
                {
                    ::munmap(result, page_size);
                }
                if (result == probe || (result == MAP_FAILED && errno == EEXIST))
                {
                    return end;
                }
            }

            return uint64_t{1} << 39;
        }
#endif

    }

    class fex_x86_64_emulator;

    // Bridges FEX's guest `syscall` exits to the registered instruction hook. Method bodies are out of
    // line, after fex_x86_64_emulator is complete, since they touch its internals.
    class fex_syscall_handler final : public FEXCore::HLE::SyscallHandler
    {
      public:
        explicit fex_syscall_handler(fex_x86_64_emulator& emulator)
            : emulator_(emulator)
        {
            // OS_GENERIC: FEX does no JIT-side syscall argument handling and spills/fills all registers,
            // which is what we want since the syscall is serviced entirely by sogen's own hook.
            this->OSABI = FEXCore::HLE::SyscallOSABI::OS_GENERIC;
        }

        uint64_t HandleSyscall(FEXCore::Core::CpuStateFrame* frame, FEXCore::HLE::SyscallArguments* args) override;
        FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(FEXCore::Core::InternalThreadState* thread, uint64_t address) override;
        void MarkGuestExecutableRange(FEXCore::Core::InternalThreadState* thread, uint64_t start, uint64_t length) override;
        std::optional<FEXCore::ExecutableFileSectionInfo> LookupExecutableFileSection(FEXCore::Core::InternalThreadState* thread,
                                                                                      uint64_t guest_addr) override;

      private:
        fex_x86_64_emulator& emulator_;
    };

    class fex_vcpu final : public x86_64_cpu
    {
      public:
        explicit fex_vcpu(fex_x86_64_emulator& emulator)
            : emulator_(emulator)
        {
        }

        size_t index() const override
        {
            return 0;
        }

        memory_interface& memory() override;
        const memory_interface& memory() const override;

        bool read_descriptor_table(int reg, descriptor_table_register& table) override
        {
            // FEX is a user-mode emulator: there is no real IDT, and the GDT is synthesized internally.
            // Only report the GDT base we were handed via load_gdt(); everything else is unsupported.
            if (reg == static_cast<int>(x86_register::gdtr))
            {
                table.base = this->gdt_base_;
                table.limit = this->gdt_limit_;
                return true;
            }
            return false;
        }

        void start(size_t count) override;

        void stop() override
        {
            this->stop_requested_ = true;
            this->request_thread_stop();
        }

        size_t read_raw_register(int reg, void* value, size_t size) override
        {
            const auto xreg = static_cast<x86_register>(reg);
            const auto mapping = detail::map_register(xreg);
            auto& state = this->cpu_state();

            switch (mapping.kind)
            {
            case detail::register_kind::gpr: {
                uint64_t raw = state.gregs[mapping.gpr.index] >> (mapping.gpr.byte_offset * 8);
                std::memcpy(value, &raw, (std::min)(size, mapping.gpr.width));
                return size;
            }
            case detail::register_kind::rip:
                std::memcpy(value, &state.rip, (std::min)(size, sizeof(state.rip)));
                return size;
            case detail::register_kind::flags: {
                const uint64_t rflags = this->read_rflags();
                std::memcpy(value, &rflags, (std::min)(size, sizeof(rflags)));
                return size;
            }
            case detail::register_kind::xmm:
                // Low 128 bits of the (possibly AVX) vector register.
                std::memcpy(value, &state.xmm.avx.data[mapping.index][0], (std::min)(size, size_t{16}));
                return size;
            case detail::register_kind::mm:
                std::memcpy(value, &state.mm[mapping.index][0], (std::min)(size, size_t{16}));
                return size;
            case detail::register_kind::mxcsr:
                std::memcpy(value, &state.mxcsr, (std::min)(size, sizeof(state.mxcsr)));
                return size;
            case detail::register_kind::fcw:
                std::memcpy(value, &state.FCW, (std::min)(size, sizeof(state.FCW)));
                return size;
            case detail::register_kind::fs_base:
                std::memcpy(value, &state.fs_cached, (std::min)(size, sizeof(state.fs_cached)));
                return size;
            case detail::register_kind::gs_base:
                std::memcpy(value, &state.gs_cached, (std::min)(size, sizeof(state.gs_cached)));
                return size;
            case detail::register_kind::segment: {
                const uint16_t selector = this->segment_selector(mapping.index);
                std::memcpy(value, &selector, (std::min)(size, sizeof(selector)));
                return size;
            }
            case detail::register_kind::fsw:
            case detail::register_kind::unsupported:
            default:
                // Unknown/unsupported register: report zeroed value rather than throwing, matching the
                // lenient behavior of the other backends for rarely-used registers.
                std::memset(value, 0, size);
                return size;
            }
        }

        size_t write_raw_register(int reg, const void* value, size_t size) override
        {
            const auto xreg = static_cast<x86_register>(reg);
            const auto mapping = detail::map_register(xreg);
            auto& state = this->cpu_state();

            switch (mapping.kind)
            {
            case detail::register_kind::gpr: {
                auto& slot = state.gregs[mapping.gpr.index];
                if (mapping.gpr.width == 8)
                {
                    std::memcpy(&slot, value, sizeof(slot));
                }
                else if (mapping.gpr.zero_extend_32)
                {
                    uint32_t v = 0;
                    std::memcpy(&v, value, sizeof(v));
                    slot = v; // 32-bit writes clear the high 32 bits
                }
                else
                {
                    uint64_t incoming = 0;
                    std::memcpy(&incoming, value, mapping.gpr.width);
                    const auto shift = mapping.gpr.byte_offset * 8;
                    const uint64_t mask = ((1ULL << (mapping.gpr.width * 8)) - 1) << shift;
                    slot = (slot & ~mask) | ((incoming << shift) & mask);
                }
                return size;
            }
            case detail::register_kind::rip:
                std::memcpy(&state.rip, value, (std::min)(size, sizeof(state.rip)));
                return size;
            case detail::register_kind::flags: {
                uint64_t rflags = 0;
                std::memcpy(&rflags, value, (std::min)(size, sizeof(rflags)));
                this->write_rflags(rflags);
                return size;
            }
            case detail::register_kind::xmm:
                std::memcpy(&state.xmm.avx.data[mapping.index][0], value, (std::min)(size, size_t{16}));
                return size;
            case detail::register_kind::mm:
                std::memcpy(&state.mm[mapping.index][0], value, (std::min)(size, size_t{16}));
                return size;
            case detail::register_kind::mxcsr:
                std::memcpy(&state.mxcsr, value, (std::min)(size, sizeof(state.mxcsr)));
                return size;
            case detail::register_kind::fcw:
                std::memcpy(&state.FCW, value, (std::min)(size, sizeof(state.FCW)));
                return size;
            case detail::register_kind::fs_base:
                std::memcpy(&state.fs_cached, value, (std::min)(size, sizeof(state.fs_cached)));
                return size;
            case detail::register_kind::gs_base:
                std::memcpy(&state.gs_cached, value, (std::min)(size, sizeof(state.gs_cached)));
                return size;
            case detail::register_kind::segment:
                this->set_segment_selector(mapping.index, value, size);
                return size;
            case detail::register_kind::fsw:
            case detail::register_kind::unsupported:
            default:
                return size;
            }
        }

        std::vector<std::byte> save_registers() const override;
        void restore_registers(const std::vector<std::byte>& register_data) override;

        bool has_violation() const override
        {
            return false;
        }

        bool supports_instruction_counting() const override
        {
            return true;
        }

        // request_thread_stop() mprotects InterruptFaultPage to PROT_NONE, which is safe to call from
        // any host thread - the software-quantum watchdog thread in windows_emulator::start() relies on
        // exactly this when instruction precision is off.
        bool is_stop_thread_safe() const override
        {
            return true;
        }

        void set_segment_base(x86_register base, pointer_type value) override
        {
            auto& state = this->cpu_state();
            if (base == x86_register::fs || base == x86_register::fs_base)
            {
                state.fs_cached = value;
            }
            else if (base == x86_register::gs || base == x86_register::gs_base)
            {
                state.gs_cached = value;
            }
        }

        pointer_type get_segment_base(x86_register base) override
        {
            const auto& state = this->cpu_state();
            if (base == x86_register::fs || base == x86_register::fs_base)
            {
                return state.fs_cached;
            }
            if (base == x86_register::gs || base == x86_register::gs_base)
            {
                return state.gs_cached;
            }
            return 0;
        }

        void notify_process_bitness(bool is_wow64_process) override;
        void load_gdt(pointer_type address, uint32_t limit) override;

      private:
        friend class fex_x86_64_emulator;
        friend class fex_syscall_handler;

        // CPUState is owned by the thread frame once a thread exists. Before the thread is created we
        // stage register accesses in a local CPUState so the Windows loader can set up the initial
        // context; create_thread() seeds the real thread from it.
        FEXCore::Core::CPUState& cpu_state()
        {
            if (this->thread_ != nullptr)
            {
                return this->thread_->CurrentFrame->State; // TODO(fex): confirm field path for the FEX version.
            }
            return this->staged_state_;
        }

        const FEXCore::Core::CPUState& cpu_state() const
        {
            if (this->thread_ != nullptr)
            {
                return this->thread_->CurrentFrame->State;
            }
            return this->staged_state_;
        }

        uint64_t read_rflags() const;
        void write_rflags(uint64_t rflags);

        uint16_t segment_selector(int index) const
        {
            const auto& state = this->cpu_state();
            switch (index)
            {
            case 0:
                return state.es_idx;
            case 1:
                return state.cs_idx;
            case 2:
                return state.ss_idx;
            case 3:
                return state.ds_idx;
            case 4:
                return state.fs_idx;
            case 5:
                return state.gs_idx;
            default:
                return 0;
            }
        }

        void set_segment_selector(int index, const void* value, size_t size)
        {
            uint16_t selector = 0;
            std::memcpy(&selector, value, (std::min)(size, sizeof(selector)));
            auto& state = this->cpu_state();
            switch (index)
            {
            case 0:
                state.es_idx = selector;
                break;
            case 1:
                state.cs_idx = selector;
                break;
            case 2:
                state.ss_idx = selector;
                break;
            case 3:
                state.ds_idx = selector;
                break;
            case 4:
                state.fs_idx = selector;
                break;
            case 5:
                state.gs_idx = selector;
                break;
            default:
                break;
            }
        }

        void request_thread_stop()
        {
            // Forces the in-flight ExecuteThread to return, from this thread or another. With
            // Config.NeedsPendingInterruptFaultCheck set (see initialize_context's CONFIG_GDBSERVER
            // comment) the JIT emits a `str zr, [InterruptFaultPage]` at every block entry, so
            // protecting that page makes the next entry fault into handle_fault_signal, which redirects
            // it to ThreadStopHandlerAddress without consulting stop_requested_.
            if (this->thread_ == nullptr)
            {
                return;
            }

            ::mprotect(this->thread_->InterruptFaultPage, sizeof(this->thread_->InterruptFaultPage), PROT_NONE);
        }

#if SOGEN_FEX_HYPERVISOR
        void prepare_vm_thread();
        void set_interrupt_page_in_vm(bool armed);
        bool in_interrupt_page(uint64_t address) const;
        void start_in_vm();
        void run_dispatcher_in_vm();

        bool returned_to_host_{};
#endif

        fex_x86_64_emulator& emulator_;
        FEXCore::Core::InternalThreadState* thread_ = nullptr;
        FEXCore::Core::CPUState staged_state_{};

        uint64_t gdt_base_ = 0;
        uint32_t gdt_limit_ = 0;
        // 0x23: 32-bit code, 0x2B: data, 0x33: 64-bit code.
        std::array<uint64_t, 8> default_gdt_{0, 0, 0, 0, 0x00CFFB000000FFFFull, 0x00CFF3000000FFFFull, 0x00AFFB000000FFFFull, 0};

        std::atomic<bool> stop_requested_{false};
    };

    class fex_x86_64_emulator final : public x86_64_emulator
    {
        struct callret_buffer_record
        {
            void* allocation_base = nullptr;
            size_t allocation_size = 0;
            uint64_t code_buffer_generation = 0;
        };

      public:
        explicit fex_x86_64_emulator(const bool in_hypervisor = false)
            : hypervisor_(in_hypervisor)
        {
#ifdef __linux__
            if (static_cast<size_t>(::getpagesize()) != page_size)
            {
                // The FEX backend is currently configured for 4KB host pages on Linux.
                // This is not an inherent requirement and may be relaxed with appropriate support.
                throw std::runtime_error("FEX backend requires 4KB host pages on Linux");
            }
#endif
            this->reserve_guest_arena();
            try
            {
                this->initialize_context();
            }
            catch (...)
            {
                this->release_guest_arena();
                throw;
            }
        }

        ~fex_x86_64_emulator() override
        {
#if SOGEN_FEX_HYPERVISOR
            // The VM goes first: it shares memory that FEX and the guest release below.
            if (this->vm_)
            {
                fex_internal_arena::instance().set_release_hook({});
                FEXCore::Allocator::dontneed = nullptr;
                g_hypervisor_emulator = nullptr;
                this->vm_.reset();
                ::munmap(this->vm_stack_, vm_stack_size);
            }
#endif
            utils::reset_object_with_delayed_destruction(this->memory_read_hooks_);
            utils::reset_object_with_delayed_destruction(this->memory_write_hooks_);
            utils::reset_object_with_delayed_destruction(this->memory_execution_hooks_);
            utils::reset_object_with_delayed_destruction(this->memory_violation_hooks_);
            utils::reset_object_with_delayed_destruction(this->interrupt_hooks_);
            utils::reset_object_with_delayed_destruction(this->basic_block_hooks_);
            utils::reset_object_with_delayed_destruction(this->instruction_hooks_);

            if (this->vcpu_.thread_ != nullptr && this->context_)
            {
                this->context_->DestroyThread(this->vcpu_.thread_);
                this->vcpu_.thread_ = nullptr;
            }

            // Release everything we claimed in the shared host == guest address space.
            for (const auto& [address, size] : this->claimed_host_ranges_)
            {
                this->for_each_host_span(address, size, [](const uint64_t host_address, const size_t span_size, size_t) {
                    ::munmap(reinterpret_cast<void*>(host_address), span_size);
                });
            }

            if (this->low_guest_window_ != 0)
            {
                ::munmap(reinterpret_cast<void*>(this->low_guest_window_), low_guest_window_size);
            }

            this->release_guest_arena();

            if (g_active_emulator == this)
            {
                g_active_emulator = nullptr;
            }

            if (this->callret_allocation_ != nullptr)
            {
                FEXCore::Allocator::munmap(this->callret_allocation_, this->callret_allocation_size_);
            }
        }

        // cpu_interface

        bool read_descriptor_table(int reg, descriptor_table_register& table) override
        {
            return this->vcpu_.read_descriptor_table(reg, table);
        }

        void start(size_t count) override
        {
            this->vcpu_.start(count);
        }

        void stop() override
        {
            this->vcpu_.stop();
        }

        size_t read_raw_register(int reg, void* value, size_t size) override
        {
            return this->vcpu_.read_raw_register(reg, value, size);
        }

        size_t write_raw_register(int reg, const void* value, size_t size) override
        {
            return this->vcpu_.write_raw_register(reg, value, size);
        }

        std::vector<std::byte> save_registers() const override
        {
            return this->vcpu_.save_registers();
        }

        // Preserves the fields that are per-FEXCore-thread-global rather than per-logical-guest-thread:
        // FEXCore rewrites L1Pointer/L1Mask itself whenever the JIT lookup cache reallocates, so the
        // live values are always the correct ones and a stale snapshot must not clobber them. The
        // segment_arrays point at the process-wide descriptor tables that load_gdt installs.
        // callret_sp/_pad1 are handled by ensure_callret_buffer.
        void restore_state_into(FEXCore::Core::InternalThreadState* thread, const FEXCore::Core::CPUState& restored)
        {
            auto& state = thread->CurrentFrame->State;
            const auto l1_pointer = state.L1Pointer;
            const auto l1_mask = state.L1Mask;
            const auto segment_arrays = std::to_array(state.segment_arrays);
            std::memcpy(&state, &restored, sizeof(state));
            state.L1Pointer = l1_pointer;
            state.L1Mask = l1_mask;
            std::ranges::copy(segment_arrays, std::begin(state.segment_arrays));
            this->ensure_callret_buffer(thread, state);
            thread->CallRetStackBase = reinterpret_cast<void*>(state._pad1);

            auto& buffer = this->callret_buffers_.at(state._pad1);
            if (buffer.code_buffer_generation != thread->CodeBufferGeneration)
            {
                constexpr size_t callret_stack_size = FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE;
                FEXCore::Allocator::VirtualDontNeed(thread->CallRetStackBase, callret_stack_size);
                state.callret_sp = state._pad1 + callret_stack_size / 4;
                buffer.code_buffer_generation = thread->CodeBufferGeneration;
            }
        }

        void restore_registers(const std::vector<std::byte>& register_data) override
        {
            this->vcpu_.restore_registers(register_data);
        }

        bool has_violation() const override
        {
            return this->vcpu_.has_violation();
        }

        bool supports_instruction_counting() const override
        {
            return this->vcpu_.supports_instruction_counting();
        }

        bool is_stop_thread_safe() const override
        {
            return this->vcpu_.is_stop_thread_safe();
        }

        // emulator

        std::string get_name() const override
        {
            return this->hypervisor_ ? "FEX in Hypervisor.framework" : "FEX";
        }

        bool supports_multiple_vcpus() const override
        {
            // sogen multiplexes logical guest threads onto a single FEXCore engine, cooperatively
            // scheduled on one host thread - no multi-vCPU support.
            return false;
        }

        x86_64_cpu& get_cpu(const size_t index) override
        {
            if (index >= this->vcpu_count())
            {
                throw std::out_of_range("Invalid vCPU index");
            }

            return this->vcpu_;
        }

        void ensure_restorable_in_new_process() const override
        {
            const auto ensure_restorable = [this](const uint64_t address, const uint64_t size) {
                const bool in_low_window = this->low_guest_window_ != 0 && address + size <= low_guest_window_size;
                const bool in_arena =
                    this->guest_arena_ != 0 && address >= this->guest_arena_ && address + size <= this->guest_arena_ + guest_arena_size;
                const bool fixed = std::ranges::any_of(this->fixed_guest_ranges_, [&](const auto& range) {
                    return address >= range.first && address + size <= range.first + range.second;
                });
                if (!in_low_window && !in_arena && !fixed)
                {
                    std::array<char, 128> message{};
                    snprintf(message.data(), message.size(), "Guest memory at 0x%llx lies where a new process may not get it back",
                             static_cast<unsigned long long>(address));
                    throw std::runtime_error(message.data());
                }
            };

            for (const auto& [address, region] : this->regions_)
            {
                ensure_restorable(address, region.size);
            }

            // Reserved but uncommitted guest memory has no region, and a new process has to claim it as well.
            for (const auto& [address, size] : this->claimed_host_ranges_)
            {
                ensure_restorable(address, size);
            }
        }

        void serialize_state(utils::buffer_serializer& buffer, bool /*is_snapshot*/) const override
        {
            buffer.write_vector(this->save_registers());
            buffer.write(this->guest_arena_);
            buffer.write(this->low_guest_window_ != 0);
            buffer.write(this->vcpu_.gdt_base_);
            buffer.write(this->vcpu_.gdt_limit_);
        }

        void deserialize_state(utils::buffer_deserializer& buffer, bool /*is_snapshot*/) override
        {
            const auto registers = buffer.read_vector<std::byte>();
            if (buffer.read<uint64_t>() != this->guest_arena_)
            {
                throw std::runtime_error("The guest memory window the process was saved with is not available");
            }

            const auto low_guest_window = buffer.read<bool>();
            const auto gdt_base = buffer.read<uint64_t>();
            const auto gdt_limit = buffer.read<uint32_t>();

            if (low_guest_window && this->low_guest_window_ == 0)
            {
                this->enable_low_guest_window();
            }

            // The descriptor table's host address depends on the low window, so it is set again once that exists.
            if (gdt_base != 0)
            {
                this->load_gdt(gdt_base, gdt_limit);
            }
            this->restore_registers(registers);
        }

        // x86_emulator

        void set_segment_base(x86_register base, pointer_type value) override
        {
            this->vcpu_.set_segment_base(base, value);
        }

        pointer_type get_segment_base(x86_register base) override
        {
            return this->vcpu_.get_segment_base(base);
        }

        // FEXCore decodes each block in the mode its CS descriptor selects, so the single 64-bit
        // Context also runs a WoW64 process's compatibility-mode code.
        void notify_process_bitness(bool is_wow64_process) override
        {
            if (is_wow64_process && this->low_guest_window_ == 0)
            {
                this->enable_low_guest_window();
            }
        }

        void load_gdt(pointer_type address, uint32_t limit) override
        {
            this->vcpu_.load_gdt(address, limit);
        }

        // memory_interface (public)

        void read_memory(uint64_t address, void* data, size_t size) const override
        {
            if (!this->try_read_memory(address, data, size))
            {
                throw std::runtime_error("Failed to read FEX guest memory");
            }
        }

        bool try_read_memory(uint64_t address, void* data, size_t size) const override
        {
            if (!this->is_range_mapped(address, size))
            {
                return false;
            }

#ifndef __APPLE__
            // A stale chunk of tracked memory is inaccessible until it is brought up to date, which the fault handler only
            // does for the guest's own accesses.
            this->for_each_tracked_chunk(address, size, [&](tracked_chunk& chunk, const uint64_t chunk_address, const size_t chunk_size) {
                this->require_up_to_date(chunk, chunk_address, chunk_size);
            });
#endif

            // Host-side reads, e.g. of a snapshot, also cover memory the guest made inaccessible to itself.
            const bool needs_temporary_read = !this->range_has_permission(address, size, memory_permission::read);
            if (needs_temporary_read)
            {
                this->set_temporary_access(address, size, memory_permission::read, true);
            }

            // memmove, not memcpy: callers (e.g. the PE loader's section copy) can hand this a source
            // buffer that overlaps the guest destination range - overlapping memcpy is undefined
            // behaviour. This is a generic guest memory-copy primitive with no non-overlap contract.
            this->for_each_host_span(address, size, [&](const uint64_t host_address, const size_t span_size, const size_t offset) {
                std::memmove(static_cast<std::byte*>(data) + offset, reinterpret_cast<const void*>(host_address), span_size);
            });

            if (needs_temporary_read)
            {
                this->set_temporary_access(address, size, memory_permission::read, false);
            }
            return true;
        }

        void write_memory(uint64_t address, const void* data, size_t size) override
        {
            if (!this->try_write_memory(address, data, size))
            {
                std::array<char, 128> buf{};
                snprintf(buf.data(), buf.size(), "Failed to write FEX guest memory at 0x%llx size=0x%zx",
                         static_cast<unsigned long long>(address), size);
                throw std::runtime_error(buf.data());
            }
        }

        bool try_write_memory(uint64_t address, const void* data, size_t size) override
        {
            if (!this->is_range_mapped(address, size))
            {
                return false;
            }

            // sogen's loader writes guest memory it has already declared read-only (a PE section's raw
            // file bytes, regardless of the section's final protection). Unlike Unicorn's uc_mem_write,
            // this backend's guest VA == host VA is backed by real host mprotect state, so the write
            // needs a temporary permission bump. Every page must be checked, not just the first: a write
            // straddling into a read-only region would otherwise fault mid-memmove.
            const bool needs_temporary_write = !this->range_is_writable(address, size);

            if (needs_temporary_write)
            {
                this->set_temporary_access(address, size, memory_permission::write, true);
            }

            // memmove, not memcpy: see try_read_memory - the source may overlap the guest destination.
            this->for_each_host_span(address, size, [&](const uint64_t host_address, const size_t span_size, const size_t offset) {
                std::memmove(reinterpret_cast<void*>(host_address), static_cast<const std::byte*>(data) + offset, span_size);
            });

            if (needs_temporary_write)
            {
                this->set_temporary_access(address, size, memory_permission::write, false);
            }

            if (this->range_has_code(address, size))
            {
                this->invalidate_code_range(address, size);
            }
            return true;
        }

        // hook_interface
        //
        // Memory access and basic-block hooks cannot fire in translated code; they are accepted and tracked (so
        // delete_hook works) purely for API compatibility. Execution hooks run through FEX's per-instruction callback.

        emulator_hook* hook_memory_execution(memory_execution_hook_callback callback) override
        {
            return this->hook_memory_range_execution(0, std::numeric_limits<uint64_t>::max(), std::move(callback));
        }

        emulator_hook* hook_memory_execution(const uint64_t address, memory_execution_hook_callback callback) override
        {
            return this->hook_memory_range_execution(address, 1, std::move(callback));
        }

        emulator_hook* hook_memory_range_execution(const uint64_t address, const uint64_t size,
                                                   memory_execution_hook_callback callback) override
        {
            auto* hook = this->make_hook();
            this->memory_execution_hooks_[hook] = {.start = address, .size = size, .callback = std::move(callback)};
            if (size == 1)
            {
                this->execution_hooks_by_address_[address].push_back(hook);
            }
            else
            {
                this->range_execution_hooks_.push_back(hook);
            }
            this->add_instruction_callback_range(address, size);
            return hook;
        }

        emulator_hook* hook_memory_read(uint64_t /*address*/, uint64_t /*size*/, memory_access_hook_callback callback) override
        {
            auto* hook = this->make_hook();
            this->memory_read_hooks_[hook] = std::move(callback);
            return hook;
        }

        emulator_hook* hook_memory_write(uint64_t /*address*/, uint64_t /*size*/, memory_access_hook_callback callback) override
        {
            auto* hook = this->make_hook();
            this->memory_write_hooks_[hook] = std::move(callback);
            return hook;
        }

        emulator_hook* hook_instruction(int instruction_type, instruction_hook_callback callback) override
        {
            auto* hook = this->make_hook();
            auto& entry = this->instruction_hooks_[hook];
            entry.type = static_cast<x86_hookable_instructions>(instruction_type);
            entry.callback = std::move(callback);
            if (entry.type == x86_hookable_instructions::syscall)
            {
                this->syscall_hook_ = &entry;
            }
            else if (entry.type != x86_hookable_instructions::invalid)
            {
                this->trap_timestamp_and_cpuid();
            }
            return hook;
        }

        emulator_hook* hook_interrupt(interrupt_hook_callback callback) override
        {
            auto* hook = this->make_hook();
            this->interrupt_hooks_[hook] = std::move(callback);
            return hook;
        }

        emulator_hook* hook_memory_violation(memory_violation_hook_callback callback) override
        {
            auto* hook = this->make_hook();
            this->memory_violation_hooks_[hook] = std::move(callback);
            return hook;
        }

        emulator_hook* hook_basic_block(basic_block_hook_callback callback) override
        {
            auto* hook = this->make_hook();
            this->basic_block_hooks_[hook] = std::move(callback);
            return hook;
        }

        void delete_hook(emulator_hook* hook) override
        {
            if (this->syscall_hook_ != nullptr)
            {
                const auto it = this->instruction_hooks_.find(hook);
                if (it != this->instruction_hooks_.end() && &it->second == this->syscall_hook_)
                {
                    this->syscall_hook_ = nullptr;
                }
            }

            this->instruction_hooks_.erase(hook);
            this->interrupt_hooks_.erase(hook);
            this->memory_read_hooks_.erase(hook);
            this->memory_write_hooks_.erase(hook);
            this->delete_execution_hook(hook);
            this->memory_violation_hooks_.erase(hook);
            this->basic_block_hooks_.erase(hook);
        }

        bool supports_global_memory_execution_hooks() const override
        {
            return true;
        }

#ifdef __APPLE__
        // Below 4GB only the low window can back the guest, as __PAGEZERO covers the rest.
        std::vector<host_reserved_range> arena_layout_apple(const uint64_t start, const uint64_t end) const
        {
            std::vector<host_reserved_range> ranges{};
            if (this->low_guest_window_ == 0 && start < low_guest_window_size)
            {
                ranges.push_back({.address = start, .size = static_cast<size_t>(std::min(end, low_guest_window_size) - start)});
            }

            this->add_ranges_outside_guest_arena(ranges, start, end);

#if SOGEN_FEX_HYPERVISOR
            if (this->vm_)
            {
                std::ranges::copy(this->vm_->reserved_host_ranges_in(start, static_cast<size_t>(end - start)), std::back_inserter(ranges));
            }
#endif
            return ranges;
        }

        std::vector<host_reserved_range> reserved_host_ranges() const override
        {
            if (this->guest_arena_ != 0)
            {
                return this->arena_layout_apple(0, guest_address_space_end);
            }

            // Enumerates everything currently mapped in this process via the Mach VM region API, the
            // Darwin equivalent of walking /proc/self/maps.
            std::vector<host_reserved_range> ranges;

            // Every 64-bit Mach-O executable reserves a __PAGEZERO segment spanning at least [0, 4GB),
            // an OS/linker convention enforced at the mmap syscall level rather than a listed VM region
            // mach_vm_region reports: its first real hit starts well above 4GB (ASLR-dependent), yet
            // mapping guest memory anywhere in the gap below still fails. So the whole gap up to the
            // scan's first real region is reserved rather than guessing a fixed size.
            //
            // The FEXCore-internal arena would otherwise be reported as many separate sub-regions
            // (PROT_NONE reservation, committed BlockLinks buffers, the MAP_JIT CodeBuffer, freed
            // holes). Registering it as one range instead covers sub-regions allocated lazily after this
            // one-shot snapshot and any transient holes, with no dependence on the scan's timing.
            const auto& arena = fex_internal_arena::instance();
            const uint64_t arena_base = arena.base();
            const uint64_t arena_end = arena.active() ? arena_base + fex_internal_arena::arena_size : 0;
            if (arena.active())
            {
                ranges.push_back({.address = arena_base, .size = fex_internal_arena::arena_size});
            }

            mach_vm_address_t address = 0;
            bool first_region = true;
            while (true)
            {
                mach_vm_size_t size = 0;
                vm_region_basic_info_data_64_t info{};
                mach_msg_type_number_t info_count = VM_REGION_BASIC_INFO_COUNT_64;
                mach_port_t object_name = MACH_PORT_NULL;
                const kern_return_t result = mach_vm_region(mach_task_self(), &address, &size, VM_REGION_BASIC_INFO_64,
                                                            reinterpret_cast<vm_region_info_t>(&info), &info_count, &object_name);
                if (result != KERN_SUCCESS)
                {
                    break;
                }

                // Sub-regions of the FEXCore-internal arena are covered by the single explicit range
                // pushed above; don't double-report them (harmless but avoids overlap churn).
                if (arena.active() && address >= arena_base && address < arena_end)
                {
                    address += size;
                    continue;
                }

                if (first_region)
                {
                    // With the low window, the guest's low 4GB is backed elsewhere and free for it.
                    const uint64_t gap_start = this->low_guest_window_ != 0 ? low_guest_window_size : 0;
                    if (address > gap_start)
                    {
                        ranges.push_back({.address = gap_start, .size = static_cast<size_t>(address - gap_start)});
                    }
                    first_region = false;
                }

                // AddressSanitizer's sparse shadow map spans tens of GB up to multiple TB per region,
                // placed far above where the guest ever allocates. Feeding those to the memory manager
                // bloats reserved_regions_ into the thousands and stalls process setup, so they are
                // skipped - in instrumented builds only.
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
                constexpr mach_vm_size_t asan_shadow_region_threshold = 0x100000000ULL; // 4 GiB
                if (size >= asan_shadow_region_threshold)
                {
                    address += size;
                    continue;
                }
#endif
#endif

                ranges.push_back({.address = address, .size = static_cast<size_t>(size)});
                address += size;
            }

#if SOGEN_FEX_HYPERVISOR
            if (this->vm_)
            {
                std::ranges::copy(this->vm_->reserved_host_ranges(), std::back_inserter(ranges));
            }
#endif
            return ranges;
        }

        static uint64_t first_host_mapping_apple()
        {
            mach_vm_address_t address = 0;
            mach_vm_size_t size = 0;
            vm_region_basic_info_data_64_t info{};
            mach_msg_type_number_t info_count = VM_REGION_BASIC_INFO_COUNT_64;
            mach_port_t object_name = MACH_PORT_NULL;
            if (mach_vm_region(mach_task_self(), &address, &size, VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&info),
                               &info_count, &object_name) != KERN_SUCCESS)
            {
                return 0;
            }
            return address;
        }

        std::vector<host_reserved_range> reserved_host_ranges_in(uint64_t address, size_t size) const override
        {
            if (this->guest_arena_ != 0)
            {
                return this->arena_layout_apple(address, address + size);
            }

            // mach_vm_region's start-address parameter lets the kernel skip straight to the first region
            // at or above the window, so this visits only regions actually inside it (usually none),
            // instead of re-walking every region in the process - a count that grows unbounded over a
            // long session. The arena and the __PAGEZERO gap are captured by the first full scan at
            // startup and never released, so the only thing a rescan of an otherwise-free window adds is
            // a foreign mapping in a gap an earlier guest unmap returned to the OS.
            std::vector<host_reserved_range> ranges;

            const mach_vm_address_t window_start = address;
            const mach_vm_address_t window_end = window_start + size;

            // Like the full scan: nothing below the process's first mapping can be allocated, not even the slice
            // between __PAGEZERO and the main executable.
            const uint64_t gap_start = this->low_guest_window_ != 0 ? low_guest_window_size : 0;
            const uint64_t gap_end = first_host_mapping_apple();
            if (window_start < gap_end && window_end > gap_start)
            {
                const uint64_t hit_start = std::max<uint64_t>(window_start, gap_start);
                const uint64_t hit_end = std::min<uint64_t>(window_end, gap_end);
                ranges.push_back({.address = hit_start, .size = static_cast<size_t>(hit_end - hit_start)});
            }

            mach_vm_address_t probe = window_start;
            while (probe < window_end)
            {
                mach_vm_address_t region_addr = probe;
                mach_vm_size_t region_size = 0;
                vm_region_basic_info_data_64_t info{};
                mach_msg_type_number_t info_count = VM_REGION_BASIC_INFO_COUNT_64;
                mach_port_t object_name = MACH_PORT_NULL;
                if (mach_vm_region(mach_task_self(), &region_addr, &region_size, VM_REGION_BASIC_INFO_64,
                                   reinterpret_cast<vm_region_info_t>(&info), &info_count, &object_name) != KERN_SUCCESS)
                {
                    break;
                }
                if (region_addr >= window_end)
                {
                    break;
                }

                const uint64_t hit_start = std::max<uint64_t>(region_addr, window_start);
                const uint64_t hit_end = std::min<uint64_t>(region_addr + region_size, window_end);
                ranges.push_back({.address = hit_start, .size = static_cast<size_t>(hit_end - hit_start)});

                probe = region_addr + region_size;
            }

#if SOGEN_FEX_HYPERVISOR
            if (this->vm_)
            {
                std::ranges::copy(this->vm_->reserved_host_ranges_in(address, size), std::back_inserter(ranges));
            }
#endif
            return ranges;
        }
#else
        std::vector<host_reserved_range> reserved_host_ranges() const override
        {
            return this->host_mappings_in(0, guest_address_space_end);
        }

        std::vector<host_reserved_range> reserved_host_ranges_in(const uint64_t address, const size_t size) const override
        {
            return this->host_mappings_in(address, address + size);
        }

        // Guest addresses inside the low window are backed by the window, which only this backend maps into. Above 4GB, the
        // guest only gets the arena.
        std::vector<host_reserved_range> host_mappings_in(uint64_t start, const uint64_t end) const
        {
            if (this->low_guest_window_ != 0)
            {
                start = std::max(start, low_guest_window_size);
            }

            if (start >= end)
            {
                return {};
            }

            if (this->guest_arena_ == 0)
            {
                return read_host_mappings_linux(start, end);
            }

            std::vector<host_reserved_range> ranges{};
            if (start < low_guest_window_size)
            {
                ranges = read_host_mappings_linux(start, std::min(end, low_guest_window_size));
            }

            this->add_ranges_outside_guest_arena(ranges, start, end);
            return ranges;
        }
#endif

      private:
        friend class fex_syscall_handler;
        friend class fex_vcpu;

        // memory_interface (private)

        void map_mmio(uint64_t address, size_t size, mmio_read_callback read_cb, [[maybe_unused]] mmio_write_callback write_cb) override
        {
            // See mmio_region's doc comment - the region stays unmapped so accesses fault and emulate.
            if (!is_page_aligned(address) || !is_page_aligned(size))
            {
                throw std::runtime_error("FEX MMIO mappings must be page aligned");
            }

#if SOGEN_FEX_HYPERVISOR
            // Inside the VM the region is a real page refreshed before every entry, so every kind of load reads it.
            if (this->hypervisor_)
            {
                hvf::map_mmio(*this->vm_, address, size, read_cb, std::move(write_cb));
            }
#endif
#ifndef __APPLE__
            // Claimed PROT_NONE so the host cannot place an accessible mapping here and accesses keep faulting into the MMIO
            // handler. Darwin's __PAGEZERO, or the low guest window, already keeps the host out.
            if (!this->host_range_is_claimed(address, size))
            {
                try
                {
                    this->reserve_guest_address_range(address, size);
                }
                catch (const std::runtime_error&)
                {
                    std::array<char, 128> message{};
                    snprintf(message.data(), message.size(), "Failed to reserve guest MMIO at 0x%llx on the host",
                             static_cast<unsigned long long>(address));
                    throw std::runtime_error(message.data());
                }
            }
#endif
            this->mmio_regions_.emplace_back(mmio_region{.address = address, .size = size, .read_cb = std::move(read_cb)});
        }

        void map_memory(uint64_t address, size_t size, memory_permission permissions) override
        {
            if (!is_page_aligned(address) || !is_page_aligned(size))
            {
                throw std::runtime_error("FEX memory mappings must be page aligned");
            }

#ifdef __APPLE__
            // The host mmap/mprotect calls happen at 16KB granularity via the shadow table (see
            // sync_host_page_apple). The range is claimed before the shadow table changes, so a
            // host_memory_collision leaves the shadow table as it was.
            this->reserve_guest_address_range(address, size);
            this->set_shadow_range_apple(address, size, permissions);
            this->sync_host_pages_covering_apple(address, size);
            this->zero_range_in_shared_host_pages_apple(address, size);
#else
            if (!this->host_range_is_claimed(address, size))
            {
                this->claim_host_range(address, size);
            }
            this->remap_host_claim(address, size, to_prot(permissions));
#endif

            this->erase_region_range(address, size);
            this->regions_[address] = mapped_region{.size = size, .permissions = permissions, .owned = true};
        }

#ifdef __linux__
        uint64_t guest_address_end() const override
        {
            return this->host_address_end_;
        }
#endif

        void reserve_guest_address_range(uint64_t address, size_t size) override
        {
#ifdef __APPLE__
            const uint64_t start = host_page_align_down_apple(address);
            const uint64_t end = host_page_align_up_apple(address + size);
#else
            const uint64_t start = address;
            const uint64_t end = address + size;
#endif

            uint64_t cursor = start;
            while (cursor < end)
            {
                auto next = this->claimed_host_ranges_.upper_bound(cursor);
                if (next != this->claimed_host_ranges_.begin())
                {
                    const auto& previous = *std::prev(next);
                    const uint64_t previous_end = previous.first + previous.second;
                    if (previous_end > cursor)
                    {
                        cursor = std::min(previous_end, end);
                        continue;
                    }
                }

                const uint64_t run_end = next == this->claimed_host_ranges_.end() ? end : std::min(next->first, end);
                this->claim_host_range(cursor, static_cast<size_t>(run_end - cursor));
                cursor = run_end;
            }
        }

        void register_fixed_guest_range(const uint64_t address, const size_t size) override
        {
            if (std::ranges::find(this->fixed_guest_ranges_, std::pair{address, size}) == this->fixed_guest_ranges_.end())
            {
                this->fixed_guest_ranges_.emplace_back(address, size);
            }
        }

        void release_guest_address_range(uint64_t address, uint64_t size) override
        {
            // The caller guarantees the range holds no reserved guest ranges (see memory_interface), so
            // every host page wholly inside it is a stale claim and can go back to the OS. Boundary
            // pages are kept: their outside part may belong to a neighboring, still-live reservation.
#ifdef __APPLE__
            const uint64_t start = host_page_align_up_apple(address);
            const uint64_t end = host_page_align_down_apple(address + size);
#else
            const uint64_t start = address;
            const uint64_t end = address + size;
#endif
            std::vector<host_reserved_range> released;

            auto it = this->claimed_host_ranges_.upper_bound(start);
            if (it != this->claimed_host_ranges_.begin())
            {
                --it;
            }

            for (; it != this->claimed_host_ranges_.end() && it->first < end; ++it)
            {
                const uint64_t hit_start = std::max(it->first, start);
                const uint64_t hit_end = std::min<uint64_t>(it->first + it->second, end);
                if (hit_start < hit_end)
                {
                    released.push_back({.address = hit_start, .size = static_cast<size_t>(hit_end - hit_start)});
                }
            }

            for (const auto& range : released)
            {
                this->release_host_claim(range.address, range.size);
                this->remove_host_claim(range.address, range.size);
            }
        }

        bool supports_host_memory_mapping() const override
        {
            return true;
        }

        bool map_host_memory(uint64_t address, size_t size, void* host_pointer, memory_permission permissions) override
        {
            if (!is_page_aligned(address) || !is_page_aligned(size))
            {
                throw std::runtime_error("FEX host memory mappings must be page aligned");
            }

#ifdef __APPLE__
            const uint64_t host_address = this->to_host_address(address);

            // VM_FLAGS_OVERWRITE replaces the reservation the memory manager put at the target;
            // copy=FALSE creates a second mapping of the caller-owned pages rather than copying them.
            const uint64_t claim_start = host_page_align_down_apple(address);
            const size_t claim_size = static_cast<size_t>(host_page_align_up_apple(address + size) - claim_start);
            if (!this->host_range_is_claimed(claim_start, claim_size))
            {
                this->claim_host_range(claim_start, claim_size);
            }

#if SOGEN_FEX_HYPERVISOR
            if (this->hypervisor_)
            {
                hvf::share_host_memory(*this->vm_, address, size, std::nullopt);
            }
#endif

            mach_vm_address_t target_address = host_address;
            vm_prot_t cur_protection = VM_PROT_NONE;
            vm_prot_t max_protection = VM_PROT_NONE;
            const kern_return_t result = ::mach_vm_remap(mach_task_self(), &target_address, size, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
                                                         mach_task_self(), reinterpret_cast<mach_vm_address_t>(host_pointer), FALSE,
                                                         &cur_protection, &max_protection, VM_INHERIT_NONE);
            if (result != KERN_SUCCESS || target_address != host_address)
            {
                throw std::runtime_error("FEX backend failed to alias host memory into the guest");
            }

            this->set_shadow_range_apple(address, size, permissions);
#else
            const uint64_t source_address = get_untagged_pointer_address(host_pointer);
            std::vector<std::pair<uint64_t, size_t>> aliased_spans{};
            bool mapped = true;
            this->for_each_host_span(address, size, [&](const uint64_t host_address, const size_t span_size, const size_t offset) {
                const uint64_t span_source = source_address + offset;
                if (mapped && host_address != span_source)
                {
                    aliased_spans.emplace_back(host_address, span_size);
                    mapped = alias_shared_mapping(span_source, span_size, host_address);
                }
            });
            if (!mapped)
            {
                // A failed MAP_FIXED or MREMAP_FIXED can already have unmapped the reservation at its target.
                for (const auto& [host_address, span_size] : aliased_spans)
                {
                    replace_host_span(host_address, span_size, PROT_NONE);
                }
                return false;
            }
#endif

#if SOGEN_FEX_HYPERVISOR
            if (this->hypervisor_)
            {
                hvf::share_host_memory(*this->vm_, address, size, reinterpret_cast<void*>(this->to_host_address(address)), permissions);
            }
            else
#endif
            {
                this->for_each_host_span(address, size, [&](const uint64_t host_address, const size_t span_size, size_t) {
                    ::mprotect(reinterpret_cast<void*>(host_address), span_size, to_prot(permissions));
                });
            }
            // owned=false: the memory belongs to the caller; we must not munmap it on teardown.
            this->erase_region_range(address, size);
            this->regions_[address] = mapped_region{.size = size, .permissions = permissions, .owned = false};
            return true;
        }

        uint64_t host_placement_address(const uint64_t address) const override
        {
            return this->to_host_address(address);
        }

        bool host_memory_aliasing_is_coherent() const override
        {
#ifdef __ANDROID__
            return true;
#else
            // Apple Silicon's unified memory makes CPU/GPU coherency for the Metal buffers behind
            // MoltenVK's Vulkan buffers likely, but it is not guaranteed across every Metal storage
            // mode this bridge might use. An unnecessary flush is a harmless no-op, whereas wrongly
            // claiming coherence surfaces as rendering corruption.
            return false;
#endif
        }

#ifndef __APPLE__
        // Inside the low window or the arena, the reserved pages are replaced with a second mapping of the host memory.
        // Without either, host memory can only appear in the guest at its own address.
        bool host_memory_mapping_requires_identity() const override
        {
            return this->low_guest_window_ == 0 && this->guest_arena_ == 0;
        }

        bool host_memory_can_stay_in_place() const override
        {
            return this->low_guest_window_ == 0;
        }
#endif

        void flush_host_memory_cache(const void* host_pointer, size_t size) override
        {
            flush_cache_lines(host_pointer, size);
        }

        static void flush_cache_lines(const void* host_pointer, size_t size)
        {
            if (host_pointer == nullptr || size == 0)
            {
                return;
            }

            // The ARM64 equivalent of the KVM backend's clflushopt+sfence pair: evict the CPU data cache
            // out to memory so a GPU reading the same physical pages non-coherently sees guest writes.
#ifdef __APPLE__
            // Darwin's public API for exactly this ("useful when dealing with cache incoherent devices
            // or DMA" - OSCacheControl.h), preferred over hand-rolled `dc civac`, since EL0 access to
            // cache-maintenance instructions is not something an embedder should assume is permitted.
            ::sys_dcache_flush(const_cast<void*>(host_pointer), size);
#else
            constexpr size_t cache_line_size = 64; // Conservative for all known ARM64 implementations.
            const auto first = reinterpret_cast<uintptr_t>(host_pointer) & ~(cache_line_size - 1);
            const auto last = reinterpret_cast<uintptr_t>(host_pointer) + size;
            for (auto line = first; line < last; line += cache_line_size)
            {
                __asm__ volatile("dc civac, %0" : : "r"(line) : "memory");
            }
            __asm__ volatile("dsb sy" ::: "memory");
#endif
        }

#ifndef __APPLE__
        bool track_access(const uint64_t address, const size_t size) override
        {
            if (size == 0 || address % page_size != 0 || size % page_size != 0 || !this->is_range_mapped(address, size) ||
                !this->range_has_permission(address, size, memory_permission::read_write))
            {
                return false;
            }

            bool overlaps = false;
            this->for_each_tracked_chunk(address, size, [&](tracked_chunk&, uint64_t, size_t) { overlaps = true; });
            if (overlaps)
            {
                return false;
            }

            const size_t chunk_count = (size + tracking_chunk_size - 1) / tracking_chunk_size;
            this->tracked_ranges_.emplace(address, tracked_range{.size = size, .chunks = std::vector<tracked_chunk>(chunk_count)});
            this->require_protection(address, size, tracked_access::read_only);
            return true;
        }

        void take_written_ranges(const uint64_t address, const size_t size,
                                 const std::function<void(uint64_t written_address, size_t written_size)>& f) override
        {
            chunk_run written_run{};
            chunk_run protect_run{};
            const auto protect = [&](const uint64_t run_address, const size_t run_size) {
                this->require_protection(run_address, run_size, tracked_access::read_only);
            };

            this->for_each_tracked_chunk(address, size, [&](tracked_chunk& chunk, const uint64_t chunk_address, const size_t chunk_size) {
                if (!chunk.written.exchange(false))
                {
                    written_run.finish(f);
                    protect_run.finish(protect);
                    return;
                }

                written_run.add(chunk_address, chunk_size, f);
                if (chunk.stale)
                {
                    protect_run.finish(protect);
                }
                else
                {
                    protect_run.add(chunk_address, chunk_size, protect);
                }
            });
            written_run.finish(f);
            protect_run.finish(protect);
        }

        void invalidate_cache_on_access(const uint64_t address, const size_t size) override
        {
            chunk_run run{};
            const auto protect = [&](const uint64_t run_address, const size_t run_size) {
                this->require_protection(run_address, run_size, tracked_access::none);
            };

            this->for_each_tracked_chunk(address, size, [&](tracked_chunk& chunk, const uint64_t chunk_address, const size_t chunk_size) {
                if (chunk.stale.exchange(true))
                {
                    run.finish(protect);
                }
                else
                {
                    run.add(chunk_address, chunk_size, protect);
                }
            });
            run.finish(protect);
        }
#endif

        void unmap_memory(uint64_t address, size_t size) override
        {
            // MMIO regions (see mmio_region's doc comment) were never really mapped at the host level.
            if (std::erase_if(this->mmio_regions_, [address](const mmio_region& region) { return region.address == address; }))
            {
#if SOGEN_FEX_HYPERVISOR
                if (this->hypervisor_)
                {
                    hvf::unmap(*this->vm_, address, size);
                }
#endif
                return;
            }

            this->stop_access_tracking(address, size);

#ifdef __APPLE__
            this->set_shadow_range_apple(address, size, std::nullopt);
            this->sync_host_pages_covering_apple(address, size);
#else
            const auto region = this->find_region_containing(address);
            const bool unmap =
                region == this->regions_.end() || region->second.owned || this->is_in_low_guest_window(this->to_host_address(address));
            if (unmap)
            {
                this->remap_host_claim(address, size, PROT_NONE);
            }
#endif
            this->invalidate_code_range(address, size);
            this->erase_region_range(address, size);
        }

        void apply_memory_protection(uint64_t address, size_t size, memory_permission permissions) override
        {
            // Tracked chunks take the new protection as it is, so writes to them go unnoticed until they are tracked again.
            this->for_each_tracked_chunk(address, size, [&](tracked_chunk& chunk, const uint64_t chunk_address, const size_t chunk_size) {
                this->require_up_to_date(chunk, chunk_address, chunk_size);
                chunk.written = true;
            });

#ifdef __APPLE__
            this->set_shadow_range_apple(address, size, permissions);
            this->sync_host_pages_covering_apple(address, size);
#else
            this->for_each_host_span(address, size, [&](const uint64_t host_address, const size_t span_size, size_t) {
                if (::mprotect(reinterpret_cast<void*>(host_address), span_size, to_prot(permissions)) != 0)
                {
                    throw std::runtime_error("FEX backend failed to change memory protection");
                }
            });
#endif

            this->set_region_range_permissions(address, size, permissions);

            // Permission changes can expose/retract executable code; keep FEX's translation cache honest.
            this->invalidate_code_range(address, size);
        }

        // access tracking

        template <typename F>
        void for_each_tracked_chunk(const uint64_t address, const size_t size, F&& f) const
        {
            const uint64_t end = address + size;
            auto it = this->tracked_ranges_.upper_bound(address);
            if (it != this->tracked_ranges_.begin())
            {
                --it;
            }

            for (; it != this->tracked_ranges_.end() && it->first < end; ++it)
            {
                const uint64_t range_end = it->first + it->second.size;
                if (range_end <= address)
                {
                    continue;
                }

                const uint64_t last = std::min(end, range_end);
                for (size_t index = (std::max(address, it->first) - it->first) / tracking_chunk_size;
                     it->first + (index * tracking_chunk_size) < last; ++index)
                {
                    const uint64_t chunk_address = it->first + (index * tracking_chunk_size);
                    f(it->second.chunks[index], chunk_address,
                      static_cast<size_t>(std::min<uint64_t>(tracking_chunk_size, range_end - chunk_address)));
                }
            }
        }

        // Calls f(address, size, permissions) for each part of a range that one mapped region covers.
        template <typename F>
        void for_each_mapped_piece(const uint64_t address, const size_t size, F&& f) const
        {
            const uint64_t end = address + size;
            uint64_t cursor = address;
            while (cursor < end)
            {
                const auto region = this->find_region_containing(cursor);
                if (region == this->regions_.end())
                {
                    const auto next = this->regions_.upper_bound(cursor);
                    cursor = next == this->regions_.end() ? end : next->first;
                    continue;
                }

                const uint64_t piece_end = std::min(end, region->first + region->second.size);
                f(cursor, static_cast<size_t>(piece_end - cursor), region->second.permissions);
                cursor = piece_end;
            }
        }

        // Applies the guest's protection to the mapped parts of a range, limited to access.
        bool restrict_protection(const uint64_t address, const size_t size, const tracked_access access) const
        {
            bool restricted = true;
            this->for_each_mapped_piece(
                address, size, [&](const uint64_t piece_address, const size_t piece_size, const memory_permission permissions) {
                    int prot = to_prot(permissions);
                    if (access == tracked_access::none)
                    {
                        prot = PROT_NONE;
                    }
                    else if (access == tracked_access::read_only)
                    {
                        prot &= ~PROT_WRITE;
                    }

                    this->for_each_host_span(piece_address, piece_size, [&](const uint64_t host_address, const size_t span_size, size_t) {
                        restricted = ::mprotect(reinterpret_cast<void*>(host_address), span_size, prot) == 0 && restricted;
                    });
                });
            return restricted;
        }

        void require_protection(const uint64_t address, const size_t size, const tracked_access access)
        {
            if (!this->restrict_protection(address, size, access))
            {
                throw std::runtime_error("FEX backend failed to change the protection of tracked memory");
            }
        }

        // Cleans and invalidates a stale chunk's cache lines, and leaves it write-protected unless the chunk is written
        // already, so that a write still marks it written. Cleaning makes the guest's unflushed writes visible as well,
        // but the chunk keeps counting as written so that they are still flushed through the device.
        bool bring_up_to_date(tracked_chunk& chunk, const uint64_t chunk_address, const size_t chunk_size) const
        {
            chunk.stale = false;
            const auto access = chunk.written ? tracked_access::full : tracked_access::read_only;
            if (!this->restrict_protection(chunk_address, chunk_size, access))
            {
                return false;
            }

            // dc civac needs read access, which the guest may have taken away from parts of the chunk.
            this->for_each_mapped_piece(
                chunk_address, chunk_size, [&](const uint64_t piece_address, const size_t piece_size, const memory_permission permissions) {
                    if ((permissions & memory_permission::read) != memory_permission::none)
                    {
                        this->for_each_host_span(piece_address, piece_size,
                                                 [&](const uint64_t host_address, const size_t span_size, size_t) {
                                                     flush_cache_lines(reinterpret_cast<const void*>(host_address), span_size);
                                                 });
                    }
                });
            return true;
        }

        void require_up_to_date(tracked_chunk& chunk, const uint64_t chunk_address, const size_t chunk_size) const
        {
            if (chunk.stale && !this->bring_up_to_date(chunk, chunk_address, chunk_size))
            {
                throw std::runtime_error("FEX backend failed to change the protection of tracked memory");
            }
        }

        // The first access to a stale chunk of tracked memory, or the first write to a clean one: brings the chunk up to
        // date or records the write, and lets the access through. Only the faulting page's protection decides whether a
        // write is allowed, since the guest can change the protection of parts of a chunk.
        bool handle_tracked_memory_fault(const uint64_t host_fault_address, const bool is_write)
        {
            const uint64_t address = this->to_guest_address(host_fault_address);
            bool handled = false;
            this->for_each_tracked_chunk(address, 1, [&](tracked_chunk& chunk, const uint64_t chunk_address, const size_t chunk_size) {
                const auto writable = [&] {
                    return this->range_has_permission(address & ~(page_size - 1), page_size, memory_permission::write);
                };
                if (chunk.stale)
                {
                    if (is_write && writable())
                    {
                        chunk.written = true;
                    }
                    handled = this->bring_up_to_date(chunk, chunk_address, chunk_size);
                }
                else if (writable())
                {
                    chunk.written = true;
                    handled = this->restrict_protection(chunk_address, chunk_size, tracked_access::full);
                }
            });
            return handled;
        }

        void stop_access_tracking(const uint64_t address, const size_t size)
        {
            const uint64_t end = address + size;
            auto it = this->tracked_ranges_.upper_bound(address);
            if (it != this->tracked_ranges_.begin())
            {
                --it;
            }

            while (it != this->tracked_ranges_.end() && it->first < end)
            {
                if (it->first + it->second.size <= address)
                {
                    ++it;
                    continue;
                }

                const uint64_t range_address = it->first;
                const size_t range_size = it->second.size;
                this->for_each_tracked_chunk(range_address, range_size,
                                             [&](tracked_chunk& chunk, const uint64_t chunk_address, const size_t chunk_size) {
                                                 this->require_up_to_date(chunk, chunk_address, chunk_size);
                                             });
                it = this->tracked_ranges_.erase(it);
                this->require_protection(range_address, range_size, tracked_access::full);
            }
        }

        // region bookkeeping

        bool host_range_is_claimed(const uint64_t address, const size_t size) const
        {
            auto it = this->claimed_host_ranges_.upper_bound(address);
            if (it == this->claimed_host_ranges_.begin())
            {
                return false;
            }

            --it;
            return address >= it->first && address + size <= it->first + it->second;
        }

        void add_host_claim(const uint64_t address, const size_t size)
        {
            uint64_t start = address;
            uint64_t end = address + size;
            auto it = this->claimed_host_ranges_.lower_bound(start);

            if (it != this->claimed_host_ranges_.begin())
            {
                const auto previous = std::prev(it);
                if (previous->first + previous->second >= start)
                {
                    start = previous->first;
                    end = std::max(end, previous->first + previous->second);
                    it = this->claimed_host_ranges_.erase(previous);
                }
            }

            while (it != this->claimed_host_ranges_.end() && it->first <= end)
            {
                end = std::max(end, it->first + it->second);
                it = this->claimed_host_ranges_.erase(it);
            }

            this->claimed_host_ranges_.emplace(start, static_cast<size_t>(end - start));
        }

        void remove_host_claim(const uint64_t address, const size_t size)
        {
            auto it = this->claimed_host_ranges_.upper_bound(address);
            if (it == this->claimed_host_ranges_.begin())
            {
                return;
            }

            --it;
            const uint64_t claim_start = it->first;
            const uint64_t claim_end = claim_start + it->second;
            const uint64_t end = address + size;
            if (address < claim_start || end > claim_end)
            {
                return;
            }

            this->claimed_host_ranges_.erase(it);
            if (claim_start < address)
            {
                this->claimed_host_ranges_.emplace(claim_start, static_cast<size_t>(address - claim_start));
            }
            if (end < claim_end)
            {
                this->claimed_host_ranges_.emplace(end, static_cast<size_t>(claim_end - end));
            }
        }

        void claim_host_range(const uint64_t address, const size_t size)
        {
            this->for_each_host_span(address, size, [&](const uint64_t host_address, const size_t span_size, size_t) {
                if (this->is_in_low_guest_window(host_address) || this->is_in_guest_arena(host_address, span_size))
                {
                    return;
                }

#ifdef __APPLE__
                mach_vm_address_t target = host_address;
                const kern_return_t result = ::mach_vm_allocate(mach_task_self(), &target, span_size, VM_FLAGS_FIXED);
                if (result == KERN_NO_SPACE)
                {
                    throw host_memory_collision{};
                }
                if (result != KERN_SUCCESS || target != host_address)
                {
                    std::array<char, 128> message{};
                    snprintf(message.data(), message.size(),
                             "FEX backend failed to reserve guest memory at 0x%llx (host 0x%llx) at the host level",
                             static_cast<unsigned long long>(address), static_cast<unsigned long long>(host_address));
                    throw std::runtime_error(message.data());
                }
                if (::mprotect(reinterpret_cast<void*>(host_address), span_size, PROT_NONE) != 0)
                {
                    ::mach_vm_deallocate(mach_task_self(), target, span_size);
                    throw std::runtime_error("FEX backend failed to protect a guest address reservation");
                }
#else
                void* result = ::mmap(reinterpret_cast<void*>(host_address), span_size, PROT_NONE,
                                      MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
                if (result == MAP_FAILED && errno != EEXIST)
                {
                    throw std::runtime_error("FEX backend failed to reserve guest address range at the host level");
                }
                if (result == MAP_FAILED || reinterpret_cast<uint64_t>(result) != host_address)
                {
                    if (result != MAP_FAILED)
                    {
                        ::munmap(result, span_size);
                    }
                    throw host_memory_collision{};
                }
#endif
            });
            this->add_host_claim(address, size);
        }

        void remap_host_claim(const uint64_t address, const size_t size, const int protection)
        {
            if (!this->host_range_is_claimed(address, size))
            {
                throw std::logic_error("FEX backend cannot replace an unclaimed host range");
            }

            this->for_each_host_span(address, size, [&](const uint64_t host_address, const size_t span_size, size_t) {
                replace_host_span(host_address, span_size, protection);
            });
        }

        static void replace_host_span(const uint64_t host_address, const size_t size, const int protection)
        {
            void* result = ::mmap(reinterpret_cast<void*>(host_address), size, protection,
                                  MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
            if (result == MAP_FAILED || reinterpret_cast<uint64_t>(result) != host_address)
            {
                throw std::runtime_error("FEX backend failed to replace a claimed host range");
            }
        }

        // Inside the low window a released range goes back to being reserved, so no other host allocation can
        // take it while the window still backs the guest's low 4GB.
        void release_host_claim(const uint64_t address, const size_t size)
        {
            this->for_each_host_span(address, size, [&](const uint64_t host_address, const size_t span_size, size_t) {
                if (this->is_in_low_guest_window(host_address) || this->is_in_guest_arena(host_address, span_size))
                {
                    replace_host_span(host_address, span_size, PROT_NONE);
                }
                else
                {
                    ::munmap(reinterpret_cast<void*>(host_address), span_size);
                }
            });
        }

        // A WoW64 process's 32-bit code needs the guest's low 4GB, which the host process may already use for
        // itself (Android's ART keeps its Java heap there) or cannot map at all (Darwin's __PAGEZERO). The guest's
        // low 4GB is then backed by a host window elsewhere, and FEX adds the window's base to every guest access
        // below 4GB. Inside a VM the low 4GB is mapped at its guest addresses, so only FEX's own reads of guest code
        // need it.
        void enable_low_guest_window()
        {
            if (!this->claimed_host_ranges_.empty() && this->claimed_host_ranges_.begin()->first < low_guest_window_size)
            {
                throw std::logic_error("FEX backend must enable the low guest window before mapping guest memory below 4GB");
            }

            void* window = ::mmap(nullptr, low_guest_window_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
            if (window == MAP_FAILED)
            {
                throw std::runtime_error("FEX backend failed to reserve the host window for the guest's low 4GB");
            }

            this->low_guest_window_ = reinterpret_cast<uint64_t>(window);
            this->context_->SetWow64GuestRebaseValue(this->low_guest_window_);
            this->context_->SetNeedsWow64GuestRebase(true);
            this->context_->SetWow64GuestRebaseInGeneratedCode(!this->hypervisor_);
        }

        // Above 4GB the guest only gets the arena.
        void add_ranges_outside_guest_arena(std::vector<host_reserved_range>& ranges, const uint64_t start, const uint64_t end) const
        {
            const auto add_unavailable = [&](const uint64_t from, const uint64_t to) {
                if (from < to)
                {
                    ranges.push_back({.address = from, .size = static_cast<size_t>(to - from)});
                }
            };

            const uint64_t above_low = std::max(start, low_guest_window_size);
            add_unavailable(above_low, std::min(end, this->guest_arena_));
            add_unavailable(std::max(above_low, this->guest_arena_ + guest_arena_size), end);
        }

        bool is_in_guest_arena(const uint64_t host_address, const size_t size) const
        {
            return this->guest_arena_ != 0 && host_address >= this->guest_arena_ &&
                   host_address + size <= this->guest_arena_ + guest_arena_size;
        }

        // Without the arena, guest memory still works but cannot be saved.
        void reserve_guest_arena()
        {
#ifdef __APPLE__
            constexpr int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
#else
            if (guest_arena_base + guest_arena_size > this->host_address_end_)
            {
                return;
            }

            constexpr int flags = MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
#endif

            void* arena = ::mmap(reinterpret_cast<void*>(guest_arena_base), guest_arena_size, PROT_NONE, flags, -1, 0);
            if (arena == MAP_FAILED)
            {
                return;
            }

            // macOS has no MAP_FIXED_NOREPLACE and Linux kernels before 4.17 ignore it, so the address is only a hint.
            if (reinterpret_cast<uint64_t>(arena) != guest_arena_base)
            {
                ::munmap(arena, guest_arena_size);
                return;
            }

            this->guest_arena_ = guest_arena_base;
        }

        void release_guest_arena()
        {
            if (this->guest_arena_ != 0)
            {
                ::munmap(reinterpret_cast<void*>(this->guest_arena_), guest_arena_size);
                this->guest_arena_ = 0;
            }
        }

        bool is_in_low_guest_window(const uint64_t host_address) const
        {
            return this->low_guest_window_ != 0 && host_address >= this->low_guest_window_ &&
                   host_address < this->low_guest_window_ + low_guest_window_size;
        }

        uint64_t to_host_address(const uint64_t address) const
        {
            return this->low_guest_window_ != 0 && address < low_guest_window_size ? this->low_guest_window_ + address : address;
        }

        uint64_t to_guest_address(const uint64_t host_address) const
        {
            return this->is_in_low_guest_window(host_address) ? host_address - this->low_guest_window_ : host_address;
        }

        // Calls f(host_address, size, offset) for each host-contiguous part of a guest range: one, or two when the
        // range crosses the end of the low window.
        template <typename F>
        void for_each_host_span(const uint64_t address, const size_t size, F&& f) const
        {
            const uint64_t end = address + size;
            uint64_t cursor = address;
            if (this->low_guest_window_ != 0 && cursor < low_guest_window_size && end > low_guest_window_size)
            {
                f(this->to_host_address(cursor), static_cast<size_t>(low_guest_window_size - cursor), size_t{0});
                cursor = low_guest_window_size;
            }

            if (cursor < end)
            {
                f(this->to_host_address(cursor), static_cast<size_t>(end - cursor), static_cast<size_t>(cursor - address));
            }
        }

        // Returns the region that contains address, not merely its predecessor in the ordered map.
        std::map<uint64_t, mapped_region>::const_iterator find_region_containing(uint64_t address) const
        {
            auto it = this->regions_.upper_bound(address);
            if (it == this->regions_.begin())
            {
                return this->regions_.end();
            }

            --it;
            return address < it->first + it->second.size ? it : this->regions_.end();
        }

        // Callers are expected to have checked is_range_mapped already.
        bool range_is_writable(uint64_t address, size_t size) const
        {
            return this->range_has_permission(address, size, memory_permission::write);
        }

        bool range_has_permission(uint64_t address, size_t size, const memory_permission permissions) const
        {
            uint64_t cursor = address;
            const uint64_t end = address + size;

            while (cursor < end)
            {
                const auto it = this->find_region_containing(cursor);
                if (it == this->regions_.end() || (it->second.permissions & permissions) != permissions)
                {
                    return false;
                }

                cursor = it->first + it->second.size;
            }

            return true;
        }

        std::optional<memory_violation_type> first_range_violation(uint64_t address, size_t size, memory_permission permission) const
        {
            if (size == 0)
            {
                return std::nullopt;
            }

#ifdef __APPLE__
            const uint64_t end = address + size;
            for (uint64_t page = address & ~(page_size - 1); page < end; page += page_size)
            {
                const auto it = this->page_shadow_apple_.find(page);
                if (it == this->page_shadow_apple_.end())
                {
                    return memory_violation_type::unmapped;
                }
                if ((it->second & permission) != permission)
                {
                    return memory_violation_type::protection;
                }
            }
#else
            uint64_t cursor = address;
            const uint64_t end = address + size;
            while (cursor < end)
            {
                const auto it = this->find_region_containing(cursor);
                if (it == this->regions_.end())
                {
                    return memory_violation_type::unmapped;
                }
                if ((it->second.permissions & permission) != permission)
                {
                    return memory_violation_type::protection;
                }
                cursor = it->first + it->second.size;
            }
#endif

            return std::nullopt;
        }

        // For loader-privileged writes: sogen itself writing guest memory it declared read-only, such as
        // a PE section's initial file content before its final permission is locked in. Needed because
        // this backend enforces the declared permission through real host protection, unlike Unicorn's
        // uc_mem_write, which operates on emulated memory independent of any host mprotect state.
        void set_temporary_access(uint64_t address, size_t size, const memory_permission added, bool enable) const
        {
#ifdef __APPLE__
            if (this->hypervisor_)
            {
                return;
            }
            const uint64_t start = host_page_align_down_apple(address);
            const uint64_t end = host_page_align_up_apple(address + size);
            for (uint64_t host_page = start; host_page < end; host_page += host_page_size_apple)
            {
                // The page stays claimed while it is accessible, so only its protection changes.
                const auto declared = this->shadow_permission_apple(host_page).value_or(memory_permission::none);
                ::mprotect(reinterpret_cast<void*>(this->to_host_address(host_page)), host_page_size_apple,
                           to_prot_apple(enable ? declared | added : declared));
            }
#else
            // The range can span several regions_ entries with different declared permissions, so
            // both the bump and the restore work per intersecting entry.
            const uint64_t end = address + size;

            auto it = this->regions_.upper_bound(address);
            if (it != this->regions_.begin())
            {
                --it;
            }

            for (; it != this->regions_.end() && it->first < end; ++it)
            {
                const uint64_t region_end = it->first + it->second.size;
                if (region_end <= address)
                {
                    continue;
                }

                const uint64_t sub_start = std::max(it->first, address & ~(page_size - 1));
                const uint64_t sub_end = std::min(region_end, (end + page_size - 1) & ~(page_size - 1));
                const memory_permission perm = enable ? (it->second.permissions | added) : it->second.permissions;
                this->for_each_host_span(sub_start, static_cast<size_t>(sub_end - sub_start),
                                         [&](const uint64_t host_address, const size_t span_size, size_t) {
                                             ::mprotect(reinterpret_cast<void*>(host_address), span_size, to_prot(perm));
                                         });
            }
#endif
        }

        // Keeps regions_ non-overlapping, the invariant is_range_mapped/range_is_writable rely on. The
        // guest memory manager commits a reserved region in gap-filling sub-ranges (several map_memory
        // calls, so several entries tile one committed region) but decommits it in one call, so an
        // unmap range spans several entries and does not start at each entry's key. A plain
        // regions_.erase(address) would orphan the rest, and is_range_mapped's --upper_bound walk would
        // later land on a stale inner entry and wrongly report "not mapped". Entries straddling an edge
        // are trimmed or split, since the host-level unmap only touches [address, address+size).
        void erase_region_range(uint64_t address, size_t size)
        {
            const uint64_t end = address + size;

            auto it = this->regions_.lower_bound(address);
            if (it != this->regions_.begin())
            {
                auto prev = std::prev(it);
                if (prev->first + prev->second.size > address)
                {
                    it = prev; // a region starting before `address` extends into the range
                }
            }

            while (it != this->regions_.end() && it->first < end)
            {
                const uint64_t region_start = it->first;
                const uint64_t region_end = region_start + it->second.size;
                if (region_end <= address)
                {
                    ++it;
                    continue;
                }

                const auto region = it->second;
                it = this->regions_.erase(it);

                if (region_start < address)
                {
                    this->regions_[region_start] = mapped_region{
                        .size = static_cast<size_t>(address - region_start), .permissions = region.permissions, .owned = region.owned};
                }
                if (region_end > end)
                {
                    it = this->regions_
                             .emplace(end, mapped_region{.size = static_cast<size_t>(region_end - end),
                                                         .permissions = region.permissions,
                                                         .owned = region.owned})
                             .first;
                    ++it;
                }
            }
        }

        // Splits entries straddling an edge so only the in-range part changes. A guest protection change
        // can target a sub-range of a larger committed region, start mid-entry, or span several entries,
        // all of which a plain regions_.find(address) either misses or over-applies. Since
        // QueryGuestExecutableRange decides executability straight from these recorded permissions, a
        // stale entry makes the JIT reject a legitimately-executable page or execute one it should not.
        // Gaps in the tiling are preserved: unmapped holes are never fabricated as mapped.
        void set_region_range_permissions(uint64_t address, size_t size, memory_permission permissions)
        {
            const uint64_t end = address + size;

            std::vector<std::pair<uint64_t, mapped_region>> touched;
            auto it = this->regions_.lower_bound(address);
            if (it != this->regions_.begin())
            {
                auto prev = std::prev(it);
                if (prev->first + prev->second.size > address)
                {
                    it = prev; // a region starting before `address` extends into the range
                }
            }
            for (; it != this->regions_.end() && it->first < end; ++it)
            {
                if (it->first + it->second.size > address)
                {
                    touched.emplace_back(it->first, it->second);
                }
            }

            for (const auto& [region_start, region] : touched)
            {
                const uint64_t region_end = region_start + region.size;
                this->regions_.erase(region_start);

                if (region_start < address)
                {
                    this->regions_[region_start] = mapped_region{
                        .size = static_cast<size_t>(address - region_start), .permissions = region.permissions, .owned = region.owned};
                }
                const uint64_t inner_start = std::max(region_start, address);
                const uint64_t inner_end = std::min(region_end, end);
                this->regions_[inner_start] =
                    mapped_region{.size = static_cast<size_t>(inner_end - inner_start), .permissions = permissions, .owned = region.owned};
                if (region_end > end)
                {
                    this->regions_[end] = mapped_region{
                        .size = static_cast<size_t>(region_end - end), .permissions = region.permissions, .owned = region.owned};
                }
            }
        }

        bool is_range_mapped(uint64_t address, size_t size) const
        {
            if (size == 0)
            {
                return true;
            }

            uint64_t cursor = address;
            const uint64_t end = address + size;

            // Walk the (sorted) region map covering [address, end). Regions are page-granular and
            // non-overlapping, so a simple forward walk suffices.
            while (cursor < end)
            {
                const auto it = this->find_region_containing(cursor);
                if (it == this->regions_.end())
                {
                    return false;
                }
                cursor = it->first + it->second.size;
            }

            return true;
        }

#ifdef __APPLE__
        // 16KB-host vs 4KB-guest permission reconciliation. nullopt permissions means unmap: the pages
        // become "never requested" again, which must still fault like reserved-but-uncommitted guest
        // memory rather than being silently allowed.
        void set_shadow_range_apple(uint64_t address, size_t size, std::optional<memory_permission> permissions)
        {
            for (uint64_t page = address; page < address + size; page += page_size)
            {
                if (permissions.has_value())
                {
                    this->page_shadow_apple_[page] = *permissions;
                }
                else
                {
                    this->page_shadow_apple_.erase(page);
                }
            }
        }

        // Derives one 16KB host page's permission from its up to four 4KB shadow slots, taking their
        // union when they disagree. The union also swallows the "some slot absent" case: a
        // guard/reserved slot sharing a host page with mapped memory is folded in rather than made to
        // fault, a deliberate relaxation until a Mach exception handler can resolve faults on a
        // PROT_NONE page without breaking a legitimate access from a stricter neighbor.
        void sync_host_page_apple(uint64_t host_page_addr)
        {
            const auto effective = this->shadow_permission_apple(host_page_addr);
            const bool currently_mapped = this->host_range_is_claimed(host_page_addr, host_page_size_apple);

#if SOGEN_FEX_HYPERVISOR
            if (this->hypervisor_)
            {
                this->sync_host_page_in_vm(host_page_addr, effective, currently_mapped);
                return;
            }
#endif

            if (!effective)
            {
                if (currently_mapped)
                {
                    // The guest range may merely be decommitted while still MEM_RESERVE'd, so the page
                    // must stay claimed: munmapping it would let a foreign host allocation land here
                    // and be clobbered by a later recommit's MAP_FIXED. Replacing the mapping rather
                    // than mprotect'ing it discards the old contents, so a recommit sees the zeroed
                    // pages MEM_COMMIT requires. release_guest_address_range drops the claim for good.
                    this->remap_host_claim(host_page_addr, host_page_size_apple, PROT_NONE);
                }
                return;
            }

            if (!currently_mapped)
            {
                this->claim_host_range(host_page_addr, host_page_size_apple);
            }

            if (::mprotect(reinterpret_cast<void*>(this->to_host_address(host_page_addr)), host_page_size_apple,
                           to_prot_apple(*effective)) != 0)
            {
                throw std::runtime_error("FEX backend failed to change memory protection");
            }
        }

        std::optional<memory_permission> shadow_permission_apple(const uint64_t host_page_addr) const
        {
            std::optional<memory_permission> effective{};
            for (uint64_t page = host_page_addr; page < host_page_addr + host_page_size_apple; page += page_size)
            {
                const auto it = this->page_shadow_apple_.find(page);
                if (it != this->page_shadow_apple_.end())
                {
                    effective = effective.value_or(memory_permission::none) | it->second;
                }
            }

            return effective;
        }

        void sync_host_pages_covering_apple(uint64_t address, size_t size)
        {
            const uint64_t start = host_page_align_down_apple(address);
            const uint64_t end = host_page_align_up_apple(address + size);
            for (uint64_t host_page = start; host_page < end; host_page += host_page_size_apple)
            {
                this->sync_host_page_apple(host_page);
            }
        }

        bool host_page_maps_pages_outside_apple(const uint64_t host_page_addr, const uint64_t address, const uint64_t end) const
        {
            for (uint64_t page = host_page_addr; page < host_page_addr + host_page_size_apple; page += page_size)
            {
                if ((page < address || page >= end) && this->page_shadow_apple_.contains(page))
                {
                    return true;
                }
            }

            return false;
        }

        // A host page stays mapped while any of its guest pages is, so a guest page unmapped from it keeps
        // its old contents. Mapping that guest page again must still hand out the zero-filled page that
        // MEM_COMMIT guarantees.
        void zero_range_in_shared_host_pages_apple(const uint64_t address, const size_t size)
        {
            const uint64_t end = address + size;
            for (uint64_t host_page = host_page_align_down_apple(address); host_page < end; host_page += host_page_size_apple)
            {
                if (!this->host_page_maps_pages_outside_apple(host_page, address, end))
                {
                    continue;
                }

                const uint64_t zero_start = std::max(address, host_page);
                const size_t zero_size = static_cast<size_t>(std::min(end, host_page + host_page_size_apple) - zero_start);
                const auto host_permission = this->shadow_permission_apple(host_page).value_or(memory_permission::none);
                const bool needs_temporary_write = (host_permission & memory_permission::write) == memory_permission::none;

                if (needs_temporary_write)
                {
                    this->set_temporary_access(zero_start, zero_size, memory_permission::write, true);
                }

                std::memset(reinterpret_cast<void*>(this->to_host_address(zero_start)), 0, zero_size);

                if (needs_temporary_write)
                {
                    this->set_temporary_access(zero_start, zero_size, memory_permission::write, false);
                }
            }
        }
#endif

        // FEX context plumbing

        void initialize_context()
        {
            // Without an installed handler, LogMan silently discards the formatted message (LogManager.
            // cpp's `if (Handler)`) while still executing FEX_TRAP_EXECUTION for ASSERT-level messages,
            // so every internal FEXCore assertion failure crashes with no indication of what failed.
            LogMan::Msg::InstallHandler([](LogMan::DebugLevels level, const char* message) {
                fprintf(stderr, "[FEXCore LogMan] level=%s: %s\n", LogMan::DebugLevelStr(level), message);
            });
            LogMan::Throw::InstallHandler([](const char* message) { fprintf(stderr, "[FEXCore LogMan THROW] %s\n", message); });

#ifdef __APPLE__
            // Must happen before the first FEXCore-internal allocation (CreateNewContext allocates the
            // CodeBuffer and dispatcher) and before reserved_host_ranges() is first queried, so the
            // whole arena is off-limits to guest allocations.
            fex_internal_arena::instance().install(this->hypervisor_);
#endif

            // libc++abi's default terminate handler prints nothing useful for an uncaught exception.
            std::set_terminate([]() {
                fprintf(stderr, "[FEX backend] std::terminate invoked\n");
                if (auto exc = std::current_exception())
                {
                    try
                    {
                        std::rethrow_exception(exc);
                    }
                    catch (const std::exception& e)
                    {
                        fprintf(stderr, "[FEX backend] uncaught exception: %s\n", e.what());
                    }
                    catch (...)
                    {
                        fprintf(stderr, "[FEX backend] uncaught exception of unknown type\n");
                    }
                }

#if !defined(__ANDROID__) || __ANDROID_API__ >= 33
                std::array<void*, 64> frames{};
                const int frame_count = ::backtrace(frames.data(), static_cast<int>(frames.size()));
                char** symbols = ::backtrace_symbols(frames.data(), frame_count);
                fprintf(stderr, "[FEX backend] backtrace (%d frames):\n", frame_count);
                for (int i = 0; i < frame_count; ++i)
                {
                    fprintf(stderr, "  %s\n", symbols ? symbols[i] : "?");
                }
                free(symbols);
#endif

                std::abort();
            });

            FEXCore::Config::Initialize();
            FEXCore::Config::Load();

            // With EMULATOR_FEX_DUMPIR naming an existing directory, FEXCore writes one file per
            // translated guest basic block, keyed by guest RIP: "<rip:x>-pre.ir" straight out of the
            // decoder and "<rip:x>-post.ir" after optimization and register allocation. Unlike inline
            // hot-path diagnostics, this is confirmed not to perturb timing-sensitive JIT bugs.
            // PassManagerDumpIR value 3 == BEFOREOPT(1)|AFTEROPT(2).
            if (const char* dumpir_dir = std::getenv("EMULATOR_FEX_DUMPIR"))
            {
                FEXCore::Config::Set(FEXCore::Config::CONFIG_DUMPIR, dumpir_dir);
                FEXCore::Config::Set(FEXCore::Config::CONFIG_PASSMANAGERDUMPIR, "3");
            }

            FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, "1");
            // FEX defaults to exact 80-bit x87 math, which it emulates in software. 32-bit games that do their
            // floating point on x87 then spend a large part of their time in softfloat, so host doubles are used instead.
            FEXCore::Config::Set(FEXCore::Config::CONFIG_X87REDUCEDPRECISION, "1");

            // Piggybacks on FEXCore's GdbServer flag, whose only effect inside FEXCore (ContextImpl::
            // InitCore) is setting Config.NeedsPendingInterruptFaultCheck, making the JIT emit a
            // `str zr, [InterruptFaultPage]` at every block entry - the mechanism request_thread_stop()
            // needs to force a stuck-in-JIT thread to fault. FEXCore's own gdbserver is unused (sogen
            // has its own stub), so there is no other observable effect.
            FEXCore::Config::Set(FEXCore::Config::CONFIG_GDBSERVER, "1");

#ifdef __APPLE__
            const FEXCore::HostFeatures features = fetch_host_features_apple();
#else
            const FEXCore::HostFeatures features = fetch_host_features_linux();
#endif
            this->context_ = FEXCore::Context::Context::CreateNewContext(features);

            this->syscall_handler_ = std::make_unique<fex_syscall_handler>(*this);
            this->context_->SetSyscallHandler(this->syscall_handler_.get());

            // InitCore() requires a non-null SignalDelegator, and the dispatcher entry-point addresses
            // handle_fault_signal needs come from non-virtual base-class methods, so the plain base
            // suffices - fault delivery is handled by the host signal handler installed below.
            this->signal_delegator_ = std::make_unique<FEXCore::SignalDelegator>();
            this->context_->SetSignalDelegator(this->signal_delegator_.get());

            this->context_->InitCore();
            this->context_->SetTrapTimestampAndCpuid(this->traps_timestamp_and_cpuid_);
            for (const auto& [start, size] : this->instruction_callback_ranges_)
            {
                this->context_->AddInstructionCallbackRange(start, size);
            }

#if SOGEN_FEX_HYPERVISOR
            if (this->hypervisor_)
            {
                this->create_vm();
                this->enable_low_guest_window();
                return;
            }
#endif
            install_fault_signal_handlers(*this);
        }

        // memory_violation_hooks_/interrupt_hooks_ callbacks are backend-agnostic windows-emulator code
        // that allocates, logs and mutates STL containers freely. That is fine from normal call context
        // (where KVM/Unicorn invoke them) but not from inside handle_fault_signal, which can interrupt
        // an unrelated malloc()/free() on this thread - a confirmed, ASLR-timing-dependent heap
        // corruption hazard. So the handler stashes plain data here instead and forces ExecuteThread to
        // unwind back to run() via ThreadStopHandlerAddress, without touching stop_requested_; run()
        // dispatches the hook in normal context and resumes by re-entering ExecuteThread, which always
        // restarts from CurrentFrame->State.rip.
        enum class pending_fault_kind
        {
            none,
            memory_violation,
            interrupt,
        };

        struct pending_fault_dispatch
        {
            pending_fault_kind kind = pending_fault_kind::none;
            uint64_t address = 0;
            size_t size = 0;
            memory_operation operation{};
            memory_violation_type type{};
            int vector = 0;
        };

        // Called only from run(), in normal call context, right after ExecuteThread returns. The
        // return value tells run()'s loop whether ExecuteThread came back because a hook was deferred
        // or because of a genuine stop.
        bool dispatch_pending_hook_if_any(fex_vcpu& vcpu)
        {
            const pending_fault_dispatch dispatch = this->pending_fault_dispatch_;
            this->pending_fault_dispatch_.kind = pending_fault_kind::none;

            switch (dispatch.kind)
            {
            case pending_fault_kind::memory_violation:
                for (auto& [_, hook] : this->memory_violation_hooks_)
                {
                    hook(vcpu, dispatch.address, dispatch.size, dispatch.operation, dispatch.type);
                }
                return true;
            case pending_fault_kind::interrupt:
                if (dispatch.vector == invalid_opcode_vector && (this->run_trapped_instruction() || this->run_invalid_instruction_hooks()))
                {
                    return true;
                }

                for (auto& [_, hook] : this->interrupt_hooks_)
                {
                    hook(vcpu, dispatch.vector);
                }
                return true;
            case pending_fault_kind::none:
            default:
                return false;
            }
        }

        // FEX then calls back before every instruction in the range (on_instruction). The code already compiled there
        // has no callbacks, so it is dropped.
        void add_instruction_callback_range(const uint64_t start, const uint64_t size)
        {
            this->instruction_callback_ranges_.emplace_back(start, size);
            if (this->context_)
            {
                this->context_->AddInstructionCallbackRange(start, size);
                this->invalidate_instruction_callback_range(start, size);
            }
        }

        void remove_instruction_callback_range(const uint64_t start, const uint64_t size)
        {
            const auto range = std::ranges::find(this->instruction_callback_ranges_, std::pair{start, size});
            if (range != this->instruction_callback_ranges_.end())
            {
                this->instruction_callback_ranges_.erase(range);
            }

            if (this->context_)
            {
                this->context_->RemoveInstructionCallbackRange(start, size);
                this->invalidate_instruction_callback_range(start, size);
            }
        }

        void invalidate_instruction_callback_range(const uint64_t start, const uint64_t size)
        {
            if (!this->vcpu_.thread_)
            {
                return;
            }

            if (size > 0x100000000ULL)
            {
                this->context_->ClearCodeCache(this->vcpu_.thread_);
            }
            else
            {
                this->invalidate_code_range(start, static_cast<size_t>(size));
            }
        }

        void delete_execution_hook(emulator_hook* hook)
        {
            if (this->running_execution_hooks_)
            {
                this->deleted_execution_hooks_.push_back(hook);
                return;
            }

            const auto entry = this->memory_execution_hooks_.find(hook);
            if (entry == this->memory_execution_hooks_.end())
            {
                return;
            }

            const auto [start, size, _] = entry->second;
            if (size == 1)
            {
                auto& hooks = this->execution_hooks_by_address_[start];
                std::erase(hooks, hook);
                if (hooks.empty())
                {
                    this->execution_hooks_by_address_.erase(start);
                }
            }
            else
            {
                std::erase(this->range_execution_hooks_, hook);
            }

            this->memory_execution_hooks_.erase(entry);
            this->remove_instruction_callback_range(start, size);
        }

        void run_execution_hooks(const uint64_t address)
        {
            auto hooks = this->range_execution_hooks_;
            if (const auto by_address = this->execution_hooks_by_address_.find(address);
                by_address != this->execution_hooks_by_address_.end())
            {
                hooks.insert(hooks.end(), by_address->second.begin(), by_address->second.end());
            }

            this->running_execution_hooks_ = true;
            for (auto* hook : hooks)
            {
                const auto entry = this->memory_execution_hooks_.find(hook);
                if (entry != this->memory_execution_hooks_.end() && address >= entry->second.start &&
                    address - entry->second.start < entry->second.size)
                {
                    entry->second.callback(this->vcpu_, address);
                }
            }
            this->running_execution_hooks_ = false;

            for (auto* hook : std::exchange(this->deleted_execution_hooks_, {}))
            {
                this->delete_execution_hook(hook);
            }
        }

        // Runs before the instruction at address. A nonzero result leaves the translated block and continues at the
        // current rip: to stop when a stop is pending or the budget of start(count) is used up, or to follow a hook that
        // moved rip.
        uint64_t on_instruction(const uint64_t address)
        {
            if (!this->vcpu_.stop_requested_ && this->instruction_budget_)
            {
                if (*this->instruction_budget_ == 0)
                {
                    this->vcpu_.stop_requested_ = true;
                }
                else
                {
                    --*this->instruction_budget_;
                }
            }

            if (this->vcpu_.stop_requested_)
            {
                this->vcpu_.request_thread_stop();
                return 1;
            }

            this->run_execution_hooks(address);
            return this->vcpu_.stop_requested_ || this->vcpu_.read_instruction_pointer() != address ? 1 : 0;
        }

        // FEX compiles these to a #UD trap once one of them is hooked, so the hooks see every execution.
        void trap_timestamp_and_cpuid()
        {
            if (this->traps_timestamp_and_cpuid_)
            {
                return;
            }

            this->traps_timestamp_and_cpuid_ = true;
            if (this->context_)
            {
                this->context_->SetTrapTimestampAndCpuid(true);
                if (this->vcpu_.thread_)
                {
                    this->context_->ClearCodeCache(this->vcpu_.thread_);
                }
            }
        }

        // A trapped RDTSC, RDTSCP or CPUID: its hooks run, and unless one skips it, its native result is produced
        // here, since the compiled code no longer computes it.
        bool run_trapped_instruction()
        {
            const auto rip = this->reg(x86_register::rip);
            std::array<uint8_t, 3> code{};
            if (!this->try_read_memory(rip, code.data(), code.size()) || code[0] != 0x0F)
            {
                return false;
            }

            x86_hookable_instructions type{};
            uint64_t length = 2;
            if (code[1] == 0x31)
            {
                type = x86_hookable_instructions::rdtsc;
            }
            else if (code[1] == 0xA2)
            {
                type = x86_hookable_instructions::cpuid;
            }
            else if (code[1] == 0x01 && code[2] == 0xF9)
            {
                type = x86_hookable_instructions::rdtscp;
                length = 3;
            }
            else
            {
                return false;
            }

            bool skip = false;
            for (auto& [_, hook] : this->instruction_hooks_)
            {
                if (hook.type == type && hook.callback && hook.callback(*this, 0) == instruction_hook_continuation::skip_instruction)
                {
                    skip = true;
                }
            }

            if (!skip)
            {
                this->run_native_instruction(type);
            }

            this->reg(x86_register::rip, this->reg(x86_register::rip) + length);
            return true;
        }

        bool run_invalid_instruction_hooks()
        {
            const auto rip = this->vcpu_.read_instruction_pointer();
            bool consumed = false;
            for (auto& [_, hook] : this->instruction_hooks_)
            {
                if (hook.type == x86_hookable_instructions::invalid && hook.callback &&
                    hook.callback(*this, 0) == instruction_hook_continuation::skip_instruction)
                {
                    consumed = true;
                }
            }

            if (consumed && this->vcpu_.read_instruction_pointer() == rip)
            {
                this->reg(x86_register::rip, rip + 2);
            }

            return consumed;
        }

        void run_native_instruction(const x86_hookable_instructions type)
        {
            if (type == x86_hookable_instructions::cpuid)
            {
                const auto result =
                    this->context_->RunCPUIDFunction(this->reg<uint32_t>(x86_register::eax), this->reg<uint32_t>(x86_register::ecx));
                this->reg(x86_register::rax, uint64_t{result.eax});
                this->reg(x86_register::rbx, uint64_t{result.ebx});
                this->reg(x86_register::rcx, uint64_t{result.ecx});
                this->reg(x86_register::rdx, uint64_t{result.edx});
                return;
            }

            const auto ticks = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
            this->reg(x86_register::rax, ticks & 0xFFFFFFFF);
            this->reg(x86_register::rdx, ticks >> 32);
            if (type == x86_hookable_instructions::rdtscp)
            {
                this->reg(x86_register::rcx, uint64_t{0});
            }
        }

        // Called only from signal-handler context; see pending_fault_kind for why hooks cannot run
        // directly there. state_already_spilled selects the dispatcher unwind entry that skips the SRA
        // spill only for FEXCore's synthetic-exception path, where CpuStateFrame is already current.

        // Called only from signal-handler context; see pending_fault_kind for why hooks cannot run
        // directly there. state_already_spilled selects the dispatcher unwind entry that skips the SRA
        // spill only for FEXCore's synthetic-exception path, where CpuStateFrame is already current.
#if SOGEN_FEX_HYPERVISOR
      public:
        // Pages FEX is about to replace or release leave the VM; they enter it again on their next use.
        void drop_from_vm(const uint64_t address, const size_t size)
        {
            if (!this->vm_ || size == 0)
            {
                return;
            }
            const auto start = address & ~(page_size - 1);
            const auto end = (address + size + page_size - 1) & ~(page_size - 1);
            hvf::share_host_memory(*this->vm_, start, static_cast<size_t>(end - start), std::nullopt);
        }

      private:
        // Hypervisor mode: FEX's dispatcher and translated code run at EL0 in a Hypervisor.framework VM. The guest's
        // memory and FEX's own appear there at their host addresses. Calls from translated code into host C++ land on
        // unmapped addresses and are made on the host, and the exits that would have been host signals in-process go
        // to handle_fault_signal through a ucontext built from the vCPU.

        static constexpr uint64_t host_call_base = 0x7F0000000000ull;
        static constexpr uint64_t host_call_stride = 0x10;
        static constexpr size_t vm_stack_size = 0x100000;
        static constexpr int undefined_instruction_class = 0x00;
        static constexpr int instruction_abort_class = 0x20;
        static constexpr int data_abort_class = 0x24;
        static constexpr int breakpoint_class = 0x3C;

        struct image_range
        {
            uint64_t start{};
            uint64_t end{};
        };

        static image_range find_image_of(const void* symbol)
        {
            Dl_info info{};
            if (::dladdr(symbol, &info) == 0 || info.dli_fbase == nullptr)
            {
                throw std::runtime_error("Failed to locate FEXCore's image");
            }

            const auto* header = static_cast<const mach_header_64*>(info.dli_fbase);
            const auto* command = reinterpret_cast<const std::byte*>(header + 1);
            uint64_t slide{};
            image_range range{.start = ~uint64_t{}, .end = 0};
            for (uint32_t index = 0; index < header->ncmds; ++index)
            {
                const auto* load = reinterpret_cast<const load_command*>(command);
                if (load->cmd == LC_SEGMENT_64)
                {
                    const auto* segment = reinterpret_cast<const segment_command_64*>(load);
                    const std::string_view name{segment->segname, strnlen(segment->segname, sizeof(segment->segname))};
                    if (name == "__TEXT")
                    {
                        slide = reinterpret_cast<uint64_t>(header) - segment->vmaddr;
                    }
                    if (name != "__PAGEZERO" && name != "__LINKEDIT")
                    {
                        range.start = std::min(range.start, segment->vmaddr);
                        range.end = std::max(range.end, segment->vmaddr + segment->vmsize);
                    }
                }
                command += load->cmdsize;
            }
            return {.start = range.start + slide, .end = range.end + slide};
        }

        void create_vm()
        {
            this->vm_ = hvf::create_arm64_emulator(1);
            hvf::set_el0_exception_handler(*this->vm_, [this](arm64_cpu& cpu, const hvf::el0_exception& exception) {
                return this->handle_vm_exception(cpu, exception);
            });
            // FEX reads the guest's TSC from CNTVCT_EL0, which the VM traps.
            this->vm_->hook_instruction(arm64_hookable_instructions::read_counter, [](cpu_interface& cpu, const uint64_t destination) {
                const uint64_t counter = __builtin_arm_rsr64("cntvct_el0");
                dynamic_cast<arm64_cpu&>(cpu).reg(static_cast<arm64_register>(destination), counter);
                return instruction_hook_continuation::skip_instruction;
            });

            auto* stack = ::mmap(nullptr, vm_stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (stack == MAP_FAILED)
            {
                throw std::runtime_error("Failed to allocate the VM stack for FEX");
            }
            this->vm_stack_ = static_cast<std::byte*>(stack);
            hvf::share_host_memory(*this->vm_, reinterpret_cast<uint64_t>(stack), vm_stack_size, memory_permission::read_write);

            this->fex_image_ = find_image_of(reinterpret_cast<const void*>(&FEXCore::Allocator::mmap));
            g_hypervisor_emulator = this;
            fex_internal_arena::instance().set_release_hook(
                [this](const uint64_t address, const size_t size) { this->drop_from_vm(address, size); });
            FEXCore::Allocator::dontneed = &drop_from_hypervisor;
        }

        bool is_fex_memory(const uint64_t address) const
        {
            const auto& arena = fex_internal_arena::instance();
            if (address >= arena.base() && address < arena.base() + fex_internal_arena::arena_size)
            {
                return true;
            }
            const auto thread_state = reinterpret_cast<uint64_t>(this->vcpu_.thread_);
            if (this->vcpu_.thread_ != nullptr && address >= thread_state && address < thread_state + sizeof(*this->vcpu_.thread_))
            {
                return true;
            }
            return address >= this->fex_image_.start && address < this->fex_image_.end;
        }

        // FEX's own memory enters the VM the first time translated code touches it. Data is shared. Constants in
        // FEXCore's image share 16 KiB pages with executable code, which the VM must not map, so they are copied.
        bool map_fex_memory(const uint64_t address)
        {
            if (!this->is_fex_memory(address) || this->vcpu_.in_interrupt_page(address))
            {
                return false;
            }
            const auto block = host_page_align_down_apple(address);
            if (hvf::can_share_host_memory(block))
            {
                hvf::share_host_memory(*this->vm_, block, host_page_size_apple, memory_permission::all);
                return true;
            }
            const auto page = address & ~(page_size - 1);
            if (hvf::is_mapped(*this->vm_, page))
            {
                return false;
            }
            hvf::copy_host_memory(*this->vm_, page, page_size, memory_permission::read);
            return true;
        }

        bool handle_vm_exception(arm64_cpu& cpu, const hvf::el0_exception& exception)
        {
            const auto address = exception.fault_address;
            const auto fault_status = exception.syndrome & 0x3F;
            const bool translation_fault = (fault_status & 0x3C) == 0x04;
            const bool abort = exception.exception_class == instruction_abort_class || exception.exception_class == data_abort_class;

            if (exception.exception_class == instruction_abort_class && translation_fault && address >= host_call_base &&
                address < host_call_base + host_call_stride * this->host_calls_.size())
            {
                this->perform_host_call(cpu, static_cast<size_t>((address - host_call_base) / host_call_stride));
                return true;
            }

            int signal{};
            int code{};
            if (abort && translation_fault)
            {
                if (this->map_fex_memory(address))
                {
                    return true;
                }
                signal = SIGSEGV;
                code = SEGV_MAPERR;
            }
            else if (abort && (fault_status & 0x3C) == 0x0C)
            {
                signal = SIGSEGV;
                code = SEGV_ACCERR;
            }
            else if (abort && fault_status == 0x21)
            {
                signal = SIGBUS;
                code = BUS_ADRALN;
            }
            else if (exception.exception_class == undefined_instruction_class)
            {
                signal = SIGILL;
                code = ILL_ILLOPC;
            }
            else if (exception.exception_class == breakpoint_class)
            {
                signal = SIGTRAP;
                code = TRAP_BRKPT;
            }
            else
            {
                return false;
            }

            return this->deliver_host_signal(cpu, signal, code, address);
        }

        void perform_host_call(arm64_cpu& cpu, const size_t index)
        {
            if (index == 0)
            {
                this->vcpu_.returned_to_host_ = true;
                cpu.stop();
                return;
            }

            sogen_fex_host_call_frame call{};
            for (int reg = 0; reg < 8; ++reg)
            {
                call.x[reg] = cpu.reg(static_cast<arm64_register>(static_cast<int>(arm64_register::x0) + reg));
                cpu.read_register(static_cast<arm64_register>(static_cast<int>(arm64_register::q0) + reg), &call.v[reg],
                                  sizeof(call.v[reg]));
            }
            call.function = this->host_calls_[index];
            sogen_fex_host_call(&call);

            cpu.reg(arm64_register::x0, call.x[0]);
            cpu.reg(arm64_register::x1, call.x[1]);
            for (int reg = 0; reg < 4; ++reg)
            {
                cpu.write_register(static_cast<arm64_register>(static_cast<int>(arm64_register::q0) + reg), &call.v[reg],
                                   sizeof(call.v[reg]));
            }
            cpu.reg(arm64_register::pc, cpu.reg(arm64_register::x30));
        }

        bool deliver_host_signal(arm64_cpu& cpu, const int signal, const int code, const uint64_t address)
        {
            __darwin_mcontext64 machine{};
            ucontext_t context{};
            context.uc_mcontext = &machine;
            for (int reg = 0; reg <= 28; ++reg)
            {
                machine.__ss.__x[reg] = cpu.reg(static_cast<arm64_register>(static_cast<int>(arm64_register::x0) + reg));
            }
            machine.__ss.__fp = cpu.reg(arm64_register::x29);
            machine.__ss.__lr = cpu.reg(arm64_register::x30);
            machine.__ss.__sp = cpu.reg(arm64_register::sp);
            arm_thread_state64_set_pc_fptr(machine.__ss, reinterpret_cast<void*>(cpu.reg(arm64_register::pc)));
            machine.__ss.__cpsr = cpu.reg<uint32_t>(arm64_register::pstate);
            for (int reg = 0; reg < 32; ++reg)
            {
                cpu.read_register(static_cast<arm64_register>(static_cast<int>(arm64_register::q0) + reg), &machine.__ns.__v[reg],
                                  sizeof(machine.__ns.__v[reg]));
            }
            machine.__ns.__fpsr = cpu.reg<uint32_t>(arm64_register::fpsr);
            machine.__ns.__fpcr = cpu.reg<uint32_t>(arm64_register::fpcr);
            machine.__es.__far = address;

            siginfo_t info{};
            info.si_signo = signal;
            info.si_code = code;
            info.si_addr = reinterpret_cast<void*>(address);

            if (!this->handle_fault_signal(signal, &info, &context))
            {
                std::array<char, 160> message{};
                snprintf(message.data(), message.size(), "Unhandled fault of FEX's code in the VM: signal %d at PC 0x%llx, address 0x%llx",
                         signal, static_cast<unsigned long long>(cpu.read_instruction_pointer()), static_cast<unsigned long long>(address));
                this->vm_failure_ = message.data();
                cpu.stop();
                return true;
            }

            for (int reg = 0; reg <= 28; ++reg)
            {
                cpu.reg(static_cast<arm64_register>(static_cast<int>(arm64_register::x0) + reg), machine.__ss.__x[reg]);
            }
            cpu.reg(arm64_register::x29, machine.__ss.__fp);
            cpu.reg(arm64_register::x30, machine.__ss.__lr);
            cpu.reg(arm64_register::sp, machine.__ss.__sp);
            cpu.reg(arm64_register::pc, arm_thread_state64_get_pc(machine.__ss));
            cpu.reg<uint32_t>(arm64_register::pstate, machine.__ss.__cpsr);
            for (int reg = 0; reg < 32; ++reg)
            {
                cpu.write_register(static_cast<arm64_register>(static_cast<int>(arm64_register::q0) + reg), &machine.__ns.__v[reg],
                                   sizeof(machine.__ns.__v[reg]));
            }
            cpu.reg<uint32_t>(arm64_register::fpsr, machine.__ns.__fpsr);
            cpu.reg<uint32_t>(arm64_register::fpcr, machine.__ns.__fpcr);
            return true;
        }

        // Host pages stay plain read-write: the guest's permissions only exist in the VM, and a host page shared with
        // the VM must never become executable.
        void sync_host_page_in_vm(const uint64_t host_page_addr, const std::optional<memory_permission>& effective,
                                  const bool currently_mapped)
        {
            if (!effective)
            {
                hvf::share_host_memory(*this->vm_, host_page_addr, host_page_size_apple, std::nullopt);
                if (currently_mapped)
                {
                    this->remap_host_claim(host_page_addr, host_page_size_apple, PROT_NONE);
                }
                return;
            }

            if (!currently_mapped)
            {
                this->claim_host_range(host_page_addr, host_page_size_apple);
            }
            const auto host_page = this->to_host_address(host_page_addr);
            if (::mprotect(reinterpret_cast<void*>(host_page), host_page_size_apple, PROT_READ | PROT_WRITE) != 0)
            {
                throw std::runtime_error("FEX backend failed to change memory protection");
            }
            for (uint64_t page = host_page_addr; page < host_page_addr + host_page_size_apple; page += page_size)
            {
                const auto shadow = this->page_shadow_apple_.find(page);
                hvf::share_host_memory(*this->vm_, page, page_size, reinterpret_cast<void*>(host_page + (page - host_page_addr)),
                                       shadow == this->page_shadow_apple_.end() ? std::nullopt : std::optional{shadow->second});
            }
        }
#endif

        void defer_hook_dispatch(ucontext_t* context, const pending_fault_dispatch& dispatch, bool state_already_spilled)
        {
            this->pending_fault_dispatch_ = dispatch;
            const auto& config = this->signal_delegator_->GetConfig();
            const auto target = state_already_spilled ? config.ThreadStopHandlerAddress : config.ThreadStopHandlerAddressSpillSRA;
            this->unwind_to_dispatcher_frame(context);
            set_host_pc(context, target);
        }

        // The stop handlers pop the dispatcher's entry frame, but JIT code can be interrupted with SP elsewhere (e.g.
        // mid-call into a helper), so SP is reset to that frame first, as FEX's own signal delegator does.
        void unwind_to_dispatcher_frame(ucontext_t* context) const
        {
            auto* frame = this->vcpu_.thread_->CurrentFrame;
            frame->SignalHandlerRefCounter = 0;
#ifdef __APPLE__
            context->uc_mcontext->__ss.__sp = frame->ReturningStackLocation;
#else
            context->uc_mcontext.sp = frame->ReturningStackLocation;
#endif
        }

        // The dispatcher is separate from FEXCore's CodeBuffer range, so IsAddressInCodeBuffer does
        // not recognize faults taken there even though the same signal paths need to identify it.
        bool host_pc_in_dispatcher(uint64_t pc) const
        {
            if (this->signal_delegator_ == nullptr)
            {
                return false;
            }
            const auto& config = this->signal_delegator_->GetConfig();
            return pc >= config.DispatcherBegin && pc < config.DispatcherEnd;
        }

        static uint64_t get_host_pc(ucontext_t* context)
        {
#ifdef __APPLE__
            return arm_thread_state64_get_pc(context->uc_mcontext->__ss);
#else
            return context->uc_mcontext.pc;
#endif
        }

        static void set_host_pc(ucontext_t* context, uint64_t pc)
        {
#ifdef __APPLE__
            arm_thread_state64_set_pc_fptr(context->uc_mcontext->__ss, reinterpret_cast<void*>(pc));
#else
            context->uc_mcontext.pc = pc;
#endif
        }

        static uint64_t get_host_gpr(ucontext_t* context, uint32_t index)
        {
            if (index == 31)
            {
                return 0;
            }

#ifdef __APPLE__
            if (index <= 28)
            {
                return context->uc_mcontext->__ss.__x[index];
            }
            return index == 29 ? context->uc_mcontext->__ss.__fp : context->uc_mcontext->__ss.__lr;
#else
            return context->uc_mcontext.regs[index];
#endif
        }

        static void set_host_gpr(ucontext_t* context, uint32_t index, uint64_t value)
        {
            if (index == 31)
            {
                return;
            }

#ifdef __APPLE__
            if (index <= 28)
            {
                context->uc_mcontext->__ss.__x[index] = value;
            }
            else if (index == 29)
            {
                context->uc_mcontext->__ss.__fp = value;
            }
            else
            {
                context->uc_mcontext->__ss.__lr = value;
            }
#else
            context->uc_mcontext.regs[index] = value;
#endif
        }

        static __uint128_t* get_host_vector_registers(ucontext_t* context)
        {
#ifdef __APPLE__
            return reinterpret_cast<__uint128_t*>(&context->uc_mcontext->__ns.__v[0]);
#else
            auto* fpsimd = find_aarch64_context<fpsimd_context>(context, FPSIMD_MAGIC);
            return fpsimd != nullptr ? fpsimd->vregs : nullptr;
#endif
        }

        // FEXCore's call-ret shadow stack (x25, see reset_callret_stack) is bracketed by a guard page
        // on each side. Upstream FEX treats hitting either guard as recoverable and resets x25 to the
        // default location (Base + CALLRET_STACK_SIZE/4), which this mirrors.
        bool handle_callret_stack_fault(ucontext_t* context, uint64_t fault_address) const
        {
            if (this->vcpu_.thread_ == nullptr || this->vcpu_.thread_->CallRetStackBase == nullptr)
            {
                return false;
            }

            const auto base = reinterpret_cast<uint64_t>(this->vcpu_.thread_->CallRetStackBase);
            const auto host_page = static_cast<uint64_t>(::getpagesize());
            constexpr uint64_t callret_stack_size = FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE;
            if (fault_address < base - host_page || fault_address >= base + callret_stack_size + host_page)
            {
                return false;
            }

            set_host_gpr(context, 25, base + callret_stack_size / 4);
            return true;
        }

      public:
        // Applies a decode_arm64_load result once its data has been fetched, writing the possibly
        // extended value into the destination register and advancing PC past the decoded instruction.
        static bool complete_decoded_load(ucontext_t* uctx, const decoded_arm64_load& decoded, const void* data, uint64_t pc)
        {
            if (decoded.is_vector)
            {
                auto* fprs = get_host_vector_registers(uctx);
                if (fprs == nullptr)
                {
                    return false;
                }

                __uint128_t value{};
                std::memcpy(&value, data, sizeof(value));
                fprs[decoded.rt] = value;
                set_host_pc(uctx, pc + 4);
                return true;
            }

            uint64_t raw_value = 0;
            std::memcpy(&raw_value, data, decoded.size);

            uint64_t result = 0;
            switch (decoded.size)
            {
            case 1:
                result = decoded.sign_extend ? static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(raw_value)))
                                             : (raw_value & 0xFFULL);
                break;
            case 2:
                result = decoded.sign_extend ? static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(raw_value)))
                                             : (raw_value & 0xFFFFULL);
                break;
            case 4:
                result = decoded.sign_extend ? static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(raw_value)))
                                             : (raw_value & 0xFFFFFFFFULL);
                break;
            default:
                result = raw_value;
                break;
            }

            if (!decoded.dest_is_64bit)
            {
                // Writing Wt always zeroes bits 63:32 of the aliased Xt (AArch64 register semantics).
                result &= 0xFFFFFFFFULL;
            }

            set_host_gpr(uctx, decoded.rt, result);
            set_host_pc(uctx, pc + 4);
            return true;
        }

        static bool handle_mmio_fault(ucontext_t* uctx, const mmio_region& region, uint64_t fault_addr)
        {
            const uint64_t pc = get_host_pc(uctx);
            const auto insn = *reinterpret_cast<const uint32_t*>(pc);
            const auto decoded = decode_arm64_load(insn);
            if (!decoded)
            {
#ifdef __APPLE__
                // stdio is not async-signal-safe and this runs inside a real signal handler, hence
                // snprintf into a fixed stack buffer followed by a single write(2).
                std::array<char, 128> buf{};
                const int len = snprintf(buf.data(), buf.size(), "[MMIO] unrecognized instruction 0x%08x at pc=%p for fault_addr=0x%llx\n",
                                         insn, reinterpret_cast<void*>(pc), static_cast<unsigned long long>(fault_addr));
                if (len > 0)
                {
                    const auto write_len = std::min(static_cast<size_t>(len), buf.size());
                    ::write(STDERR_FILENO, buf.data(), write_len);
                }
#endif
                return false;
            }

            alignas(16) std::array<std::byte, 16> buffer{};
            region.read_cb(fault_addr - region.address, buffer.data(), decoded->size);
            return complete_decoded_load(uctx, *decoded, buffer.data(), pc);
        }

        // LDAR/LDAPR/STLR require natural alignment on real hardware, unlike plain LDR/STR, but x86
        // permits unaligned accesses freely and FEX uses this family to model x86's stronger memory
        // ordering, so an ordinary unaligned guest access to legitimately mapped memory faults here
        // (Darwin reports SIGBUS/BUS_ADRALN). sogen runs every guest thread of a process cooperatively
        // on one host thread, so there is no concurrent host-thread race for these instructions to
        // order against, making the downgrade to a plain access correctness-preserving.
        bool handle_misaligned_atomic_fault(ucontext_t* uctx, uint64_t fault_addr)
        {
            const uint64_t pc = get_host_pc(uctx);

            // FEXCore rewrites a faulting access in its own code into a half-barrier sequence, so it does not trap
            // again. Emulating each access instead traps on every execution.
            if (this->context_ && this->context_->IsAddressInCodeBuffer(this->vcpu_.thread_, pc))
            {
#ifdef __APPLE__
                ::pthread_jit_write_protect_np(0);
                auto* gprs = reinterpret_cast<uint64_t*>(&uctx->uc_mcontext->__ss);
#else
                auto* gprs = reinterpret_cast<uint64_t*>(uctx->uc_mcontext.regs);
#endif
                const auto skip = FEXCore::ArchHelpers::Arm64::HandleUnalignedAccess(
                    this->vcpu_.thread_, FEXCore::ArchHelpers::Arm64::UnalignedHandlerType::HalfBarrier, pc, gprs);
#ifdef __APPLE__
                ::pthread_jit_write_protect_np(1);
#endif
                if (skip)
                {
                    set_host_pc(uctx, pc + *skip);
                    return true;
                }
            }

            const auto insn = *reinterpret_cast<const uint32_t*>(pc);

            if (const auto load = decode_arm64_load(insn))
            {
                return complete_decoded_load(uctx, *load, reinterpret_cast<const void*>(fault_addr), pc);
            }

            if (const auto store = decode_arm64_store(insn))
            {
                const uint64_t value = get_host_gpr(uctx, store->rt);
                std::memcpy(reinterpret_cast<void*>(fault_addr), &value, store->size);
                set_host_pc(uctx, pc + 4);
                return true;
            }

            return false;
        }

        bool handle_general_memory_violation(ucontext_t* uctx, uint64_t fault_addr)
        {
            const uint64_t pc = get_host_pc(uctx);
            const auto guest_fault_addr = this->to_guest_address(fault_addr);

            memory_operation operation = memory_operation::exec;
            size_t access_size = 1;
            if (fault_addr != pc)
            {
                const auto insn = *reinterpret_cast<const uint32_t*>(pc);
#ifdef __linux__
                // The emulation decoder below is intentionally limited to the ordered STLR-family
                // instructions that can fault for alignment. Ordinary translated STR/STUR stores must
                // still be reported as writes on real protection faults, so use the kernel-provided
                // AArch64 ESR WnR bit for access classification instead of broadening that decoder.
                constexpr uint64_t esr_write_not_read = 1ULL << 6;
                if (const auto* esr = find_aarch64_context<esr_context>(uctx, ESR_MAGIC))
                {
                    operation = (esr->esr & esr_write_not_read) != 0 ? memory_operation::write : memory_operation::read;
                }
                else
#elif defined(__APPLE__)
                if (const auto data_abort = data_abort_operation(uctx->uc_mcontext->__es.__esr))
                {
                    operation = *data_abort;
                }
                else
#endif
                {
                    operation = decode_arm64_store(insn) ? memory_operation::write : memory_operation::read;
                }

                if (operation == memory_operation::write)
                {
                    if (const auto store = decode_arm64_store(insn))
                    {
                        access_size = store->size;
                    }
                }
                else if (const auto load = decode_arm64_load(insn))
                {
                    access_size = load->size;
                }
            }

            const auto violation = this->first_range_violation(guest_fault_addr, access_size, operation);
            if (!violation)
            {
                return this->handle_misaligned_atomic_fault(uctx, fault_addr);
            }

            // FEX's block chaining (directly-linked blocks and callret RET fast-paths) advances execution
            // without rewriting CurrentFrame->State.rip, so it holds whatever was last written to it and
            // is frequently stale here. The memory-violation hook reads State.rip as the faulting
            // instruction pointer, so it must be reconstructed from the live host PC - the same mechanism
            // FEX's own ReconstructThreadState uses. Guarded on a non-zero result so a failed
            // reconstruction never zeroes a usable stale rip.
            if (const uint64_t recon_rip = this->context_->RestoreRIPFromHostPC(this->vcpu_.thread_, pc))
            {
                this->vcpu_.thread_->CurrentFrame->State.rip = recon_rip;
            }

            pending_fault_dispatch dispatch{};
            dispatch.kind = pending_fault_kind::memory_violation;
            dispatch.address = guest_fault_addr;
            dispatch.size = access_size;
            dispatch.operation = operation;
            dispatch.type = *violation;

            // SRA is still live only in host registers here - this fault interrupted guest-translated
            // JIT code at an arbitrary point, not FEXCore's own controlled synthetic-exception path.
            this->defer_hook_dispatch(uctx, dispatch, /*state_already_spilled=*/false);
            return true;
        }

        // Returns false for anything it does not recognize, which the wrapper below reports as an
        // unhandled host fault and re-raises with default disposition.
        bool handle_fault_signal(int sig, siginfo_t* info, void* raw_ucontext)
        {
            if (this->vcpu_.thread_ == nullptr)
            {
                return false;
            }

            auto* uctx = static_cast<ucontext_t*>(raw_ucontext);

            if (sig == SIGSEGV || sig == SIGBUS)
            {
                const auto fault_addr = reinterpret_cast<uint64_t>(info->si_addr);

#ifdef __APPLE__
                // Must run before any si_code-specific branch below, because Darwin can report this
                // PROT_NONE violation as BUS_ADRALN instead of SEGV_ACCERR/SEGV_MAPERR. Since
                // InterruptFaultPage is never inside the CodeBuffer, a misclassified fault would
                // otherwise fall through to handle_general_memory_violation, which treats fault_addr as
                // a guest address and would dispatch a synthetic guest exception built from this
                // backend's own internal pointer.
#endif
                const auto interrupt_page_addr = get_untagged_pointer_address(this->vcpu_.thread_->InterruptFaultPage);
                if (fault_addr >= interrupt_page_addr && fault_addr < interrupt_page_addr + sizeof(this->vcpu_.thread_->InterruptFaultPage))
                {
                    const auto fault_pc = get_host_pc(uctx);
                    const bool is_dispatch_code = this->context_ && this->context_->IsAddressInCodeBuffer(this->vcpu_.thread_, fault_pc);

                    // ExitFunctionLinkerAddress's own epilogue (EmitSignalGuardedRegion's closing
                    // sequence) also writes InterruptFaultPage from inside the CodeBuffer, via a
                    // JIT-emitted `strb`, so IsAddressInCodeBuffer cannot tell it apart from a genuine
                    // per-block-entry interrupt check. Redirecting to ThreadStopHandlerAddress while
                    // mid-epilogue pops the dispatcher's frame at the wrong stack depth. The raw
                    // instruction word distinguishes them: STRB (unsigned-offset immediate) encodes
                    // size=00,V=0,opc=00, distinct from both of EmitSuspendInterruptCheck's forms
                    // (64-bit `str` has size=11, the 128-bit vector `str` has V=1).
                    const bool is_strb_epilogue_write = (*reinterpret_cast<const uint32_t*>(fault_pc) & 0xFFC00000u) == 0x39000000u;

                    if (is_dispatch_code && !is_strb_epilogue_write)
                    {
                        // A genuine cooperative stop at a JIT block-entry interrupt check, triggered by
                        // the quantum timer's async mprotect. Live guest state is in host registers
                        // (SRA) and CPUState.rip is frequently stale, since a completed syscall leaves
                        // it at its fallthrough and execution then runs through directly-linked blocks
                        // and callret RET fast-paths that never rewrite it. Resuming from that stale rip
                        // with stale registers re-executes an already-retired instruction - a `retn`
                        // whose return slot was reused pops garbage and branches wild. Both halves are
                        // therefore required: reconstruct the rip from the faulting host PC, and unwind
                        // through the SpillSRA handler so the live SRA registers reach CPUState before
                        // ExecuteThread returns. This mirrors FEX's own ReconstructThreadState.
                        this->vcpu_.thread_->CurrentFrame->State.rip = this->context_->RestoreRIPFromHostPC(this->vcpu_.thread_, fault_pc);
                        this->interrupt_page_unwind_ = true;
                        const auto& stop_cfg = this->signal_delegator_->GetConfig();
                        this->unwind_to_dispatcher_frame(uctx);
                        set_host_pc(uctx, stop_cfg.ThreadStopHandlerAddressSpillSRA);
                        return true;
                    }

                    // Not a block-entry check but ordinary bookkeeping racing a stop request from another
                    // thread: either FEXCore's DeferredSignalRefCountGuard destructor or
                    // ExitFunctionLinkerAddress's JIT-emitted epilogue strb. ThreadStopHandlerAddress
                    // expects to unwind a live JIT dispatcher frame, so taking it from the host-C++ side
                    // corrupts the stack into a pc==lr==0 crash. Only the page's protection state drives
                    // the stop mechanism, not the stored value, so skip the faulting store; the next real
                    // block entry still sees the page protected and stops correctly.
                    set_host_pc(uctx, fault_pc + 4);
                    return true;
                }

#ifndef __APPLE__
                if (sig == SIGSEGV && info->si_code == SEGV_ACCERR && this->handle_tracked_memory_fault(fault_addr, is_write_fault(uctx)))
                {
                    return true;
                }
#endif

#ifdef __APPLE__
                // A W^X instruction-fetch fault on FEXCore's dispatcher trampoline: it is host MAP_JIT
                // code like a CodeBuffer, but IsAddressInCodeBuffer does not recognize it, so the
                // CodeBuffer-gated retries below never fire for it, and this handler's other retry paths
                // can leave the thread's JIT write-protect in write mode. Classified by shape - an
                // instruction fetch inside the dispatcher range - rather than si_code, since Darwin
                // reports this as any of SEGV_ACCERR/SEGV_MAPERR/BUS_ADRALN.
                //
                // Deliberately unbounded, unlike the CodeBuffer cases: a host pc inside the dispatcher
                // is unambiguously FEXCore's own code on genuine RWX-capable MAP_JIT, so toggling
                // execute mode always lets the fetch succeed and can never spin. A spuriously exhausted
                // budget would instead drop the fault into handle_general_memory_violation, which would
                // mis-read the host dispatcher address as a guest access violation.
                {
                    const auto host_pc = get_host_pc(uctx);
                    if (fault_addr == host_pc && this->host_pc_in_dispatcher(host_pc))
                    {
                        ::pthread_jit_write_protect_np(1);
                        return true;
                    }
                }
#endif

                // Checked early, before the general-violation routing that would otherwise mis-dispatch
                // this host arena address as a guest access violation.
                if (this->handle_callret_stack_fault(uctx, fault_addr))
                {
                    return true;
                }

                // An instruction-fetch fault reports si_addr == pc, which a genuine MMIO data access
                // never does, so excluding it costs no real MMIO hits. It matters because a bad branch
                // to a garbage pc can land inside a registered mmio_region by chance; handle_mmio_fault
                // would then try to decode the instruction at that same garbage address and crash there
                // instead, masking the real bug behind a confusing secondary symptom.
                const auto pc_for_mmio_check = get_host_pc(uctx);
                if (fault_addr != pc_for_mmio_check)
                {
                    const auto guest_fault_addr = this->to_guest_address(fault_addr);
                    for (const auto& region : this->mmio_regions_)
                    {
                        if (guest_fault_addr >= region.address && guest_fault_addr < region.address + region.size)
                        {
                            return handle_mmio_fault(uctx, region, guest_fault_addr);
                        }
                    }
                }

#ifdef __APPLE__
                // A BUS_ADRALN whose address is inside the live CodeBuffer but whose PC is FEXCore's own
                // host C++ code (ExitFunctionLink's self-modifying write) is not a real alignment fault
                // at all - it decodes to an ordinary aligned 32-bit `str`. It is the JIT W^X race, which
                // Darwin sometimes reports as BUS_ADRALN instead of SEGV_ACCERR/SEGV_MAPERR (the same
                // si_code ambiguity as the JITGuardPage branch below). Falling through to the BUS_ADRALN
                // branch would unwind via ThreadStopHandlerAddress as if interrupting the JIT
                // dispatcher's own frame - wrong here, since execution is several real C++ frames deep
                // inside FEXCore, corrupting STATE (x28) and other SRA registers with stack garbage and
                // crashing later on an unrelated-looking null-Frame dereference. Treated instead like
                // the write-protect race below, with the same per-address bound.
                if (sig == SIGBUS && info->si_code == BUS_ADRALN && this->context_ &&
                    this->context_->IsAddressInCodeBuffer(this->vcpu_.thread_, fault_addr))
                {
                    auto& retry_count = jit_write_protect_retry_count_for(fault_addr);
                    constexpr int max_write_protect_retries = 4;
                    if (retry_count < max_write_protect_retries)
                    {
                        ++retry_count;
                        ::pthread_jit_write_protect_np(0);
                        return true;
                    }
                }

                // A genuine guest-memory alignment fault (the CodeBuffer case above returns early).
                // Routed through handle_general_memory_violation rather than calling
                // handle_misaligned_atomic_fault directly, because that helper's "legitimately mapped
                // memory" premise is not guaranteed: a guest instruction can compute a garbage address
                // that merely happens to also be unaligned, and memcpy-ing to it would fault a second
                // time inside the signal handler. The shadow-table-validated path classifies such an
                // address correctly and raises it via memory_violation_hooks_ instead.
                if (sig == SIGBUS && info->si_code == BUS_ADRALN && this->handle_general_memory_violation(uctx, fault_addr))
                {
                    return true;
                }
#endif
            }

#ifdef __APPLE__
            if ((sig == SIGSEGV || sig == SIGBUS) && (info->si_code == SEGV_ACCERR || info->si_code == SEGV_MAPERR))
#else
            if (sig == SIGSEGV || sig == SIGBUS)
#endif
            {
                const auto guard_page = this->vcpu_.thread_->JITGuardPage;
                const auto fault_addr = reinterpret_cast<uintptr_t>(info->si_addr);
                if (guard_page != 0 && fault_addr >= guard_page && fault_addr < guard_page + FEXCore::Utils::FEX_HOST_PAGE_SIZE)
                {
#ifdef __APPLE__
                    auto* gprs = reinterpret_cast<uint64_t*>(&uctx->uc_mcontext->__ss);
                    auto* pc_ptr = reinterpret_cast<uint64_t*>(&uctx->uc_mcontext->__ss.__pc);
#else
                    auto* gprs = reinterpret_cast<uint64_t*>(uctx->uc_mcontext.regs);
                    auto* pc_ptr = reinterpret_cast<uint64_t*>(&uctx->uc_mcontext.pc);
#endif
                    auto* fprs = get_host_vector_registers(uctx);
                    if (fprs == nullptr)
                    {
                        return false;
                    }
                    FEXCore::UncheckedLongJump::ManuallyLoadJumpBuf(this->vcpu_.thread_->RestartJump,
                                                                    this->vcpu_.thread_->JITGuardOverflowArgument, gprs, fprs, pc_ptr);
                    return true;
                }

#ifdef __APPLE__
                // FEXCore's code-patching paths (ExitFunctionLink, block delinkers) do not reliably
                // leave this thread's JIT write-protect state correct across their self-modifying
                // writes, so set whatever the faulting access needs and retry the same instruction.
                // An instruction fetch (PC == fault address) needs execute mode, a data write needs
                // write mode; guessing wrong just re-faults and consumes a retry harmlessly.
                //
                // The IsAddressInCodeBuffer gate matters: without it the branch fires for any
                // SEGV_ACCERR/SEGV_MAPERR, burning retries toggling W^X on faults that were never a
                // CodeBuffer race - a branch-to-null would be retried this way before falling through.
                if (this->context_ && this->context_->IsAddressInCodeBuffer(this->vcpu_.thread_, fault_addr))
                {
                    const auto fault_addr_u64 = reinterpret_cast<uint64_t>(info->si_addr);
                    auto& retry_count = jit_write_protect_retry_count_for(fault_addr_u64);
                    constexpr int max_write_protect_retries = 4;
                    if (retry_count < max_write_protect_retries)
                    {
                        ++retry_count;
                        const uint64_t faulting_pc = get_host_pc(uctx);
                        const bool is_instruction_fetch = (faulting_pc == fault_addr_u64);
                        ::pthread_jit_write_protect_np(is_instruction_fetch ? 1 : 0);
                        return true;
                    }
                }
#endif
            }

            const uint64_t pc = get_host_pc(uctx);

            // FEXCore's guest-exception trampoline always re-enters within the dispatcher range.
            if (!this->host_pc_in_dispatcher(pc))
            {
                // The common case: translated guest code faulted directly. Only attempted once pc is
                // confirmed inside a live JIT code buffer - anything else is a host bug elsewhere that
                // must not be interpreted as guest state, and the wrapper below logs and re-raises it.
                if ((sig == SIGSEGV || sig == SIGBUS) && this->context_ && this->context_->IsAddressInCodeBuffer(this->vcpu_.thread_, pc) &&
                    this->handle_general_memory_violation(uctx, reinterpret_cast<uint64_t>(info->si_addr)))
                {
                    return true;
                }

                return false;
            }

            auto* frame = this->vcpu_.thread_->CurrentFrame;
            if (!frame->SynchronousFaultData.FaultToTopAndGeneratedException)
            {
                return false;
            }

            // FEXCore's IR "Break" op raises this both for x86 conditions with a compile-time-known trap
            // vector (HLT/UD2/INT3/INT1/INTO/unhandled INT N) and for its own synthetic #PF (NoExecOp,
            // when QueryGuestExecutableRange reports an address is not executable), so vector 14 needs
            // the fault address while everything else is a plain exception vector. Mirrors the KVM
            // backend's #PF vs. other-vector split in handle_exception().
            auto vector = static_cast<int>(frame->SynchronousFaultData.TrapNo);

            // sogen has no guest IDT, so a guest `INT N` FEXCore cannot dispatch synthesizes a #GP(13)
            // whose error code names the referenced IDT selector (bit 1 set, index in bits[15:3]), just
            // as real hardware does for an absent IDT gate. Unicorn/KVM model no IDT lookup and report
            // `INT N` as vector N, so FEX's more faithful #GP is remapped back to the plain vector the
            // shared interrupt dispatch expects - otherwise a __fastfail `int 0x29` re-faults forever.
            constexpr int gp_fault_vector = 13;
            constexpr uint32_t idt_reference_bit = 0x2;
            if (vector == gp_fault_vector && (frame->SynchronousFaultData.err_code & idt_reference_bit) != 0)
            {
                vector = static_cast<int>(frame->SynchronousFaultData.err_code >> 3);
            }

            // Must be reset before deferring, not after: it gates re-entry into this branch, and the
            // hook does not run until run() reaches it, so the next real fault of this shape would
            // otherwise be swallowed in the meantime.
            frame->SynchronousFaultData.FaultToTopAndGeneratedException = false;

            pending_fault_dispatch dispatch{};
            if (vector == 14)
            {
                const auto err_code = frame->SynchronousFaultData.err_code;
                // NoExecOp (the only current producer of a synthetic #PF) always faults on the
                // instruction fetch at the current guest RIP - there is no separate stored fault
                // address (real x86 would use CR2), so RIP is the only correct source for now.
                const bool is_write = (err_code & 0x2) != 0;
                const bool is_instr_fetch = (err_code & 0x10) != 0;
                dispatch.kind = pending_fault_kind::memory_violation;
                dispatch.address = frame->State.rip;
                dispatch.size = 1;
                if (is_instr_fetch)
                {
                    dispatch.operation = memory_operation::exec;
                }
                else
                {
                    dispatch.operation = is_write ? memory_operation::write : memory_operation::read;
                }
                dispatch.type = (err_code & 0x1) ? memory_violation_type::protection : memory_violation_type::unmapped;
            }
            else
            {
                // FEXCore's INT3 translation stores the post-instruction RIP (INTOp's SetRIPToNext),
                // while NT and the KVM/WHP backends report #BP at the 0xCC itself. Only the direct #BP
                // needs normalizing: a GP-remapped `INT N` (e.g. the `CD 2D` debug service trap, whose
                // bytes exception_dispatch.cpp expects to find at RIP) already stores its own address.
                if (frame->SynchronousFaultData.TrapNo == FEXCore::X86State::X86_TRAPNO_BP)
                {
                    frame->State.rip -= 1;
                }

                dispatch.kind = pending_fault_kind::interrupt;
                dispatch.vector = vector;
            }

            // SRA is already spilled: this is FEXCore's own controlled synthetic-exception/Break-op
            // path, not an arbitrary interruption of live JIT code.
            this->defer_hook_dispatch(uctx, dispatch, /*state_already_spilled=*/true);
            return true;
        }

      private:
        void run(fex_vcpu& vcpu, size_t count)
        {
            if (count != 0)
            {
                if (!this->counts_instructions_)
                {
                    this->counts_instructions_ = true;
                    this->add_instruction_callback_range(0, std::numeric_limits<uint64_t>::max());
                }
                this->instruction_budget_ = count;
            }
            const auto _ = utils::finally([this] { this->instruction_budget_.reset(); });

#if SOGEN_FEX_HYPERVISOR
            if (this->hypervisor_)
            {
                vcpu.start_in_vm();
                return;
            }
#endif

            if (vcpu.thread_ == nullptr)
            {
                this->create_thread(vcpu);
            }

            vcpu.stop_requested_ = false;
            // Re-arm InterruptFaultPage for this quantum - see request_thread_stop's doc comment; a
            // prior stop() may have left it protected to force the last quantum's ExecuteThread to
            // return, and it must be writable again before the JIT's per-block-entry store runs.
            ::mprotect(vcpu.thread_->InterruptFaultPage, sizeof(vcpu.thread_->InterruptFaultPage), PROT_READ | PROT_WRITE);

            // ExecuteThread runs the translated guest until the thread is asked to stop (which the
            // syscall bridge does when a hook calls stop()), or the guest faults/exits.
            // It can also return early because handle_fault_signal deferred a hook dispatch (see
            // pending_fault_dispatch_) rather than genuinely stopping - dispatch it in normal call
            // context, where that is safe, then resume by calling ExecuteThread again; it always
            // restarts from CurrentFrame->State.rip, which the hook is free to have redirected.
            for (;;)
            {
                this->context_->ExecuteThread(vcpu.thread_);

                const bool hook_dispatched = this->dispatch_pending_hook_if_any(vcpu);
                const bool interrupt_page_unwind = this->interrupt_page_unwind_.exchange(false);

                // An InterruptFaultPage unwind with no stop pending is the quantum timer racing this
                // quantum's own entry: stop() sets stop_requested_ then protects the page, but a
                // concurrently-entered run() has already cleared the flag and the timer's mprotect
                // only lands after the re-arm above, so the first block-entry check faults with nothing
                // requested. Treating that as a real stop makes windows_emulator::vcpu_worker read it as
                // a fatal wind-down, so re-arm and resume instead; the timer's pending switch_thread
                // request is honored at the next genuine stop.
                if (vcpu.stop_requested_ || (!hook_dispatched && !interrupt_page_unwind))
                {
                    break;
                }

                if (interrupt_page_unwind)
                {
                    ::mprotect(vcpu.thread_->InterruptFaultPage, sizeof(vcpu.thread_->InterruptFaultPage), PROT_READ | PROT_WRITE);
                }
            }
        }

        void create_thread(fex_vcpu& vcpu)
        {
            // Other backends start in 64-bit mode without a descriptor table, but FEX decodes in the mode the CS
            // descriptor selects, so until the caller loads one the thread uses the flat Windows x64 table.
            if (vcpu.staged_state_.segment_arrays[0] == nullptr)
            {
                vcpu.staged_state_.segment_arrays[0] = reinterpret_cast<FEXCore::Core::CPUState::gdt_segment*>(vcpu.default_gdt_.data());
            }
            if (vcpu.staged_state_.cs_idx == 0)
            {
                vcpu.staged_state_.cs_idx = 0x33;
                vcpu.staged_state_.ss_idx = 0x2B;
                vcpu.staged_state_.ds_idx = 0x2B;
                vcpu.staged_state_.es_idx = 0x2B;
            }

            // Seed the FEX thread from the staged CPUState the loader populated before the first start().
            vcpu.thread_ =
                this->context_->CreateThread(vcpu.staged_state_.rip, vcpu.staged_state_.gregs[detail::greg_rsp], &vcpu.staged_state_);

            // FEXCore's core does not set up the call-ret shadow stack; on Linux that is embedder glue
            // in ThreadManager::CreateThread, replicated here. Without it the first x86 CALL in compiled
            // code dereferences a null callret_sp and crashes.
            this->ensure_callret_stack(vcpu.thread_, vcpu.thread_->CurrentFrame->State);

#if SOGEN_FEX_HYPERVISOR
            if (this->hypervisor_)
            {
                vcpu.prepare_vm_thread();
                return;
            }
#endif

#ifdef __APPLE__
            // See exit_function_link_jit_write_wrapper: the call-site patch must happen with this
            // thread's JIT write-protection disabled.
            g_original_exit_function_link = vcpu.thread_->CurrentFrame->Pointers.ExitFunctionLink;
            vcpu.thread_->CurrentFrame->Pointers.ExitFunctionLink = reinterpret_cast<uint64_t>(&exit_function_link_jit_write_wrapper);
#endif
        }

        // FEXCore's call-ret shadow stack has no notion of "logical guest thread" - callret_sp is just a
        // raw pointer into whatever host buffer this sets up. sogen models logical guest threads as
        // CPUState snapshots swapped in and out of one FEXCore thread, so a shared buffer would let one
        // logical thread's pushes corrupt the pending frames of another suspended mid-call-chain. Each
        // gets its own, identified by state._pad1, the unused CPUState padding right after callret_sp,
        // doubling as a marker: 0 means this snapshot has never been assigned one, non-zero is the
        // buffer's base, safe to reuse since it round-trips with the rest of the snapshot. Callers point
        // InternalThreadState::CallRetStackBase at it themselves.
        void ensure_callret_buffer(FEXCore::Core::InternalThreadState* thread, FEXCore::Core::CPUState& state)
        {
            if (state._pad1 == 0)
            {
                // Guard pages on both sides, sized to the real host page (getpagesize(), not the
                // guest's fixed 4KB) so mprotect can't spill onto the guard.
                const size_t host_page = static_cast<size_t>(::getpagesize());
                constexpr size_t callret_stack_size = FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE;
                const size_t callret_alloc_size = callret_stack_size + 2 * host_page;

                // Routed through FEXCore::Allocator::mmap rather than raw ::mmap so that on Apple it
                // hits the fex_internal_arena hook and lands inside the guest-excluded arena. The JIT
                // consumes callret_sp as a plain host pointer, so a buffer aliasing the live guest stack
                // would let a host-side push/pop scribble guest memory with no guest instruction
                // involved. On Linux the hook defaults to raw ::mmap, so behavior there is unchanged.
                void* alloc_base = FEXCore::Allocator::mmap(nullptr, callret_alloc_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                if (alloc_base == MAP_FAILED)
                {
                    throw std::runtime_error("FEX backend failed to allocate the call-ret stack");
                }

                auto* callret_stack_base = static_cast<uint8_t*>(alloc_base) + host_page;
                if (::mprotect(callret_stack_base, callret_stack_size, PROT_READ | PROT_WRITE) != 0)
                {
                    throw std::runtime_error("FEX backend failed to make the call-ret stack writable");
                }

                state._pad1 = reinterpret_cast<uint64_t>(callret_stack_base);
                // Leave headroom for underflows without hitting the guard page immediately, matching
                // ThreadManager::GetCallRetStackInfo's DefaultLocation (Base + size/4).
                state.callret_sp = reinterpret_cast<uint64_t>(callret_stack_base) + callret_stack_size / 4;

                this->callret_buffers_.emplace(state._pad1, callret_buffer_record{
                                                                .allocation_base = alloc_base,
                                                                .allocation_size = callret_alloc_size,
                                                                .code_buffer_generation = thread->CodeBufferGeneration,
                                                            });
            }
        }

        // Ensures the thread's call-ret buffer exists and points CallRetStackBase at it.
        void ensure_callret_stack(FEXCore::Core::InternalThreadState* thread, FEXCore::Core::CPUState& state)
        {
            this->ensure_callret_buffer(thread, state);
            thread->CallRetStackBase = reinterpret_cast<void*>(state._pad1);
        }

        static void invalidate_code_range_in(FEXCore::Context::Context* context, FEXCore::Core::InternalThreadState* thread,
                                             uint64_t address, size_t size)
        {
            if (context == nullptr)
            {
                return;
            }

            // InvalidateCodeBuffersCodeRange/InvalidateThreadCachedCodeRange both require the caller
            // to already hold GetCodeInvalidationMutex() exclusively (see FEXCore's own
            // ThreadManager::InvalidateGuestCodeRange, the canonical caller on Linux) - without it,
            // CompileBlock's shared lock deadlocks permanently the first time this runs.
            std::unique_lock lock(context->GetCodeInvalidationMutex());

#ifdef __APPLE__
            // Invalidating a range can synchronously delink already-linked call sites (AddBlockLink's
            // delinker callbacks), writing into a MAP_JIT buffer - the same per-thread write-protect
            // requirement as exit_function_link_jit_write_wrapper.
            ::pthread_jit_write_protect_np(0);
#endif
            context->InvalidateCodeBuffersCodeRange(address, size);
            if (thread != nullptr)
            {
                context->InvalidateThreadCachedCodeRange(thread, address, size);
            }
#ifdef __APPLE__
            ::pthread_jit_write_protect_np(1);
#endif
        }

        void invalidate_code_range(uint64_t address, size_t size)
        {
            invalidate_code_range_in(this->context_.get(), this->vcpu_.thread_, address, size);
            this->code_pages_.erase(this->code_pages_.lower_bound(address & ~(page_size - 1)),
                                    this->code_pages_.lower_bound(address + size));
        }

        bool range_has_code(uint64_t address, size_t size) const
        {
            const auto page = this->code_pages_.lower_bound(address & ~(page_size - 1));
            return page != this->code_pages_.end() && *page < address + size;
        }

        emulator_hook* make_hook()
        {
            return reinterpret_cast<emulator_hook*>(this->next_hook_id_++);
        }

        // state

        fextl::unique_ptr<FEXCore::Context::Context> context_{};
        fex_vcpu vcpu_{*this};

        std::unique_ptr<fex_syscall_handler> syscall_handler_{};
        // Does no fault handling; the plain base only carries the dispatcher config
        // (ThreadStopHandlerAddress, DispatcherBegin/End) that handle_fault_signal reads.
        std::unique_ptr<FEXCore::SignalDelegator> signal_delegator_{};

        uintptr_t next_hook_id_ = 1;

        pending_fault_dispatch pending_fault_dispatch_{};
        // Set by handle_fault_signal on an InterruptFaultPage unwind, consumed by run()'s loop to tell
        // it apart from any other clean return. Atomic even though both ends are the same thread: the
        // C++ abstract machine has no signal-delivery control-flow edge, so an optimizer may treat this
        // as unmodified across the opaque ExecuteThread() call and cache a stale read.
        std::atomic<bool> interrupt_page_unwind_{false};

        bool hypervisor_{};
#if SOGEN_FEX_HYPERVISOR
        std::unique_ptr<arm64_emulator> vm_{};
        std::byte* vm_stack_{};
        image_range fex_image_{};
        std::vector<uint64_t> host_calls_{};
        std::string vm_failure_{};
#endif

        std::map<uint64_t, mapped_region> regions_;
        // The pages FEX has translated code from since they were last invalidated, as it reports them. Only writes to
        // these need to invalidate code; FEX only reports a page again once its code has been invalidated.
        std::set<uint64_t> code_pages_;

#ifdef __linux__
        uint64_t host_address_end_{find_host_address_end()};
#endif
        uint64_t guest_arena_{};

        std::vector<mmio_region> mmio_regions_;
        // Bringing a stale chunk up to date only changes host protections and caches, which reads do as well.
        mutable std::map<uint64_t, tracked_range> tracked_ranges_;

        // Every per-logical-thread call-ret buffer ever allocated by ensure_callret_buffer, so the
        // destructor can release them - these live outside regions_ (host allocator space, not the
        // guest address space) and outlive any individual CPUState snapshot they were allocated for.
        std::unordered_map<uint64_t, callret_buffer_record> callret_buffers_;
        uint64_t instance_id_{std::random_device{}() | (static_cast<uint64_t>(std::random_device{}()) << 32)};

        void* callret_allocation_{};
        size_t callret_allocation_size_{};

        std::map<uint64_t, size_t> claimed_host_ranges_;
        std::vector<std::pair<uint64_t, size_t>> fixed_guest_ranges_;

        // Host base of the window backing a WoW64 process's low 4GB, or 0 while guest VA == host VA.
        uint64_t low_guest_window_{};

#ifdef __APPLE__
        // Apple Silicon's 16KB host page is coarser than the guest's 4KB architectural page, so one host
        // mprotect cannot always express what the guest requested per 4KB page - a PE image's .text (RX)
        // directly followed by .data (RW) share a host page. page_shadow_apple_ is the per-4KB source of
        // truth (absent = never requested, which must still fault).
        std::map<uint64_t, memory_permission> page_shadow_apple_;
#endif

        hook_entry* syscall_hook_ = nullptr;
        static constexpr int invalid_opcode_vector = 6;
        bool traps_timestamp_and_cpuid_ = false;
        std::unordered_map<emulator_hook*, hook_entry> instruction_hooks_;
        std::unordered_map<emulator_hook*, interrupt_hook_callback> interrupt_hooks_;
        std::unordered_map<emulator_hook*, memory_access_hook_callback> memory_read_hooks_;
        std::unordered_map<emulator_hook*, memory_access_hook_callback> memory_write_hooks_;

        struct execution_hook
        {
            uint64_t start{};
            uint64_t size{};
            memory_execution_hook_callback callback{};
        };

        std::map<emulator_hook*, execution_hook> memory_execution_hooks_;
        std::unordered_map<uint64_t, std::vector<emulator_hook*>> execution_hooks_by_address_;
        std::vector<emulator_hook*> range_execution_hooks_;
        std::vector<emulator_hook*> deleted_execution_hooks_;
        bool running_execution_hooks_ = false;
        std::optional<size_t> instruction_budget_{};
        bool counts_instructions_ = false;
        std::vector<std::pair<uint64_t, uint64_t>> instruction_callback_ranges_;
        std::unordered_map<emulator_hook*, memory_violation_hook_callback> memory_violation_hooks_;
        std::unordered_map<emulator_hook*, basic_block_hook_callback> basic_block_hooks_;
    };

    namespace
    {
        // Keep this function async-signal-safe: it may run from a signal handler while
        // libc or other runtime code is in an inconsistent/locked state. Avoid stdio,
        // snprintf, allocation, locks, and other non-async-signal-safe operations.
        // Formatting is therefore done manually into a fixed stack buffer, followed by
        // a single write(2), which is async-signal-safe.
        void log_unhandled_signal(int sig, const siginfo_t* info, ucontext_t* uctx)
        {
            const int saved_errno = errno;

            std::array<char, 160> buf{};
            char* out = buf.data();
            const char* const end = buf.data() + buf.size();

            const auto append_string = [&](const char* str) {
                while (*str != '\0' && out < end)
                {
                    *out++ = *str++;
                }
            };

            const auto append_decimal = [&](int64_t value) {
                std::array<char, 20> digits{};
                size_t count = 0;

                uint64_t magnitude = 0;
                if (value < 0)
                {
                    if (out < end)
                    {
                        *out++ = '-';
                    }

                    magnitude = static_cast<uint64_t>(-(value + 1)) + 1;
                }
                else
                {
                    magnitude = static_cast<uint64_t>(value);
                }

                do
                {
                    digits[count++] = static_cast<char>('0' + magnitude % 10);
                    magnitude /= 10;
                } while (magnitude != 0);

                while (count != 0 && out < end)
                {
                    *out++ = digits[--count];
                }
            };

            const auto append_hex = [&](uint64_t value) {
                constexpr std::string_view hex_digits = "0123456789abcdef";

                std::array<char, 16> digits{};
                size_t count = 0;

                do
                {
                    digits[count++] = hex_digits[value & 0xf];
                    value >>= 4;
                } while (value != 0);

                while (count != 0 && out < end)
                {
                    *out++ = digits[--count];
                }
            };

            append_string("[FEX backend] unhandled signal ");
            append_decimal(sig);

            append_string(" si_code=");
            append_decimal(info->si_code);

            append_string(" at pc=0x");
#ifdef __APPLE__
            append_hex(arm_thread_state64_get_pc(uctx->uc_mcontext->__ss));
#else
            append_hex(uctx->uc_mcontext.pc);
#endif

            append_string(" fault_addr=0x");
            append_hex(reinterpret_cast<uintptr_t>(info->si_addr));

            if (out < end)
            {
                *out++ = '\n';
            }

            if (out != buf.data())
            {
                (void)::write(STDERR_FILENO, buf.data(), static_cast<size_t>(out - buf.data()));
            }

            errno = saved_errno;
        }

        bool is_sent_signal(const siginfo_t& info)
        {
#ifdef __APPLE__
            // Darwin reports some hardware traps, like the SIGTRAP of a BRK, with si_code 0, and sent signals with
            // SI_USER/SI_QUEUE, which are positive there.
            return info.si_code == SI_USER || info.si_code == SI_QUEUE;
#else
            return info.si_code <= 0;
#endif
        }

        void forward_sent_signal(const struct sigaction& previous, const int sig, siginfo_t* info, void* raw_ucontext)
        {
            if ((previous.sa_flags & SA_SIGINFO) != 0)
            {
                previous.sa_sigaction(sig, info, raw_ucontext);
            }
            else if (previous.sa_handler == SIG_DFL)
            {
                struct sigaction default_action{};
                default_action.sa_handler = SIG_DFL;
                ::sigaction(sig, &default_action, nullptr);
                ::raise(sig);
            }
            else if (previous.sa_handler != SIG_IGN)
            {
                previous.sa_handler(sig);
            }
        }

        void fault_signal_handler(int sig, siginfo_t* info, void* raw_ucontext)
        {
            auto* uctx = static_cast<ucontext_t*>(raw_ucontext);
            const auto& previous =
                g_previous_fault_actions[static_cast<size_t>(std::ranges::find(fault_signals, sig) - fault_signals.begin())];

            // A signal that was sent rather than caused by an instruction is no fault. It goes to the previous handler without
            // uninstalling this one, since that handler may return and later faults still need handling here.
            if (is_sent_signal(*info))
            {
                forward_sent_signal(previous, sig, info, raw_ucontext);
                return;
            }

            const bool handled = g_active_emulator != nullptr && g_active_emulator->handle_fault_signal(sig, info, raw_ucontext);
            if (handled)
            {
                return;
            }

            log_unhandled_signal(sig, info, uctx);

            // Returning re-executes the faulting instruction, which raises the signal again for the previous handler.
            ::sigaction(sig, &previous, nullptr);
        }
    } // namespace

#if SOGEN_FEX_HYPERVISOR
    // Every call from translated code into host C++ goes through a JITPointers slot or the dispatcher's literal
    // pool; each target is replaced by an address the VM does not map. Slot 0 is the dispatcher's return to us.
    void fex_vcpu::prepare_vm_thread()
    {
        auto& pointers = this->thread_->CurrentFrame->Pointers;
        this->emulator_.host_calls_.assign(1, 0);
        const auto redirect = [&](uint64_t& slot) {
            if (slot == 0)
            {
                return;
            }
            this->emulator_.host_calls_.push_back(slot);
            slot = fex_x86_64_emulator::host_call_base + fex_x86_64_emulator::host_call_stride * (this->emulator_.host_calls_.size() - 1);
        };

        for (auto* slot :
             {&pointers.PrintValue, &pointers.PrintVectorValue, &pointers.PrintMsgValue, &pointers.ThreadRemoveCodeEntryFromJIT,
              &pointers.CPUIDFunction, &pointers.XCRFunction, &pointers.SyscallHandlerFunc, &pointers.ExitFunctionLink,
              &pointers.MonoBackpatcherWrite, &pointers.LUDIV, &pointers.LDIV})
        {
            redirect(*slot);
        }
        for (auto& fallback : pointers.FallbackHandlerPointers)
        {
            redirect(fallback.Func);
        }

        // The dispatcher ends with its literal pool: CTX, SleepThread, CompileBlock, CompileSingleStep.
        auto* literals = reinterpret_cast<uint64_t*>(this->emulator_.signal_delegator_->GetConfig().DispatcherEnd) - 4;
        if (literals[0] != reinterpret_cast<uint64_t>(this->emulator_.context_.get()))
        {
            throw std::runtime_error("FEX dispatcher literal pool not found");
        }
        for (size_t index = 1; index < 4; ++index)
        {
            redirect(literals[index]);
        }
    }

    // FEX stops a thread by making the page every block entry stores to fault; in the VM, by removing it there.
    void fex_vcpu::set_interrupt_page_in_vm(const bool armed)
    {
        if (this->thread_ == nullptr || !this->emulator_.vm_)
        {
            return;
        }
        hvf::share_host_memory(*this->emulator_.vm_, reinterpret_cast<uint64_t>(this->thread_->InterruptFaultPage),
                               sizeof(this->thread_->InterruptFaultPage),
                               armed ? std::optional{memory_permission::read_write} : std::nullopt);
    }

    bool fex_vcpu::in_interrupt_page(const uint64_t address) const
    {
        const auto page = reinterpret_cast<uint64_t>(this->thread_->InterruptFaultPage);
        return address >= page && address < page + sizeof(this->thread_->InterruptFaultPage);
    }

    void fex_vcpu::start_in_vm()
    {
        if (this->thread_ == nullptr)
        {
            this->emulator_.create_thread(*this);
        }

        this->stop_requested_ = false;
        this->set_interrupt_page_in_vm(true);

        // As in-process: a deferred hook or an InterruptFaultPage unwind without a stop resumes the guest.
        for (;;)
        {
            this->run_dispatcher_in_vm();

            const bool hook_dispatched = this->emulator_.dispatch_pending_hook_if_any(*this);
            const bool interrupt_page_unwind = this->emulator_.interrupt_page_unwind_.exchange(false);
            if (this->stop_requested_ || (!hook_dispatched && !interrupt_page_unwind))
            {
                break;
            }
            if (interrupt_page_unwind)
            {
                this->set_interrupt_page_in_vm(true);
            }
        }
    }

    // The VM's counterpart of ExecuteThread: enters the dispatcher with the thread's frame and runs until the
    // dispatcher returns to its caller.
    void fex_vcpu::run_dispatcher_in_vm()
    {
        auto& vm = *this->emulator_.vm_;
        vm.reg(arm64_register::pc, this->emulator_.signal_delegator_->GetConfig().DispatcherBegin);
        vm.reg(arm64_register::x0, reinterpret_cast<uint64_t>(this->thread_->CurrentFrame));
        vm.reg(arm64_register::x1, 0);
        vm.reg(arm64_register::x30, fex_x86_64_emulator::host_call_base);
        vm.reg(arm64_register::sp, reinterpret_cast<uint64_t>(this->emulator_.vm_stack_) + fex_x86_64_emulator::vm_stack_size);

        this->returned_to_host_ = false;
        while (!this->returned_to_host_ && this->emulator_.vm_failure_.empty())
        {
            vm.start();
        }
        if (!this->emulator_.vm_failure_.empty())
        {
            throw std::runtime_error(std::exchange(this->emulator_.vm_failure_, {}));
        }
    }
#endif

    memory_interface& fex_vcpu::memory()
    {
        return this->emulator_;
    }

    const memory_interface& fex_vcpu::memory() const
    {
        return this->emulator_;
    }

    void fex_vcpu::start(const size_t count)
    {
        this->emulator_.run(*this, count);
    }

    // The whole architectural state lives in a single CPUState struct, followed by the id of the emulator it came from.
    std::vector<std::byte> fex_vcpu::save_registers() const
    {
        const auto& state = this->cpu_state();
        std::vector<std::byte> data(sizeof(FEXCore::Core::CPUState) + sizeof(this->emulator_.instance_id_));
        std::memcpy(data.data(), &state, sizeof(state));
        std::memcpy(data.data() + sizeof(state), &this->emulator_.instance_id_, sizeof(this->emulator_.instance_id_));
        return data;
    }

    void fex_vcpu::restore_registers(const std::vector<std::byte>& register_data)
    {
        FEXCore::Core::CPUState restored{};
        uint64_t instance_id{};
        if (register_data.size() != sizeof(restored) + sizeof(instance_id))
        {
            throw std::runtime_error("FEX register snapshot has unexpected size");
        }

        std::memcpy(&restored, register_data.data(), sizeof(restored));
        std::memcpy(&instance_id, register_data.data() + sizeof(restored), sizeof(instance_id));

        // The call-ret buffer, descriptor tables and JIT blocks are host memory of the emulator that saved the state,
        // so a state from a saved process gets its own.
        if (instance_id != this->emulator_.instance_id_)
        {
            restored._pad1 = 0;
            restored.callret_sp = 0;
            restored.InlineJITBlockHeader = 0;
        }

        if (this->thread_ == nullptr)
        {
            // No thread yet: writing into staged_state_, which create_thread() will seed the
            // real thread from (including installing L1Pointer/L1Mask/callret_sp correctly
            // itself afterward).
            const auto segment_arrays = std::to_array(this->staged_state_.segment_arrays);
            std::memcpy(&this->staged_state_, &restored, sizeof(restored));
            std::ranges::copy(segment_arrays, std::begin(this->staged_state_.segment_arrays));
            return;
        }

        this->emulator_.restore_state_into(this->thread_, restored);
    }

    void fex_vcpu::notify_process_bitness(const bool is_wow64_process)
    {
        this->emulator_.notify_process_bitness(is_wow64_process);
    }

    void fex_vcpu::load_gdt(const pointer_type address, const uint32_t limit)
    {
        // Only remember the base/limit for callers querying gdtr (see read_descriptor_table).
        this->gdt_base_ = address;
        this->gdt_limit_ = limit;

        // sogen writes real GDT descriptors (matching FEXCore::Core::CPUState::gdt_segment's
        // bitfield layout byte-for-byte) directly into guest memory at `address`. Since guest VA
        // == host VA under this backend's model, point FEX's own segment table at that same
        // memory instead of duplicating it - CS/segment lookups (GetSegmentFromIndex) then see
        // whatever sogen's loader wrote, including the long-mode (L) bit, with no extra sync step.
        this->cpu_state().segment_arrays[0] =
            reinterpret_cast<FEXCore::Core::CPUState::gdt_segment*>(this->emulator_.to_host_address(address));
    }

    uint64_t fex_vcpu::read_rflags() const
    {
        // FEXCore's ReconstructCompactedEFLAGS requires a live thread (it dereferences Thread to
        // reach CurrentFrame->State); before create_thread(), fall back to the local
        // reimplementation operating on the staged CPUState directly (see reconstruct_compacted_eflags).
        if (this->thread_ != nullptr)
        {
            // At rest (not in JIT) WasInJIT=false and the host GPR/PSTATE inputs are unused.
            return this->emulator_.context_->ReconstructCompactedEFLAGS(this->thread_, /*WasInJIT=*/false, nullptr, 0);
        }
        return reconstruct_compacted_eflags(this->staged_state_);
    }

    void fex_vcpu::write_rflags(const uint64_t rflags)
    {
        if (this->thread_ != nullptr)
        {
            this->emulator_.context_->SetFlagsFromCompactedEFLAGS(this->thread_, static_cast<uint32_t>(rflags));
            return;
        }
        set_flags_from_compacted_eflags(this->staged_state_, static_cast<uint32_t>(rflags));
    }

    uint64_t fex_syscall_handler::HandleSyscall(FEXCore::Core::CpuStateFrame* /*frame*/, FEXCore::HLE::SyscallArguments* args)
    {
        if (args->Argument[0] == FEXCore::HLE::InstructionCallbackID)
        {
            return this->emulator_.on_instruction(this->emulator_.vcpu_.cpu_state().rip);
        }

        // SyscallOp sets CPUState.rip to the address of the `syscall` instruction itself before invoking
        // us, which is the convention sogen's shared syscall layer expects: it leaves rip so that
        // advancing by the 2-byte syscall length always yields the intended next rip - the following
        // instruction for a plain syscall, and the real target for a redirecting one, which sets
        // rip = target - 2. On non-_WIN32 hosts `syscall` is not FLAGS_BLOCK_END, so without the
        // unconditional advance below the JIT falls through and either re-executes the syscall on block
        // re-entry or, for a redirect, branches into the middle of an instruction at target - 2.
        auto& vcpu = this->emulator_.vcpu_;
        auto* hook = this->emulator_.syscall_hook_;
        if (hook != nullptr && hook->callback)
        {
            // The Windows syscall layer reads/writes guest registers itself through the emulator, so the
            // hook needs no data argument here. It places the NT status in RAX before returning.
            hook->callback(vcpu, 0);
        }

        vcpu.cpu_state().rip += 2;

        if (vcpu.stop_requested_)
        {
            vcpu.request_thread_stop();
        }

        // FEX writes our return value into guest RAX; hand back whatever the hook already set so the
        // value is preserved.
        return vcpu.cpu_state().gregs[detail::greg_rax];
    }

    FEXCore::HLE::ExecutableRangeInfo fex_syscall_handler::QueryGuestExecutableRange(FEXCore::Core::InternalThreadState* /*thread*/,
                                                                                     uint64_t address)
    {
        // FEXCore checks this before compiling a guest address and synthesizes a #PF if it is not
        // reported executable here, so a blanket {} would make every guest instruction fetch look like
        // a DEP violation.
        auto& regions = this->emulator_.regions_;
        auto it = regions.upper_bound(address);
        if (it == regions.begin())
        {
            return {};
        }
        --it;
        const uint64_t region_end = it->first + it->second.size;
        if (address < it->first || address >= region_end)
        {
            return {};
        }

        const auto perms = it->second.permissions;
        if ((perms & memory_permission::exec) == memory_permission::none)
        {
            return {};
        }

        return {
            .Base = it->first,
            .Size = region_end - it->first,
            .Writable = (perms & memory_permission::write) != memory_permission::none,
        };
    }

    void fex_syscall_handler::MarkGuestExecutableRange(FEXCore::Core::InternalThreadState* /*thread*/, const uint64_t start,
                                                       const uint64_t length)
    {
        for (uint64_t page = start & ~(page_size - 1); page < start + length; page += page_size)
        {
            this->emulator_.code_pages_.insert(page);
        }
    }

    std::optional<FEXCore::ExecutableFileSectionInfo> fex_syscall_handler::LookupExecutableFileSection(
        FEXCore::Core::InternalThreadState* /*thread*/, uint64_t /*guest_addr*/)
    {
        // We do not back guest code by host file sections, so there is nothing to look up.
        return std::nullopt;
    }

    std::unique_ptr<x86_64_emulator> create_x86_64_emulator()
    {
        return std::make_unique<fex_x86_64_emulator>();
    }

#if SOGEN_FEX_HYPERVISOR
    namespace
    {
        void drop_from_hypervisor(void* address, const size_t size)
        {
            if (g_hypervisor_emulator != nullptr)
            {
                g_hypervisor_emulator->drop_from_vm(reinterpret_cast<uint64_t>(address), size);
            }
        }
    }

    std::unique_ptr<x86_64_emulator> create_hypervisor_x86_64_emulator()
    {
        return std::make_unique<fex_x86_64_emulator>(true);
    }
#endif
} // namespace sogen::fex
