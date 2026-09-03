#include "std_include.hpp"
#include "macos_memory_manager.hpp"

#include <utils/finally.hpp>

#include <cstdlib>
#include <cstring>

#include <utils/string.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <stdexcept>

namespace sogen
{
    namespace
    {
        struct page_range
        {
            uint64_t address{};
            size_t length{};
        };

        // The commpage sits above MACOS_MAX_MMAP_END_EXCL, so allocate_reserved_region needs a ceiling
        // of its own: the mmap ceiling is where the guest's own allocator stops, not where the guest
        // address space stops.
        constexpr uint64_t ADDRESS_SPACE_END_EXCL = MACOS_COMMPAGE_NESTING_START + MACOS_COMMPAGE_NESTING_SIZE;

        // Bounds the staging buffer map_file allocates: the mapping length comes straight from the
        // guest's mmap arguments and would otherwise size a host allocation.
        constexpr uint64_t MACOS_FILE_MAP_CHUNK_SIZE = 0x100000;

        static_assert(MACOS_MAX_MMAP_END_EXCL % MACOS_PAGE_SIZE == 0,
                      "align_to_pages relies on the ceiling being page aligned to keep its rounded end in bounds");
        static_assert(ADDRESS_SPACE_END_EXCL % MACOS_PAGE_SIZE == 0,
                      "align_to_pages relies on the ceiling being page aligned to keep its rounded end in bounds");

        // Walks an ordered interval set once, moving the candidate past anything it collides with.
        template <typename Intervals, typename LengthOf>
        bool advance_past(const Intervals& intervals, LengthOf length_of, uint64_t& candidate, const uint64_t size)
        {
            auto moved = false;

            for (const auto& entry : intervals)
            {
                const auto entry_end = entry.first + length_of(entry);
                if (entry_end <= candidate)
                {
                    continue;
                }

                if (entry.first >= candidate + size)
                {
                    break;
                }

                candidate = entry_end;
                moved = true;
            }

            return moved;
        }

        std::optional<page_range> align_to_pages(const uint64_t address, const size_t size,
                                                 const uint64_t end_excl = MACOS_MAX_MMAP_END_EXCL)
        {
            if (size == 0 || address >= end_excl || size > end_excl - address)
            {
                return std::nullopt;
            }

            const auto start = page_align_down(address, MACOS_PAGE_SIZE);
            const auto end = page_align_up(address + size, MACOS_PAGE_SIZE);

            return page_range{.address = start, .length = static_cast<size_t>(end - start)};
        }

        macos_memory_region_info make_region_info(const uint64_t start, const macos_memory_region& region)
        {
            macos_memory_region_info info{};
            info.start = start;
            info.length = region.length;
            info.permissions = region.permissions;
            info.allocation_base = start;
            info.allocation_length = region.length;
            info.initial_permissions = region.permissions;
            info.is_file_backed = region.file_backed;
            info.backing_path = region.backing_path;
            return info;
        }
    }

    void macos_memory_region::serialize(utils::buffer_serializer& buffer) const
    {
        buffer.write<uint64_t>(this->length);
        buffer.write(this->permissions);
        buffer.write(this->max_permissions);
        buffer.write(this->backed);
        buffer.write(this->file_backed);
        buffer.write_string(this->backing_path);
    }

    void macos_memory_region::deserialize(utils::buffer_deserializer& buffer)
    {
        this->length = static_cast<size_t>(buffer.read<uint64_t>());
        buffer.read(this->permissions);
        buffer.read(this->max_permissions);
        buffer.read(this->backed);
        buffer.read(this->file_backed);
        buffer.read_string(this->backing_path);
    }

    void macos_memory_manager::set_demand_pager(macos_demand_pager pager)
    {
        this->demand_pager_ = std::move(pager);
    }

    bool macos_memory_manager::materialize(const uint64_t address, const size_t size) const
    {
        if (!this->demand_pager_ || this->demand_paging_ || size == 0)
        {
            return false;
        }

        this->demand_paging_ = true;
        const auto reset = utils::finally([this] { this->demand_paging_ = false; });

        return this->demand_pager_(address, size);
    }

    void macos_memory_manager::read_memory(const uint64_t address, void* data, const size_t size) const
    {
        if (this->try_read_memory(address, data, size))
        {
            return;
        }

        // Left to the backend so that a read which really cannot be served still fails the way every
        // caller of this overload is written against.
        this->memory_->read_memory(address, data, size);
    }

