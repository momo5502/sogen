#pragma once

#include <map>
#include <platform/platform.hpp>

#include "../module/mapped_module.hpp"

#include <cctype>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>

namespace sogen
{
    namespace detail
    {
        // Zw* aliases share syscall IDs with their Nt counterparts, and find_syscalls rejects a duplicate ID.
        inline bool is_syscall_name(const std::string_view name)
        {
            return name.starts_with("Nt") && name.size() > 3 &&
                   std::toupper(static_cast<unsigned char>(name[2])) == static_cast<unsigned char>(name[2]);
        }
    }

    struct arch_syscall_abi
    {
        virtual ~arch_syscall_abi() = default;

        virtual const char* name() const = 0;

        virtual uint32_t syscall_instruction_size() const = 0;

        virtual std::optional<uint32_t> scan_syscall_id(const exported_symbol& symbol, std::span<const std::byte> image) const = 0;
    };

    // Stub shape:  4C 8B D1            mov r10, rcx
    //              B8 <id:u32>         mov eax, <id>
    //              ...                 (test/jnz/syscall tail, not needed to read the id)
    struct x86_64_syscall_abi : arch_syscall_abi
    {
        const char* name() const override
        {
            return "x86_64";
        }

        uint32_t syscall_instruction_size() const override
        {
            return 2;
        }

        std::optional<uint32_t> scan_syscall_id(const exported_symbol& symbol, std::span<const std::byte> image) const override
        {
            if (!detail::is_syscall_name(symbol.name))
            {
                return std::nullopt;
            }

            constexpr uint64_t operand_offset = 3; // skip `mov r10, rcx`
            constexpr uint64_t operand_size = 4;
            constexpr auto opcode = static_cast<std::byte>(0xB8); // mov eax, imm32

            const auto opcode_rva = symbol.rva + operand_offset;
            if (image.size() < opcode_rva + 1 + operand_size || image[static_cast<size_t>(opcode_rva)] != opcode)
            {
                return std::nullopt;
            }

            uint32_t id{};
            std::memcpy(&id, image.data() + opcode_rva + 1, sizeof(id));
            return id;
        }
    };

    // Native ARM64 stubs carry the syscall ID in SVC's immediate, followed by RET X30.
    struct arm64_syscall_abi : arch_syscall_abi
    {
        const char* name() const override
        {
            return "arm64";
        }

        uint32_t syscall_instruction_size() const override
        {
            return 4;
        }

        std::optional<uint32_t> scan_syscall_id(const exported_symbol& symbol, std::span<const std::byte> image) const override
        {
            if (!detail::is_syscall_name(symbol.name))
            {
                return std::nullopt;
            }

            constexpr uint64_t stub_size = 8; // svc + ret
            if (image.size() < symbol.rva + stub_size)
            {
                return std::nullopt;
            }

            uint32_t svc{};
            uint32_t ret{};
            std::memcpy(&svc, image.data() + symbol.rva, sizeof(svc));
            std::memcpy(&ret, image.data() + symbol.rva + 4, sizeof(ret));

            constexpr uint32_t svc_mask = 0xFFE0001Fu; // SVC #imm16, other fields fixed
            constexpr uint32_t svc_base = 0xD4000001u;
            constexpr uint32_t ret_lr = 0xD65F03C0u; // RET (x30)

            if ((svc & svc_mask) != svc_base || ret != ret_lr)
            {
                return std::nullopt;
            }

            return (svc >> 5) & 0xFFFFu;
        }
    };

    inline const arch_syscall_abi& x86_64_syscall_abi_instance()
    {
        static const x86_64_syscall_abi abi{};
        return abi;
    }

    inline const arch_syscall_abi& arm64_syscall_abi_instance()
    {
        static const arm64_syscall_abi abi{};
        return abi;
    }

    inline const arch_syscall_abi& syscall_abi_for(const PEMachineType machine)
    {
        switch (machine)
        {
        case PEMachineType::ARM64:
            return arm64_syscall_abi_instance();
        default:
            return x86_64_syscall_abi_instance();
        }
    }

} // namespace sogen
