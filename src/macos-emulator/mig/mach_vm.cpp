#include "macos_mig.hpp"
#include "../macos_emulator.hpp"
#include "../macos_memory_protection.hpp"
#include "../syscalls/macos_syscalls.hpp"

#include <address_utils.hpp>

#include <array>
#include <cstring>

namespace sogen
{
    // libsyscall falls back to this MIG routine instead of the trap whenever the caller passes a memory object, a
    // maximum protection other than VM_PROT_ALL, or a non-default inheritance. The request is complex: the memory
    // object travels in a port descriptor ahead of the NDR record, and the remaining fields are packed to 4 bytes.
    std::vector<std::byte> mig_mach_vm_map(const macos_syscall_context& c, const std::span<const std::byte> request)
    {
        constexpr size_t object_offset = sizeof(mach_msg_header) + sizeof(uint32_t);
        constexpr size_t descriptor_size = 12;
        constexpr size_t address_offset = object_offset + descriptor_size + 8;
        constexpr size_t size_offset = address_offset + sizeof(uint64_t);
        constexpr size_t mask_offset = size_offset + sizeof(uint64_t);
        constexpr size_t flags_offset = mask_offset + sizeof(uint64_t);
        constexpr size_t cur_protection_offset = flags_offset + sizeof(int32_t) + sizeof(uint64_t) + sizeof(uint32_t);

        const auto object = read_mig_field<uint32_t>(request, object_offset);
        if (object != 0)
        {
            c.emu_ref.log.error("Unhandled mach_vm_map of memory object 0x%x\n", object);
            return {};
        }

        const auto size = page_align_up(read_mig_field<uint64_t>(request, size_offset));
        if (size == 0)
        {
            return make_mig_return_code_reply(request, macos_kern_return::KERN_INVALID_ARGUMENT);
        }

        const auto address = allocate_virtual_memory(
            c.emu_ref, read_mig_field<uint64_t>(request, address_offset), size, read_mig_field<uint64_t>(request, mask_offset),
            static_cast<uint32_t>(read_mig_field<int32_t>(request, flags_offset)),
            to_memory_permission(static_cast<uint32_t>(read_mig_field<int32_t>(request, cur_protection_offset))), true);
        if (!address)
        {
            return make_mig_return_code_reply(request, macos_kern_return::KERN_NO_SPACE);
        }

        const auto return_code = static_cast<int32_t>(macos_kern_return::KERN_SUCCESS);
        std::array<std::byte, sizeof(return_code) + sizeof(*address)> body{};
        std::memcpy(body.data(), &return_code, sizeof(return_code));
        std::memcpy(body.data() + sizeof(return_code), &*address, sizeof(*address));
        return make_mig_reply(request, body);
    }
}