    bool macos_memory_manager::try_read_memory(const uint64_t address, void* data, const size_t size) const
    {
        if (this->memory_->try_read_memory(address, data, size))
        {
            return true;
        }

        return this->materialize(address, size) && this->memory_->try_read_memory(address, data, size);
    }

    void macos_memory_manager::write_memory(const uint64_t address, const void* data, const size_t size)
    {
        this->memory_->write_memory(address, data, size);
    }

    bool macos_memory_manager::try_write_memory(const uint64_t address, const void* data, const size_t size)
    {
        return this->memory_->try_write_memory(address, data, size);
    }

    void macos_memory_manager::map_mmio(const uint64_t address, const size_t size, mmio_read_callback read_cb, mmio_write_callback write_cb)
    {
        this->memory_->map_mmio(address, size, std::move(read_cb), std::move(write_cb));
    }

    void macos_memory_manager::map_memory(const uint64_t address, const size_t size, const memory_permission permissions)
    {
        this->memory_->map_memory(address, size, permissions);
    }

    void macos_memory_manager::unmap_memory(const uint64_t address, const size_t size)
    {
        this->memory_->unmap_memory(address, size);
    }

    void macos_memory_manager::apply_memory_protection(const uint64_t address, const size_t size, const memory_permission permissions)
    {
        this->memory_->apply_memory_protection(address, size, permissions);
    }

    bool macos_memory_manager::overlaps_mapped_region(const uint64_t address, const size_t size) const
    {
        for (const auto& [start, region] : this->mapped_regions_)
        {
            if (regions_with_length_intersect(address, size, start, region.length))
            {
                return true;
            }
        }

        return false;
    }

    bool macos_memory_manager::is_reserved_range(const uint64_t address, const size_t size)
    {
        if (size == 0)
        {
            return false;
        }

        if (address < MACOS_PAGEZERO_END)
        {
            return true;
        }

        if (address >= MACOS_MAX_MMAP_END_EXCL || size > MACOS_MAX_MMAP_END_EXCL - address)
        {
            return true;
        }

        return regions_with_length_intersect(address, size, MACOS_COMMPAGE_NESTING_START, MACOS_COMMPAGE_NESTING_SIZE);
    }

    bool macos_memory_manager::claim_region(const uint64_t address, const size_t size, const memory_permission permissions,
                                            const uint64_t end_excl)
    {
        const auto range = align_to_pages(address, size, end_excl);
        if (!range || this->overlaps_mapped_region(range->address, range->length))
        {
            return false;
        }

        // VM_PROT_NONE claims address space without charging for it: XNU gives the entry no resident
        // pages and no swap reservation, and the guest has to protect a slice before it can touch it.
        // libmalloc reserves its magazine ranges this way, tens of gigabytes of them, so backing them
        // here would be invisible on a host that pages lazily and fatal anywhere memory has to be real.
        if (permissions == memory_permission::none)
        {
            this->mapped_regions_[range->address] =
                macos_memory_region{.length = range->length, .permissions = permissions, .backed = false};
            return true;
        }

        this->map_memory(range->address, range->length, permissions);
        this->mapped_regions_[range->address] = macos_memory_region{.length = range->length, .permissions = permissions, .backed = true};

        return true;
    }

    bool macos_memory_manager::allocate_memory(const uint64_t address, const size_t size, const memory_permission permissions)
    {
        if (is_reserved_range(address, size))
        {
            return false;
        }

        return this->claim_region(address, size, permissions, MACOS_MAX_MMAP_END_EXCL);
    }

    bool macos_memory_manager::allocate_reserved_region(const uint64_t address, const size_t size, const memory_permission permissions)
    {
        return this->claim_region(address, size, permissions, ADDRESS_SPACE_END_EXCL);
    }

    uint64_t macos_memory_manager::allocate_memory(const size_t size, const memory_permission permissions, const uint64_t start)
    {
        const auto base = this->find_free_allocation_base(size, start, start == 0);
        if (!base || !this->allocate_memory(base, size, permissions))
        {
            return 0;
        }

        if (start != 0)
        {
            this->claim_for_emulator(base, static_cast<size_t>(page_align_up(size, MACOS_PAGE_SIZE)));
        }

        return base;
    }

