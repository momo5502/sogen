#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>

#include <arch_emulator.hpp>

namespace sogen::kvm::detail
{
    constexpr uint64_t page_size = 0x1000;
    constexpr uint64_t page_table_entry_present = 1ull << 0;
    constexpr uint64_t page_table_entry_writable = 1ull << 1;
    constexpr uint64_t page_table_entry_user = 1ull << 2;
    constexpr uint64_t page_table_entry_address_mask = 0x000FFFFFFFFFF000ull;
    constexpr uint64_t internal_page_table_base = 0x0000007000000000ull;

    inline uint64_t align_down_to_page(const uint64_t value)
    {
        return value & ~(page_size - 1);
    }

    inline bool is_page_aligned(const uint64_t value)
    {
        return (value % page_size) == 0;
    }

    struct mapped_page
    {
        void* host_page = nullptr;
        memory_permission permissions = memory_permission::none;
        bool user_accessible = true;
        std::optional<uint64_t> physical_page{};
        std::shared_ptr<uint8_t> owned_page{};
    };

    struct gp_register_access
    {
        size_t offset = 0;
        size_t width = sizeof(uint64_t);
        bool zero_extend_32 = false;
    };

    inline std::optional<gp_register_access> classify_gp_register_access(const x86_register reg)
    {
        switch (reg)
        {
        case x86_register::al:
        case x86_register::bl:
        case x86_register::cl:
        case x86_register::dl:
        case x86_register::sil:
        case x86_register::dil:
        case x86_register::bpl:
        case x86_register::spl:
        case x86_register::r8b:
        case x86_register::r9b:
        case x86_register::r10b:
        case x86_register::r11b:
        case x86_register::r12b:
        case x86_register::r13b:
        case x86_register::r14b:
        case x86_register::r15b:
            return gp_register_access{.offset = 0, .width = sizeof(uint8_t)};
        case x86_register::ah:
        case x86_register::bh:
        case x86_register::ch:
        case x86_register::dh:
            return gp_register_access{.offset = 1, .width = sizeof(uint8_t)};
        case x86_register::ax:
        case x86_register::bx:
        case x86_register::cx:
        case x86_register::dx:
        case x86_register::si:
        case x86_register::di:
        case x86_register::bp:
        case x86_register::sp:
        case x86_register::r8w:
        case x86_register::r9w:
        case x86_register::r10w:
        case x86_register::r11w:
        case x86_register::r12w:
        case x86_register::r13w:
        case x86_register::r14w:
        case x86_register::r15w:
        case x86_register::ip:
        case x86_register::flags:
            return gp_register_access{.offset = 0, .width = sizeof(uint16_t)};
        case x86_register::eax:
        case x86_register::ebx:
        case x86_register::ecx:
        case x86_register::edx:
        case x86_register::esi:
        case x86_register::edi:
        case x86_register::ebp:
        case x86_register::esp:
        case x86_register::r8d:
        case x86_register::r9d:
        case x86_register::r10d:
        case x86_register::r11d:
        case x86_register::r12d:
        case x86_register::r13d:
        case x86_register::r14d:
        case x86_register::r15d:
        case x86_register::eip:
        case x86_register::eflags:
            return gp_register_access{.offset = 0, .width = sizeof(uint32_t), .zero_extend_32 = true};
        case x86_register::rax:
        case x86_register::rbx:
        case x86_register::rcx:
        case x86_register::rdx:
        case x86_register::rsi:
        case x86_register::rdi:
        case x86_register::rbp:
        case x86_register::rsp:
        case x86_register::r8:
        case x86_register::r9:
        case x86_register::r10:
        case x86_register::r11:
        case x86_register::r12:
        case x86_register::r13:
        case x86_register::r14:
        case x86_register::r15:
        case x86_register::rip:
        case x86_register::rflags:
            return gp_register_access{.offset = 0, .width = sizeof(uint64_t)};
        default:
            return std::nullopt;
        }
    }

    struct instruction_hook_entry
    {
        x86_hookable_instructions type = x86_hookable_instructions::invalid;
        instruction_hook_callback callback{};
    };

