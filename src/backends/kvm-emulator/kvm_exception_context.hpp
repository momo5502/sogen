#pragma once

#include <linux/kvm.h>

#include <cstdint>
#include <optional>
#include <utility>

namespace sogen::kvm::detail
{
    inline kvm_segment make_segment(const uint16_t selector, const bool is_code, const bool is_user)
    {
        // User selector 0x33 is 64-bit code; selector 0x23 is compatibility-mode code.
        // Their L/D bits must match before restoring an exception context.
        const bool long_mode_code = is_code && ((selector | 3) == 0x33);

        kvm_segment segment{};
        segment.base = 0;
        // The register API takes the expanded byte limit, even when G is set.
        segment.limit = 0xFFFFFFFF;
        segment.selector = selector;
        segment.type = is_code ? 0xB : 0x3;
        segment.present = 1;
        segment.dpl = is_user ? 3 : 0;
        segment.db = long_mode_code ? 0 : 1;
        segment.s = 1;
        segment.l = long_mode_code ? 1 : 0;
        segment.g = 1;
        segment.avl = 0;
        segment.unusable = 0;
        return segment;
    }

    struct exception_frame
    {
        uint64_t rip;
        uint64_t cs;
        uint64_t rflags;
        uint64_t rsp;
        uint64_t ss;
    };

    struct exception_dispatch_result
    {
        bool handled{};
        std::optional<exception_frame> return_frame{};
    };

    template <typename Callback>
    exception_dispatch_result dispatch_exception_context(kvm_regs& regs, kvm_sregs& sregs, const exception_frame& frame,
                                                         const uint32_t vector, const uint64_t frame_address,
                                                         const uint64_t trampoline_address, Callback&& callback)
    {
        const bool compat_mode = (static_cast<uint16_t>(frame.cs) | 3u) != 0x33;
        const auto entry_cs = sregs.cs;
        const auto entry_ss = sregs.ss;
        regs.rip = frame.rip - (vector == 3 ? 1 : 0);
        regs.rsp = frame.rsp;
        regs.rflags = frame.rflags;
        sregs.cs = make_segment(static_cast<uint16_t>(frame.cs), true, (frame.cs & 3) == 3);
        sregs.ss = make_segment(static_cast<uint16_t>(frame.ss), false, (frame.ss & 3) == 3);
        if (sregs.ds.selector & ~3u)
        {
            sregs.ds = make_segment(sregs.ds.selector, false, (sregs.ds.selector & 3) == 3);
        }
        if (sregs.es.selector & ~3u)
        {
            sregs.es = make_segment(sregs.es.selector, false, (sregs.es.selector & 3) == 3);
        }
        if (!std::forward<Callback>(callback)())
        {
            return {};
        }
        exception_dispatch_result result{.handled = true};
        if (compat_mode || ((sregs.cs.selector | 3u) != 0x33))
        {
            result.return_frame = exception_frame{
                .rip = regs.rip,
                .cs = sregs.cs.selector,
                .rflags = regs.rflags,
                .rsp = regs.rsp,
                .ss = sregs.ss.selector,
            };
            // The callback sees user context; the return trampoline still runs with the privileged descriptors.
            sregs.cs = entry_cs;
            sregs.ss = entry_ss;
            regs.rsp = frame_address;
            regs.rip = trampoline_address;
            // A single step inside the trampoline would overwrite its return frame on the shared exception stack.
            regs.rflags &= ~(1ULL << 8);
        }
        return result;
    }
}