    // Merges into whatever it touches, so an arena the emulator allocates and releases over and over
    // costs one entry rather than one per cycle.
    void macos_memory_manager::claim_for_emulator(const uint64_t address, const size_t length)
    {
        auto start = address;
        auto end = address + length;

        auto first = this->emulator_ranges_.upper_bound(start);
        if (first != this->emulator_ranges_.begin())
        {
            const auto previous = std::prev(first);
            if (previous->first + previous->second >= start)
            {
                first = previous;
            }
        }

        auto last = first;
        while (last != this->emulator_ranges_.end() && last->first <= end)
        {
            start = std::min(start, last->first);
            end = std::max(end, last->first + last->second);
            ++last;
        }

        this->emulator_ranges_.erase(first, last);
        this->emulator_ranges_[start] = static_cast<size_t>(end - start);
    }

    bool macos_memory_manager::reserve_memory(const uint64_t address, const size_t size)
    {
        const auto range = align_to_pages(address, size);
        if (!range || this->overlaps_mapped_region(range->address, range->length))
        {
            return false;
        }

        this->mapped_regions_[range->address] = macos_memory_region{
            .length = range->length, .permissions = memory_permission::none, .max_permissions = memory_permission::none, .backed = false};

        return true;
    }

    // A reservation becomes real memory the first time the guest protects it to something usable, which
    // is how libmalloc turns a slice of its magazine range into a heap. Going the other way keeps the
    // backing: VM_PROT_NONE on a live entry does not discard its pages, and dropping them here would
    // lose data the guest expects to find again after it protects the range back.
    void macos_memory_manager::apply_permissions_to_region(const uint64_t base, macos_memory_region& region,
                                                           const memory_permission permissions)
    {
        if (!region.backed && permissions != memory_permission::none)
        {
            this->map_memory(base, region.length, permissions);
            region.backed = true;
        }
        else if (region.backed)
        {
            this->apply_memory_protection(base, region.length, permissions);
        }

        region.permissions = permissions;
    }

    bool macos_memory_manager::protect_memory(const uint64_t address, const size_t size, const memory_permission permissions)
    {
        const auto range = align_to_pages(address, size);
        if (!range)
        {
            return false;
        }

        const auto prot_start = range->address;
        const auto prot_end = prot_start + range->length;

        auto covered_until = prot_start;
        while (covered_until < prot_end)
        {
            const auto upper_bound = this->mapped_regions_.upper_bound(covered_until);
            if (upper_bound == this->mapped_regions_.begin())
            {
                return false;
            }

            const auto entry = std::prev(upper_bound);
            const auto entry_end = entry->first + entry->second.length;
            if (entry_end <= covered_until)
            {
                return false;
            }

            // XNU refuses the whole call when any entry in the range would exceed its max_protection,
            // rather than protecting the part it can, so the range is checked before anything is edited.
            const auto excess = static_cast<uint8_t>(permissions) & ~static_cast<uint8_t>(entry->second.max_permissions);
            if (excess != 0)
            {
                return false;
            }

            covered_until = std::min(entry_end, prot_end);
        }

        std::vector<std::pair<uint64_t, macos_memory_region>> replacements{};
        std::vector<uint64_t> removals{};

        for (auto& [base, region] : this->mapped_regions_)
        {
            const auto region_end = base + region.length;
            if (base >= prot_end || region_end <= prot_start)
            {
                continue;
            }

            if (base >= prot_start && region_end <= prot_end)
            {
                this->apply_permissions_to_region(base, region, permissions);
                continue;
            }

            removals.push_back(base);

            if (base < prot_start)
            {
                auto prefix = region;
                prefix.length = static_cast<size_t>(prot_start - base);
                replacements.emplace_back(base, std::move(prefix));
            }

            const auto overlap_start = std::max(base, prot_start);
            const auto overlap_end = std::min(region_end, prot_end);

            auto middle = region;
            middle.length = static_cast<size_t>(overlap_end - overlap_start);
            this->apply_permissions_to_region(overlap_start, middle, permissions);
            replacements.emplace_back(overlap_start, std::move(middle));

            if (region_end > prot_end)
            {
                auto suffix = region;
                suffix.length = static_cast<size_t>(region_end - prot_end);
                replacements.emplace_back(prot_end, std::move(suffix));
            }
        }

        this->apply_region_edits(removals, std::move(replacements));

        return true;
    }