    struct execution_hook_entry
    {
        std::optional<uint64_t> address{};
        uint64_t size{};
        memory_execution_hook_callback callback{};
    };

    struct memory_access_hook_entry
    {
        uint64_t address{};
        uint64_t size{};
        memory_access_hook_callback callback{};
    };

    struct mmio_region
    {
        uint64_t address{};
        size_t size{};
        mmio_read_callback read_cb{};
        mmio_write_callback write_cb{};
    };

    template <typename MmioMap>
    inline mmio_region* find_mmio_region(MmioMap& mmio_regions, const uint64_t address)
    {
        for (auto& [base, region] : mmio_regions)
        {
            if (address >= base && address < base + region.size)
            {
                return &region;
            }
        }

        return nullptr;
    }

    template <typename PageTableViews>
    inline uint64_t* get_page_table_entries(PageTableViews& page_table_views, const uint64_t page_gpa)
    {
        return page_table_views.at(page_gpa);
    }

    template <typename PageTableViews, typename AllocateInternalPageFn>
    inline uint64_t ensure_child_table(PageTableViews& page_table_views, AllocateInternalPageFn&& allocate_internal_page,
                                       const uint64_t table_gpa, const size_t index)
    {
        auto* const table_entries = get_page_table_entries(page_table_views, table_gpa);
        auto& entry = table_entries[index];

        if ((entry & page_table_entry_present) == 0)
        {
            const auto child_gpa = allocate_internal_page(false, false);
            entry = child_gpa | page_table_entry_present | page_table_entry_writable | page_table_entry_user;
            return child_gpa;
        }

        return entry & page_table_entry_address_mask;
    }

    template <typename PageTableViews, typename AllocateInternalPageFn>
    inline void ensure_virtual_mapping(PageTableViews& page_table_views, const uint64_t pml4_gpa,
                                       AllocateInternalPageFn&& allocate_internal_page, const uint64_t guest_address,
                                       const uint64_t physical_page_base, const bool user_accessible = true)
    {
        const auto page_base = align_down_to_page(guest_address);
        const auto pml4_index = static_cast<size_t>((page_base >> 39) & 0x1FF);
        const auto pdpt_index = static_cast<size_t>((page_base >> 30) & 0x1FF);
        const auto pd_index = static_cast<size_t>((page_base >> 21) & 0x1FF);
        const auto pt_index = static_cast<size_t>((page_base >> 12) & 0x1FF);

        const auto pdpt_gpa = ensure_child_table(page_table_views, allocate_internal_page, pml4_gpa, pml4_index);
        const auto pd_gpa = ensure_child_table(page_table_views, allocate_internal_page, pdpt_gpa, pdpt_index);
        const auto pt_gpa = ensure_child_table(page_table_views, allocate_internal_page, pd_gpa, pd_index);

        auto* const pt_entries = get_page_table_entries(page_table_views, pt_gpa);
        auto entry = physical_page_base | page_table_entry_present | page_table_entry_writable;
        if (user_accessible)
        {
            entry |= page_table_entry_user;
        }
        pt_entries[pt_index] = entry;
    }

    template <typename PageMap>
    inline bool access_memory(const PageMap& mapped_pages, const uint64_t address, void* data, size_t size, const bool is_write)
    {
        auto current_address = address;
        auto* cursor = static_cast<std::byte*>(data);

        while (size > 0)
        {
            const auto page_base = align_down_to_page(current_address);
            const auto entry = mapped_pages.find(page_base);
            if (entry == mapped_pages.end() || !entry->second || entry->second->host_page == nullptr)
            {
                return false;
            }

            const auto offset = static_cast<size_t>(current_address - page_base);
            const auto chunk = (std::min)(size, static_cast<size_t>(page_size - offset));
            auto* page_ptr = static_cast<std::byte*>(entry->second->host_page) + offset;

            if (is_write)
            {
                std::memcpy(page_ptr, cursor, chunk);
            }
            else
            {
                std::memcpy(cursor, page_ptr, chunk);
            }

            current_address += chunk;
            cursor += chunk;
            size -= chunk;
        }

        return true;
    }
} // namespace sogen::kvm::detail
