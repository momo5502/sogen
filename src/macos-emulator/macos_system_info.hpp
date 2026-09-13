#pragma once

#include <cstdint>
#include <string>

namespace sogen
{
    // Single source of truth for the machine the guest is told it runs on. The commpage and sysctl must
    // agree field for field: a guest that cross-checks hw.memsize against _COMM_PAGE_MEMORY_SIZE notices
    // any divergence.
    struct macos_system_info
    {
        std::string signature{"commpage 64-bit"};

        // The word a real machine publishes, minus the features this backend cannot actually perform.
        // A capability bit here is a promise: libraries branch on it and execute the instruction
        // without checking anything else. libcorecrypto reads the word straight off the commpage --
        //     mov x8, #0xfffff0000 ; movk x8, #0xc010 ; ldr x9, [x8] ; tbnz x9, #57, use_dit
        // -- and on bit 57 executes MRS DIT, which unicorn's arm model does not implement (its own
        // ID_AA64PFR0_EL1 correctly reports the feature absent, but nothing consults it). Bit 57 was
        // read out of that branch, not guessed.
        //
        // Other bits in this word are equally unbacked and simply have not been reached yet; clear each
        // one where the instruction it promises is found, and measure the position the same way.
        static constexpr uint64_t FEAT_DIT = uint64_t{1} << 57;

        uint64_t cpu_capabilities64{0x87FFEF97A70E7FC8ULL & ~FEAT_DIT};
        uint32_t cpu_capabilities32{0xA70E7FC8U};
        uint16_t version{3};
        uint8_t ncpus{14};
        uint8_t active_cpus{14};
        uint8_t physical_cpus{14};
        uint8_t logical_cpus{14};
        uint8_t cpu_clusters{3};
        uint16_t cache_linesize{128};
        uint64_t memory_size{0x200000000ULL};
        uint32_t cpufamily{0x72015832U};
        uint8_t user_page_shift{14};
        uint8_t kernel_page_shift{14};
        std::string machine{"arm64"};
        std::string model{"Mac15,1"};
        std::string os_type{"Darwin"};
        std::string os_release{"25.6.0"};
        std::string os_version{"25G76"};
        std::string os_product_version{"26.6.1"};
        int32_t os_revision{199506};
        bool has_feat_pauth{true};
        bool has_feat_lse{true};
        bool has_feat_fp16{true};
        bool has_feat_bti{false};
    };
}