    bool macos_memory_manager::zero_memory(const uint64_t address, const size_t size)
    {
        const auto range = align_to_pages(address, size);
        if (!range)
        {
            return false;
        }

        const auto end = range->address + range->length;

        auto covered_until = range->address;
        while (covered_until < end)
        {
            const auto upper_bound = this->mapped_regions_.upper_bound(covered_until);
            if (upper_bound == this->mapped_regions_.begin())
            {
                return false;
            }

            const auto& [base, region] = *std::prev(upper_bound);
            const auto region_end = base + region.length;

            if (region_end <= covered_until || !region.backed || (region.permissions & memory_permission::write) == memory_permission::none)
            {
                return false;
            }

            covered_until = std::min(region_end, end);
        }

        const std::array<uint8_t, MACOS_PAGE_SIZE> zeros{};

        for (auto cursor = range->address; cursor < end; cursor += MACOS_PAGE_SIZE)
        {
            if (!this->try_write_memory(cursor, zeros.data(), zeros.size()))
            {
                return false;
            }
        }

        return true;
    }

    bool macos_memory_manager::release_memory(const uint64_t address, const size_t size)
    {
        const auto range = align_to_pages(address, size);
        if (!range)
        {
            return false;
        }

        const auto release_start = range->address;
        const auto release_end = release_start + range->length;

        std::vector<std::pair<uint64_t, macos_memory_region>> replacements{};
        std::vector<uint64_t> removals{};

        for (const auto& [base, region] : this->mapped_regions_)
        {
            const auto region_end = base + region.length;
            if (base >= release_end || region_end <= release_start)
            {
                continue;
            }

            removals.push_back(base);

            if (base < release_start)
            {
                auto prefix = region;
                prefix.length = static_cast<size_t>(release_start - base);
                replacements.emplace_back(base, std::move(prefix));
            }

            if (region_end > release_end)
            {
                auto suffix = region;
                suffix.length = static_cast<size_t>(region_end - release_end);
                replacements.emplace_back(release_end, std::move(suffix));
            }

            if (region.backed)
            {
                const auto unmap_start = std::max(base, release_start);
                const auto unmap_end = std::min(region_end, release_end);
                this->unmap_memory(unmap_start, static_cast<size_t>(unmap_end - unmap_start));
            }
        }

        if (removals.empty())
        {
            return false;
        }

        this->apply_region_edits(removals, std::move(replacements));

        return true;
    }

    void macos_memory_manager::apply_region_edits(const std::vector<uint64_t>& removals,
                                                  std::vector<std::pair<uint64_t, macos_memory_region>> replacements)
    {
        for (const auto base : removals)
        {
            this->mapped_regions_.erase(base);
        }

        for (auto& [base, region] : replacements)
        {
            this->mapped_regions_[base] = std::move(region);
        }
    }

    bool macos_memory_manager::map_host_file_range(const uint64_t address, const size_t size, const std::filesystem::path& host_path,
                                                   const uint64_t file_offset, const memory_permission permissions)
    {
        auto mapping = host_file_mapping::create(host_path, file_offset, size);
        if (!mapping)
        {
            return false;
        }

        if (!this->map_host_file_memory(address, size, mapping->data(), permissions))
        {
            return false;
        }

        this->host_mappings_.emplace_back(address, std::move(*mapping));
        return true;
    }

    void macos_memory_manager::unmap_all_memory()
    {
        for (const auto& [base, region] : this->mapped_regions_)
        {
            if (region.backed)
            {
                this->unmap_memory(base, region.length);
            }
        }

        this->mapped_regions_.clear();
        this->host_mappings_.clear();
    }

    // The guest range aliases host_pointer directly - unicorn flags a uc_mem_map_ptr block RAM_PREALLOC
    // and never frees it, so the caller keeps ownership of the host mapping and its copy-on-write
    // semantics are whatever the host mmap gave it.
    bool macos_memory_manager::map_host_file_memory(const uint64_t address, const size_t size, void* host_pointer,
                                                    const memory_permission permissions)
    {
        if (!host_pointer || (address % MACOS_PAGE_SIZE) != 0 || (size % MACOS_PAGE_SIZE) != 0 || is_reserved_range(address, size))
        {
            return false;
        }

        const auto range = align_to_pages(address, size);
        if (!range || this->overlaps_mapped_region(range->address, range->length))
        {
            return false;
        }

        this->memory_->map_host_memory(range->address, range->length, host_pointer, permissions);
        this->mapped_regions_[range->address] =
            macos_memory_region{.length = range->length, .permissions = permissions, .backed = true, .file_backed = true};

        return true;
    }

    void* macos_memory_manager::acquire_shared_backing(const size_t size)
    {
        const auto rounded = static_cast<size_t>(page_align_up(size, MACOS_PAGE_SIZE));
        if (rounded == 0)
        {
            return nullptr;
        }

        auto* buffer = std::aligned_alloc(MACOS_PAGE_SIZE, rounded);
        if (buffer == nullptr)
        {
            return nullptr;
        }

        std::memset(buffer, 0, rounded);
        this->shared_backings_.emplace_back(buffer, &std::free);
        return buffer;
    }

    bool macos_memory_manager::map_file(const uint64_t address, const size_t size, const memory_permission permissions,
                                        const std::filesystem::path& host_path, const uint64_t file_offset)
    {
        const auto aligned_size = static_cast<size_t>(page_align_up(size, MACOS_PAGE_SIZE));
        if (size == 0 || aligned_size < size)
        {
            return false;
        }

        // Claimed writable and protected afterwards, the way macho_mapping.cpp stages a segment: a
        // read-only or executable file mapping still has to receive the file contents first.
        if (!this->allocate_memory(address, aligned_size, memory_permission::read_write))
        {
            return false;
        }

        const auto start = page_align_down(address, MACOS_PAGE_SIZE);

        std::ifstream file{host_path, std::ios::binary};
        if (file)
        {
            file.seekg(static_cast<std::streamoff>(file_offset));

            std::vector<uint8_t> chunk(static_cast<size_t>(std::min<uint64_t>(size, MACOS_FILE_MAP_CHUNK_SIZE)));
            uint64_t copied = 0;

            // A short read leaves the rest of the range at the zero the fresh allocation already holds,
            // which is what mapping past the end of a file gives on Darwin.
            while (copied < size && file)
            {
                const auto count = static_cast<size_t>(std::min<uint64_t>(chunk.size(), size - copied));
                file.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(count));

                const auto received = static_cast<size_t>(file.gcount());
                if (received == 0)
                {
                    break;
                }

                this->write_memory(address + copied, chunk.data(), received);
                copied += received;
            }
        }

        if (permissions != memory_permission::read_write && !this->protect_memory(start, aligned_size, permissions))
        {
            this->release_memory(start, aligned_size);
            return false;
        }

        const auto entry = this->mapped_regions_.find(start);
        if (entry != this->mapped_regions_.end())
        {
            entry->second.file_backed = true;
            entry->second.backing_path = host_path.string();
        }

        return true;
    }

    uint64_t macos_memory_manager::find_free_allocation_base(const size_t size, const uint64_t start,
                                                             const bool avoid_emulator_ranges) const
    {
        const auto aligned_size = page_align_up(size, MACOS_PAGE_SIZE);
        if (size == 0 || aligned_size < size || aligned_size > MACOS_MAX_MMAP_END_EXCL)
        {
            return 0;
        }

        // A caller-supplied start is mmap's hint and is honoured wherever it points. An unhinted search
        // instead floors at the shared cache end, so that a mmap_base_ written below it - by
        // set_mmap_base, or by a snapshot fed to deserialize_memory_state - cannot walk the search back
        // into the span Stage 5 maps the cache into.
        const auto floor = start ? std::max(start, MACOS_PAGEZERO_END) : std::max(this->mmap_base_, MACOS_SHARED_CACHE_END);

        auto candidate = page_align_up(floor, MACOS_PAGE_SIZE);

        // Two ordered interval sets, and moving past one can put the candidate back inside the other,
        // so the walk repeats until neither moves it.
        for (auto moved = true; moved;)
        {
            moved = advance_past(this->mapped_regions_, [](const auto& entry) { return entry.second.length; }, candidate, aligned_size);

            // The guest must never be handed an address the emulator has taken for itself, because sogen
            // releases those on its own schedule and is never told whether the guest still holds the
            // pointer -- a range recycled from one into the guest's heap is a use-after-free the guest
            // pays for. The emulator goes on reusing them itself, which is what bounds the set.
            if (avoid_emulator_ranges &&
                advance_past(this->emulator_ranges_, [](const auto& entry) { return entry.second; }, candidate, aligned_size))
            {
                moved = true;
            }
        }

        if (candidate > MACOS_MAX_MMAP_END_EXCL - aligned_size)
        {
            return 0;
        }

        return candidate;
    }

    std::vector<macos_memory_region_info> macos_memory_manager::get_mapped_region_infos() const
    {
        std::vector<macos_memory_region_info> infos{};
        infos.reserve(this->mapped_regions_.size());

        for (const auto& [start, region] : this->mapped_regions_)
        {
            infos.push_back(make_region_info(start, region));
        }

        return infos;
    }

    std::optional<macos_memory_region_info> macos_memory_manager::get_region_info(const uint64_t address) const
    {
        const auto upper_bound = this->mapped_regions_.upper_bound(address);
        if (upper_bound == this->mapped_regions_.begin())
        {
            return std::nullopt;
        }

        const auto entry = std::prev(upper_bound);
        if (address - entry->first >= entry->second.length)
        {
            return std::nullopt;
        }

        return make_region_info(entry->first, entry->second);
    }

    // Mirrors validate_restored_regions: a backed region inside a reserved range is emulator-owned (the
    // commpage), gets rebuilt from scratch on restore, and would make every snapshot unrestorable if it
    // were written out here.
    void macos_memory_manager::serialize_memory_state(utils::buffer_serializer& buffer) const
    {
        region_map guest_regions{};

        for (const auto& [start, region] : this->mapped_regions_)
        {
            if (!region.backed || !is_reserved_range(start, region.length))
            {
                guest_regions.emplace(start, region);
            }
        }

        buffer.write(this->mmap_base_);
        buffer.write_map(guest_regions);

        std::vector<uint8_t> data{};

        for (const auto& [start, region] : guest_regions)
        {
            if (!region.backed)
            {
                continue;
            }

            data.resize(region.length);
            this->read_memory(start, data.data(), region.length);
            buffer.write(data.data(), region.length);
        }
    }

    // A snapshot is attacker-influenceable: it is captured while a hostile sample runs, so every region
    // in it is untrusted input and has to clear the invariants a live allocation clears before it can
    // reach map_memory.
    void macos_memory_manager::validate_restored_regions(const region_map& regions)
    {
        uint64_t previous_end = 0;

        for (const auto& [start, region] : regions)
        {
            if (region.backed && is_reserved_range(start, region.length))
            {
                throw std::runtime_error("Snapshot maps a region over a reserved range at 0x" + utils::string::to_hex_number(start));
            }

            const auto range = align_to_pages(start, region.length);
            if (!range || range->address != start || range->length != region.length)
            {
                throw std::runtime_error("Snapshot region at 0x" + utils::string::to_hex_number(start) +
                                         " is not a page aligned range below the mmap ceiling");
            }

            if (start < previous_end)
            {
                throw std::runtime_error("Snapshot region at 0x" + utils::string::to_hex_number(start) + " overlaps the region below it");
            }

            previous_end = start + region.length;
        }
    }

    // Host aliasing is not restored: the host pointer behind a map_host_file_memory region belongs to
    // the caller and is not part of the snapshot, so the contents come back as private guest memory.
    void macos_memory_manager::deserialize_memory_state(utils::buffer_deserializer& buffer)
    {
        const auto new_mmap_base = buffer.read<uint64_t>();
        auto new_regions = buffer.read_map<region_map>();

        // Validated before any region_data is sized, so a forged length cannot force a huge host
        // allocation, and before unmap_all_memory, so a rejected snapshot leaves the live state intact.
        validate_restored_regions(new_regions);

        std::vector<std::vector<uint8_t>> region_data{};
        region_data.reserve(new_regions.size());

        for (const auto& [_, region] : new_regions)
        {
            auto& data = region_data.emplace_back();
            if (!region.backed)
            {
                continue;
            }

            data.resize(region.length);
            buffer.read(data.data(), region.length);
        }

        this->unmap_all_memory();
        this->mmap_base_ = new_mmap_base;

        auto data_entry = region_data.begin();
        for (const auto& [start, region] : new_regions)
        {
            if (region.backed)
            {
                this->map_memory(start, region.length, region.permissions);
                this->write_memory(start, data_entry->data(), region.length);
            }

            ++data_entry;
        }

        this->mapped_regions_ = std::move(new_regions);
    }
}
