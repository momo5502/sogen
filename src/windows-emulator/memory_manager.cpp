#include "std_include.hpp"
#include "memory_manager.hpp"

#include "memory_region.hpp"
#include "address_utils.hpp"
#include "memory_permission_ext.hpp"

#include <vector>
#include <optional>
#include <stdexcept>
#include <cassert>

namespace sogen
{

    namespace
    {
        void split_regions(memory_manager::committed_region_map& regions, const std::vector<uint64_t>& split_points)
        {
            for (auto i = regions.begin(); i != regions.end(); ++i)
            {
                for (const auto split_point : split_points)
                {
                    if (is_within_start_and_length(split_point, i->first, i->second.length) && i->first != split_point)
                    {
                        const auto first_length = split_point - i->first;
                        const auto second_length = i->second.length - first_length;

                        i->second.length = static_cast<size_t>(first_length);

                        regions[split_point] = memory_manager::committed_region{
                            .length = static_cast<size_t>(second_length),
                            .permissions = i->second.permissions,
                        };
                    }
                }
            }
        }

        void merge_regions(memory_manager::committed_region_map& regions)
        {
            for (auto i = regions.begin(); i != regions.end();)
            {
                assert(i->second.length > 0);

                auto next = i;
                std::advance(next, 1);

                if (next == regions.end())
                {
                    break;
                }

                assert(next->second.length > 0);

                const auto end = i->first + i->second.length;
                assert(end <= next->first);

                if (end != next->first || i->second.permissions != next->second.permissions)
                {
                    ++i;
                    continue;
                }

                i->second.length += next->second.length;
                regions.erase(next);
            }
        }
    }

    namespace utils
    {
        static void serialize(buffer_serializer& buffer, const memory_manager::committed_region& region)
        {
            buffer.write<uint64_t>(region.length);
            buffer.write(region.permissions);
        }

        static void deserialize(buffer_deserializer& buffer, memory_manager::committed_region& region)
        {
            region.length = static_cast<size_t>(buffer.read<uint64_t>());
            region.permissions = buffer.read<nt_memory_permission>();
        }

        static void serialize(buffer_serializer& buffer, const memory_manager::reserved_region& region)
        {
            buffer.write(region.kind);
            buffer.write(region.mapped_filename);
            buffer.write(region.initial_permission);
            buffer.write<uint64_t>(region.length);
            buffer.write_map(region.committed_regions);
        }

        static void deserialize(buffer_deserializer& buffer, memory_manager::reserved_region& region)
        {
            buffer.read(region.kind);
            buffer.read(region.mapped_filename);
            buffer.read(region.initial_permission);
            region.length = static_cast<size_t>(buffer.read<uint64_t>());
            buffer.read_map(region.committed_regions);
        }
    }

    void memory_manager::update_layout_version()
    {
#if SOGEN_REFLECTION_LEVEL > 0
        this->layout_version_.fetch_add(1, std::memory_order_relaxed);
#endif
    }

    memory_stats memory_manager::compute_memory_stats() const
    {
        memory_stats stats{};
        stats.reserved_memory = 0;
        stats.committed_memory = 0;

        for (const auto& reserved_region : this->reserved_regions_ | std::views::values)
        {
            stats.reserved_memory += reserved_region.length;

            for (const auto& committed_region : reserved_region.committed_regions | std::views::values)
            {
                stats.committed_memory += committed_region.length;
            }
        }

        return stats;
    }

    memory_permission memory_manager::get_effective_permissions(const nt_memory_permission permissions) const
    {
        if (permissions.is_guarded())
        {
            return memory_permission::none;
        }

        auto effective = permissions.common;
        if (!this->dep_enabled_ && effective != memory_permission::none)
        {
            effective |= memory_permission::exec;
        }

        return effective;
    }

    void memory_manager::set_dep_enabled(const bool enabled)
    {
        if (this->dep_enabled_ == enabled)
        {
            return;
        }

        this->dep_enabled_ = enabled;
        for (const auto& reserved : this->reserved_regions_ | std::views::values)
        {
            if (reserved.kind == memory_region_kind::mmio)
            {
                continue;
            }

            for (const auto& [address, region] : reserved.committed_regions)
            {
                this->apply_memory_protection(address, region.length, this->get_effective_permissions(region.permissions));
            }
        }
    }

    void memory_manager::serialize_memory_state(utils::buffer_serializer& buffer, const bool is_snapshot) const
    {
        buffer.write_atomic(this->layout_version_);
        buffer.write(this->default_allocation_address_);
        buffer.write(this->dep_enabled_);
        buffer.write_map(this->reserved_regions_);

        if (is_snapshot)
        {
            return;
        }

        std::vector<uint8_t> data{};

        for (const auto& reserved_region : this->reserved_regions_)
        {
            if (reserved_region.second.kind == memory_region_kind::mmio)
            {
                continue;
            }

            for (const auto& region : reserved_region.second.committed_regions)
            {
                data.resize(region.second.length);

                this->read_memory(region.first, data.data(), region.second.length);

                buffer.write(data.data(), region.second.length);
            }
        }
    }

    void memory_manager::deserialize_memory_state(utils::buffer_deserializer& buffer, const bool is_snapshot)
    {
        if (!is_snapshot)
        {
            assert(this->reserved_regions_.empty());
        }

        buffer.read_atomic(this->layout_version_);
        buffer.read(this->default_allocation_address_);
        buffer.read(this->dep_enabled_);
        buffer.read_map(this->reserved_regions_);

        if (is_snapshot)
        {
            return;
        }

        std::vector<uint8_t> data{};

        for (auto i = this->reserved_regions_.begin(); i != this->reserved_regions_.end();)
        {
            auto& reserved_region = i->second;
            if (reserved_region.kind == memory_region_kind::mmio)
            {
                i = this->reserved_regions_.erase(i);
                continue;
            }

            ++i;

            for (const auto& region : reserved_region.committed_regions)
            {
                data.resize(region.second.length);

                buffer.read(data.data(), region.second.length);

                const auto effective_permission = this->get_effective_permissions(region.second.permissions);
                this->map_memory(region.first, region.second.length, effective_permission);
                this->write_memory(region.first, data.data(), region.second.length);
            }
        }
    }

    bool memory_manager::protect_memory(const uint64_t address, const size_t size, const nt_memory_permission permissions,
                                        nt_memory_permission* old_permissions)
    {
        const auto entry = this->find_reserved_region(address);
        if (entry == this->reserved_regions_.end())
        {
            return false;
        }

        const auto end = address + size;
        const auto region_end = entry->first + entry->second.length;

        if (region_end < end)
        {
            throw std::runtime_error("Cross region protect not supported yet!");
        }

        std::optional<memory_permission> old_first_permissions{};

        auto& committed_regions = entry->second.committed_regions;
        split_regions(committed_regions, {address, end});

        const auto effective_permission = this->get_effective_permissions(permissions);

        for (auto& sub_region : committed_regions)
        {
            if (sub_region.first >= end)
            {
                break;
            }

            const auto sub_region_end = sub_region.first + sub_region.second.length;
            if (sub_region.first >= address && sub_region_end <= end)
            {
                if (!old_first_permissions.has_value())
                {
                    old_first_permissions = sub_region.second.permissions;
                }

                this->apply_memory_protection(sub_region.first, sub_region.second.length, effective_permission);
                sub_region.second.permissions = permissions;
            }
        }

        if (old_permissions)
        {
            *old_permissions = old_first_permissions.value_or(memory_permission::none);
        }

        merge_regions(committed_regions);

        this->update_layout_version();

        return true;
    }

    bool memory_manager::allocate_mmio(const uint64_t address, const size_t size, mmio_read_callback read_cb, mmio_write_callback write_cb)
    {
        if (this->overlaps_reserved_region(address, size, true))
        {
            return false;
        }

        this->carve_host_reserved_hole(address, size);
        this->map_mmio(address, size, std::move(read_cb), std::move(write_cb));

        const auto entry = this->reserved_regions_
                               .try_emplace(address,
                                            reserved_region{
                                                .length = size,
                                                .kind = memory_region_kind::mmio,
                                            })
                               .first;

        entry->second.committed_regions[address] = committed_region{
            .length = size,
            .permissions = memory_permission::read_write,
        };

        this->update_layout_version();

        return true;
    }

    bool memory_manager::allocate_host_memory(const uint64_t address, const size_t size, void* host_pointer,
                                              const nt_memory_permission permissions)
    {
        if (this->overlaps_reserved_region(address, size))
        {
            return false;
        }

        this->map_host_memory(address, size, host_pointer, this->get_effective_permissions(permissions));

        const auto entry = this->reserved_regions_
                               .try_emplace(address,
                                            reserved_region{
                                                .length = size,
                                                .kind = memory_region_kind::mmio,
                                            })
                               .first;

        entry->second.committed_regions[address] = committed_region{
            .length = size,
            .permissions = permissions,
        };

        this->update_layout_version();

        return true;
    }

    void memory_manager::reserve_host_memory_ranges()
    {
        // Cheap, hot-path-safe: only ever adds newly-discovered ranges, never releases existing
        // ones - safe to call frequently (e.g. before every dynamic allocation, so a backend's later
        // host-side claims like JIT code buffers are seen too - see the size-only allocate_memory
        // overload). Existing host_reserved entries are left completely alone, so this can't shrink
        // or momentarily drop a reservation - unlike reset_host_memory_ranges below, which does need
        // to release first and is reserved for the rare case that actually requires it.
        for (const auto& range : this->memory_->reserved_host_ranges())
        {
            this->reserve_host_range_gaps(range.address, range.size);
        }
    }

    void memory_manager::reserve_host_memory_ranges_in(const uint64_t address, const size_t size)
    {
        for (const auto& range : this->memory_->reserved_host_ranges_in(address, size))
        {
            this->reserve_host_range_gaps(range.address, range.size);
        }
    }

    void memory_manager::reserve_host_range_gaps(const uint64_t address, const size_t size)
    {
        // A backend-reported range can partially overlap entries that already exist - its own
        // earlier records, guest allocations made since, or an MMIO hole carved by allocate_mmio -
        // so each still-uncovered gap is recorded individually rather than failing the whole range.
        // Tagged host_reserved (not private_allocation) so allocate_mmio can still claim addresses
        // in here - see its doc comment. Uses allocate_memory_raw (not the public
        // allocate_memory(address, ...), which itself rescans host ranges first) to avoid infinite
        // recursion.
        const uint64_t end = address + size;
        uint64_t cursor = address;

        while (cursor < end)
        {
            const auto next = this->reserved_regions_.upper_bound(cursor);
            if (next != this->reserved_regions_.begin())
            {
                const auto& prev = *std::prev(next);
                const auto prev_end = prev.first + prev.second.length;
                if (prev_end > cursor)
                {
                    cursor = prev_end;
                    continue;
                }
            }

            const auto gap_end = next == this->reserved_regions_.end() ? end : std::min<uint64_t>(end, next->first);
            if (gap_end > cursor && this->allocate_memory_raw(cursor, static_cast<size_t>(gap_end - cursor), nt_memory_permission{}, true,
                                                              memory_region_kind::host_reserved))
            {
                this->host_reserved_addresses_.push_back(cursor);
            }

            cursor = gap_end;
        }
    }

    void memory_manager::carve_host_reserved_hole(const uint64_t address, const size_t size)
    {
        // allocate_mmio may claim addresses inside a host_reserved range (see
        // overlaps_reserved_region's ignore_host_reserved). Nesting the MMIO entry inside the
        // host_reserved one would break the pairwise-non-overlap invariant
        // overlaps_reserved_region's fast path depends on, so the surrounding host_reserved
        // coverage is split around the hole instead.
        const uint64_t end = address + size;

        auto it = this->reserved_regions_.upper_bound(address);
        if (it != this->reserved_regions_.begin())
        {
            --it;
        }

        while (it != this->reserved_regions_.end() && it->first < end)
        {
            const auto region_start = it->first;
            const auto region_end = region_start + it->second.length;

            if (region_end <= address || it->second.kind != memory_region_kind::host_reserved)
            {
                ++it;
                continue;
            }

            assert(it->second.committed_regions.empty());
            it = this->reserved_regions_.erase(it);

            if (region_start < address)
            {
                this->reserved_regions_.try_emplace(region_start, reserved_region{
                                                                      .length = static_cast<size_t>(address - region_start),
                                                                      .kind = memory_region_kind::host_reserved,
                                                                  });
            }

            if (region_end > end)
            {
                it = this->reserved_regions_
                         .try_emplace(end,
                                      reserved_region{
                                          .length = static_cast<size_t>(region_end - end),
                                          .kind = memory_region_kind::host_reserved,
                                      })
                         .first;
                ++it;
            }
        }
    }

    bool memory_manager::host_window_is_free(const uint64_t address, const size_t size) const
    {
        return this->memory_->reserved_host_ranges_in(address, size).empty();
    }

    void memory_manager::reset_host_memory_ranges()
    {
        // Unlike reserve_host_memory_ranges (safe to call frequently, only ever adds), this releases
        // every previously-tracked host_reserved range first - needed only when the backend's answer
        // can genuinely change, not for routine re-scanning. Calling this from a hot path would
        // double the syscall count of every dynamic allocation for no benefit and momentarily
        // un-reserve everything, widening the race window against anything else in the process that
        // maps host memory concurrently - so this must stay a rare, explicit call.
        for (const auto addr : this->host_reserved_addresses_)
        {
            this->release_memory(addr, 0);
        }
        this->host_reserved_addresses_.clear();

        this->reserve_host_memory_ranges();
    }

    bool memory_manager::allocate_memory(const uint64_t address, const size_t size, const nt_memory_permission permissions,
                                         const bool reserve_only, const memory_region_kind kind)
    {
        // Unlike the size-only overload (which always rescans via reserve_host_memory_ranges before
        // picking an address - see its call above), this fixed-address overload must confirm its
        // target window against the live host layout, not just sogen's bookkeeping as of the last
        // rescan: for a backend sharing the guest address space with the host process (FEX on
        // Apple), a host framework can claim a guest-visible VA at any time (e.g. AppKit/SkyLight
        // lazily vm_allocate'ing a window-tag shared page into a gap freed by an earlier guest DLL
        // unload), and a fixed-address mmap over it would silently destroy that foreign mapping.
        // This path is exactly what module preferred-base loads (try_map_module_at_current_base) go
        // through on every DLL map/remap; the rescan makes such a collision show up as an ordinary
        // overlaps_reserved_region hit, which already routes relocatable modules through the
        // existing find_free_allocation_base fallback. Only [address, size) - the sole window this
        // specific allocation could ever clobber - needs rescanning; everything sogen already holds
        // (incl. the arena/__PAGEZERO gap, reserved once and never released) is already accounted
        // for by overlaps_reserved_region without help here, and a full-address-space rescan on
        // every fixed-address call gets more expensive the longer the host process's own memory map
        // grows over a session, for no additional safety.
        this->reserve_host_memory_ranges_in(address, size);

        return this->allocate_memory_raw(address, size, permissions, reserve_only, kind);
    }

    bool memory_manager::allocate_memory_raw(const uint64_t address, const size_t size, const nt_memory_permission permissions,
                                             const bool reserve_only, const memory_region_kind kind)
    {
        if (this->overlaps_reserved_region(address, size))
        {
            return false;
        }

        const auto entry = this->reserved_regions_
                               .try_emplace(address,
                                            reserved_region{
                                                .length = size,
                                                .initial_permission = permissions,
                                                .kind = kind,
                                            })
                               .first;

        if (!reserve_only)
        {
            this->map_memory(address, size, this->get_effective_permissions(permissions));
            entry->second.committed_regions[address] = committed_region{
                .length = size,
                .permissions = permissions,
            };
        }

        this->update_layout_version();

        return true;
    }

    bool memory_manager::commit_memory(const uint64_t address, const size_t size, const nt_memory_permission permissions)
    {
        return this->commit_memory(address, size, permissions, false);
    }

    bool memory_manager::commit_image_memory(const uint64_t address, const size_t size, const nt_memory_permission permissions)
    {
        const auto entry = this->find_reserved_region(address);
        if (entry == this->reserved_regions_.end() || entry->second.kind != memory_region_kind::section_image)
        {
            return false;
        }

        return this->commit_memory(address, size, permissions, true);
    }

    bool memory_manager::commit_memory(const uint64_t address, const size_t size, const nt_memory_permission permissions,
                                       const bool allow_image_section)
    {
        const auto entry = this->find_reserved_region(address);
        if (entry == this->reserved_regions_.end())
        {
            return false;
        }

        if (memory_region_policy::is_section_kind(entry->second.kind) &&
            !(allow_image_section && entry->second.kind == memory_region_kind::section_image))
        {
            return false;
        }

        const auto end = address + size;
        const auto region_end = entry->first + entry->second.length;

        if (region_end < end)
        {
            throw std::runtime_error("Cross region commit not supported yet!");
        }

        auto& committed_regions = entry->second.committed_regions;
        split_regions(committed_regions, {address, end});

        uint64_t last_region_start{};
        const committed_region* last_region{nullptr};

        const auto effective_permission = this->get_effective_permissions(permissions);

        for (auto& sub_region : committed_regions)
        {
            if (sub_region.first >= end)
            {
                break;
            }

            const auto sub_region_end = sub_region.first + sub_region.second.length;
            if (sub_region.first >= address && sub_region_end <= end)
            {
                const auto map_start = last_region ? (last_region_start + last_region->length) : address;
                const auto map_length = sub_region.first - map_start;

                if (map_length > 0)
                {
                    this->map_memory(map_start, static_cast<size_t>(map_length), effective_permission);
                    committed_regions[map_start] = committed_region{
                        .length = static_cast<size_t>(map_length),
                        .permissions = permissions,
                    };
                }

                // Update protection for existing committed region when re-committing
                this->apply_memory_protection(sub_region.first, sub_region.second.length, effective_permission);
                sub_region.second.permissions = permissions;

                last_region_start = sub_region.first;
                last_region = &sub_region.second;
            }
        }

        if (!last_region || (last_region_start + last_region->length) < end)
        {
            const auto map_start = last_region ? (last_region_start + last_region->length) : address;
            const auto map_length = end - map_start;

            this->map_memory(map_start, static_cast<size_t>(map_length), effective_permission);
            committed_regions[map_start] = committed_region{
                .length = static_cast<size_t>(map_length),
                .permissions = permissions,
            };
        }

        merge_regions(committed_regions);

        this->update_layout_version();

        return true;
    }

    bool memory_manager::decommit_memory(const uint64_t address, const size_t size)
    {
        const auto entry = this->find_reserved_region(address);
        if (entry == this->reserved_regions_.end())
        {
            return false;
        }

        if (entry->second.kind == memory_region_kind::mmio)
        {
            throw std::runtime_error("Not allowed to decommit MMIO!");
        }

        const auto end = address + size;
        const auto region_end = entry->first + entry->second.length;

        if (region_end < end)
        {
            throw std::runtime_error("Cross region decommit not supported yet!");
        }

        auto& committed_regions = entry->second.committed_regions;

        split_regions(committed_regions, {address, end});

        for (auto i = committed_regions.begin(); i != committed_regions.end();)
        {
            if (i->first >= end)
            {
                break;
            }

            const auto sub_region_end = i->first + i->second.length;
            if (i->first >= address && sub_region_end <= end)
            {
                this->unmap_memory(i->first, i->second.length);
                i = committed_regions.erase(i);
                continue;
            }

            ++i;
        }

        this->update_layout_version();

        return true;
    }

    void memory_manager::release_host_claims(const uint64_t released_end)
    {
        // Hands the reservation-free gap around a just-released range back to the backend (see
        // memory_interface::release_guest_address_range). Since reserved_regions_ entries are
        // pairwise non-overlapping, the space between the last region starting below released_end
        // and the first region starting at or above it contains no reservations. Passing this whole
        // gap (not just the released range) lets the backend drop claim pages that straddle the
        // released range's unaligned edges.
        uint64_t gap_start = 0;
        uint64_t gap_end = MAX_ALLOCATION_END_EXCL;

        const auto next = this->reserved_regions_.lower_bound(released_end);
        if (next != this->reserved_regions_.end())
        {
            gap_end = next->first;
        }

        if (next != this->reserved_regions_.begin())
        {
            const auto& prev = *std::prev(next);
            gap_start = prev.first + prev.second.length;
        }

        if (gap_start >= gap_end)
        {
            return;
        }

        this->memory_->release_guest_address_range(gap_start, static_cast<size_t>(gap_end - gap_start));
    }

    bool memory_manager::release_memory(const uint64_t address, size_t size)
    {
        if (!size)
        {
            const auto entry = this->reserved_regions_.find(address);
            if (entry == this->reserved_regions_.end())
            {
                return false;
            }

            const auto entry_length = entry->second.length;

            auto& committed_regions = entry->second.committed_regions;
            for (auto i = committed_regions.begin(); i != committed_regions.end();)
            {
                this->unmap_memory(i->first, i->second.length);
                i = committed_regions.erase(i);
            }

            this->reserved_regions_.erase(entry);
            this->release_host_claims(address + entry_length);
            this->update_layout_version();
            return true;
        }

        const auto aligned_start = page_align_down(address);
        const auto aligned_end = page_align_up(address + size);
        size = static_cast<size_t>(aligned_end - aligned_start);

        const auto entry = this->find_reserved_region(aligned_start);
        if (entry == this->reserved_regions_.end())
        {
            return false;
        }

        const auto reserved_start = entry->first;
        const auto reserved_end = entry->first + entry->second.length;

        if (reserved_end < aligned_end)
        {
            throw std::runtime_error("Cross region release not supported yet!");
        }

        reserved_region region = std::move(entry->second);
        this->reserved_regions_.erase(entry);

        auto& committed_regions = region.committed_regions;
        split_regions(committed_regions, {aligned_start, aligned_end});

        for (auto i = committed_regions.begin(); i != committed_regions.end();)
        {
            if (i->first >= aligned_end)
            {
                break;
            }

            const auto sub_region_end = i->first + i->second.length;
            if (i->first >= aligned_start && sub_region_end <= aligned_end)
            {
                this->unmap_memory(i->first, i->second.length);
                i = committed_regions.erase(i);
            }
            else
            {
                ++i;
            }
        }

        committed_region_map left_committed{};
        committed_region_map right_committed{};

        for (const auto& sub_region : committed_regions)
        {
            if (sub_region.first < aligned_start)
            {
                left_committed.emplace(sub_region.first, sub_region.second);
            }
            else if (sub_region.first >= aligned_end)
            {
                right_committed.emplace(sub_region.first, sub_region.second);
            }
        }

        if (reserved_start < aligned_start)
        {
            reserved_region left_region{};
            left_region.length = static_cast<size_t>(aligned_start - reserved_start);
            left_region.initial_permission = region.initial_permission;
            left_region.kind = region.kind;
            left_region.mapped_filename = region.mapped_filename;
            left_region.committed_regions = std::move(left_committed);
            this->reserved_regions_.try_emplace(reserved_start, std::move(left_region));
        }

        if (aligned_end < reserved_end)
        {
            reserved_region right_region{};
            right_region.length = static_cast<size_t>(reserved_end - aligned_end);
            right_region.initial_permission = region.initial_permission;
            right_region.kind = region.kind;
            right_region.mapped_filename = region.mapped_filename;
            right_region.committed_regions = std::move(right_committed);
            this->reserved_regions_.try_emplace(aligned_end, std::move(right_region));
        }

        this->release_host_claims(aligned_end);
        this->update_layout_version();
        return true;
    }

    void memory_manager::unmap_all_memory()
    {
        for (const auto& reserved_region : this->reserved_regions_)
        {
            for (const auto& region : reserved_region.second.committed_regions)
            {
                this->unmap_memory(region.first, region.second.length);
            }
        }

        this->reserved_regions_.clear();
    }

    namespace
    {
        // Backstop for allocate_memory's pick/confirm loop below. Every non-settling iteration does a
        // full reserve_host_memory_ranges() rescan, which records at least one more foreign range into
        // reserved_regions_, so find_free_allocation_base is guaranteed to make progress; this cap only
        // guards against pathological churn.
        constexpr int max_host_reserved_retries = 8;
    }

    uint64_t memory_manager::allocate_memory(const size_t size, const nt_memory_permission permissions, const bool reserve_only,
                                             uint64_t start, const memory_region_kind kind)
    {
        // Backends sharing the guest address space with this process (e.g. FEX) can allocate their own
        // host-side memory (JIT code buffers, thread stacks, a framework's lazy vm_allocate, a GCD
        // worker stack) at any point during execution, so find_free_allocation_base - which picks
        // purely from sogen's own in-memory bookkeeping - can hand back an address a foreign host
        // mapping has quietly claimed since the last scan. reserve_guest_address_range below would
        // then mmap(MAP_FIXED) over the pick, silently destroying that foreign mapping. An
        // unconditional full reserve_host_memory_ranges() rescan on every call would prevent that,
        // but costs O(host VM regions) real Mach IPC syscalls, growing without bound over a session.
        //
        // Instead: pick from sogen's current view, then confirm just the picked window is still free at
        // the host level with a bounded windowed query (host_window_is_free - usually a single
        // mach_vm_region call, and correct even for a foreign mapping straddling the window's lower
        // edge, since mach_vm_region reports the region *containing* the probe, not only those above
        // it). Only on an actual collision - rare, since it needs a foreign claim landing exactly in the
        // freshly-picked gap - pay for a full rescan to record every current foreign range and re-pick
        // past them. This never widens the pre-existing race between the confirm and the MAP_FIXED
        // below (nothing makes that atomic without a non-clobbering host reservation); it strictly
        // narrows it, since the confirm now sits immediately before the claim rather than behind a
        // full-address-space scan.
        //
        // Deliberately host_window_is_free (a pure probe), not reserve_host_memory_ranges_in: the
        // latter records a *clamped* window slice into reserved_regions_, which would then block the
        // full rescan below from recording an intruder's full extent and make re-picking crawl across
        // it in size-sized steps instead of skipping it in one go.
        const uint64_t allocation_base = this->find_free_host_allocation_base(size, start);
        if (!allocation_base)
        {
            return 0;
        }

        // Claim the range at the host OS level immediately, even though it may only be reserve-only
        // (not yet backed by map_memory) - see reserve_guest_address_range's doc comment. Safe now that
        // the window above was confirmed free; done only for this freshly-picked address sogen doesn't
        // already know about.
        this->memory_->reserve_guest_address_range(allocation_base, size);

        // Uses allocate_memory_raw (not the public allocate_memory(address, ...), which itself rescans
        // via reserve_host_memory_ranges) - a rescan here would immediately re-discover the
        // reserve_guest_address_range call just above as a "foreign" host allocation (it is a real
        // host mmap, made before reserved_regions_ knows about it) and mark allocation_base
        // host_reserved, making the allocate_memory_raw call right below self-conflict and fail. The
        // confirm above already covers this window.
        if (!this->allocate_memory_raw(allocation_base, size, permissions, reserve_only, kind))
        {
            this->release_host_claims(allocation_base + size);
            return 0;
        }

        return allocation_base;
    }

    uint64_t memory_manager::find_free_host_allocation_base(const size_t size, const uint64_t start)
    {
        // Pick from sogen's current bookkeeping, then confirm just the picked window is still free at the
        // host level (host_window_is_free - a pure probe, no clamped slice recorded). Only on an actual
        // collision - a foreign host mapping having landed in the freshly-picked gap since the last scan -
        // rescan and re-pick. Bounded by max_host_reserved_retries: each non-settling iteration records at
        // least one more foreign range overlapping the just-rejected pick, so find_free_allocation_base
        // cannot return it again and is guaranteed to make progress. Returns 0 when a pick could not
        // be confirmed.
        //
        // The rescan is BOTH the full reserve_host_memory_ranges() AND the windowed
        // reserve_host_memory_ranges_in(pick, size), because the two cover cases the other misses:
        //   * The full scan records every currently-visible foreign range at once, so a pick landing at the
        //     low edge of a large contiguous host-occupied region jumps clear past the whole region in one
        //     step. The windowed record alone would only retire its own pick-sized slice, unable to escape a
        //     region wider than max_host_reserved_retries picks.
        //   * The full scan can have a structural blind spot the windowed host_window_is_free probe does not:
        //     a backend may omit ranges it considers guest-owned rather than foreign from the full
        //     reserved_host_ranges() scan while its windowed reserved_host_ranges_in() still reports them
        //     occupied. An auto-placement whose lowest free bookkeeping hole is such a range would otherwise
        //     have host_window_is_free reject the same pick every iteration while a full rescan recorded
        //     nothing new, spinning to exhaustion. The windowed record retires exactly the window
        //     host_window_is_free just flagged, closing that gap.
        return this->find_free_host_allocation_base(size, start, MAX_ALLOCATION_END_EXCL - 1);
    }

    uint64_t memory_manager::find_free_host_allocation_base(const size_t size, const uint64_t start, const uint64_t highest_address)
    {
        for (int attempt = 0;; ++attempt)
        {
            const uint64_t allocation_base =
                this->find_free_allocation_base(size, start, ALLOCATION_GRANULARITY, MIN_ALLOCATION_ADDRESS, highest_address);
            if (!allocation_base)
            {
                return 0;
            }

            if (this->host_window_is_free(allocation_base, size))
            {
                return allocation_base;
            }

            if (attempt >= max_host_reserved_retries)
            {
                return 0;
            }

            this->reserve_host_memory_ranges();
            this->reserve_host_memory_ranges_in(allocation_base, size);
        }
    }

    uint64_t memory_manager::find_free_allocation_base(const size_t size, const uint64_t start) const
    {
        return this->find_free_allocation_base(size, start, ALLOCATION_GRANULARITY, MIN_ALLOCATION_ADDRESS, MAX_ALLOCATION_END_EXCL - 1);
    }

    namespace
    {
        bool is_power_of_two(const uint64_t value)
        {
            return value != 0 && (value & (value - 1)) == 0;
        }

        std::optional<uint64_t> checked_align_up(const uint64_t value, const uint64_t alignment)
        {
            if (!is_power_of_two(alignment) || value > UINT64_MAX - (alignment - 1))
            {
                return std::nullopt;
            }

            return align_up(value, alignment);
        }
    }

    uint64_t memory_manager::find_free_allocation_base(const size_t size, const uint64_t start, uint64_t alignment, uint64_t lowest_address,
                                                       uint64_t highest_address) const
    {
        if (!is_power_of_two(alignment) || alignment < ALLOCATION_GRANULARITY || size == 0)
        {
            return 0;
        }

        lowest_address = std::max<uint64_t>(lowest_address, MIN_ALLOCATION_ADDRESS);
        highest_address = std::min<uint64_t>(highest_address ? highest_address : MAX_ALLOCATION_END_EXCL - 1, MAX_ALLOCATION_END_EXCL - 1);
        if (lowest_address > highest_address)
        {
            return 0;
        }

        auto candidate = start ? start : this->default_allocation_address_;
        if (candidate < lowest_address || candidate > highest_address)
        {
            candidate = lowest_address;
        }

        auto aligned_start = checked_align_up(candidate, alignment);
        if (!aligned_start.has_value())
        {
            return 0;
        }

        uint64_t start_address = *aligned_start;

        // Since reserved_regions_ is a sorted map, we can iterate through it
        // and find gaps between regions
        while (start_address <= highest_address)
        {
            const auto end_address = start_address + size;
            if (end_address < start_address || end_address > MAX_ALLOCATION_END_EXCL || end_address - 1 > highest_address)
            {
                return 0;
            }

            bool conflict = false;

            // Check if the proposed range [start_address, start_address+size) conflicts with any existing region
            for (const auto& region : this->reserved_regions_)
            {
                const auto region_end = region.first + region.second.length;
                if (region_end < region.first)
                {
                    return 0;
                }

                // If this region ends before our start, skip it
                if (region_end <= start_address)
                {
                    continue;
                }

                // If this region starts after our end, we're done checking (map is sorted)
                if (region.first >= end_address)
                {
                    break;
                }

                // Otherwise, we have a conflict
                conflict = true;
                // Move start_address past this conflicting region
                aligned_start = checked_align_up(region_end, alignment);
                if (!aligned_start.has_value())
                {
                    return 0;
                }

                start_address = *aligned_start;
                break;
            }

            // If no conflict was found, we have our address
            if (!conflict)
            {
                return start_address;
            }
        }

        return 0;
    }

    region_info memory_manager::get_region_info(const uint64_t address)
    {
        region_info result{};
        result.start = page_align_down(address);
        result.length = static_cast<size_t>(MAX_ALLOCATION_END_EXCL - result.start);
        result.permissions = nt_memory_permission();
        result.initial_permissions = nt_memory_permission();
        result.allocation_base = {};
        result.allocation_length = result.length;
        result.is_committed = false;
        result.is_reserved = false;

        if (this->reserved_regions_.empty())
        {
            return result;
        }

        auto upper_bound = this->reserved_regions_.upper_bound(address);
        if (upper_bound == this->reserved_regions_.begin())
        {
            result.length = static_cast<size_t>(upper_bound->first - result.start);
            return result;
        }

        const auto entry = --upper_bound;
        const auto reserved_end = entry->first + entry->second.length;

        if (reserved_end <= address)
        {
            auto next = std::next(entry);

            result.start = page_align_down(address);
            result.length = next == this->reserved_regions_.end() ? static_cast<size_t>(MAX_ALLOCATION_END_EXCL - result.start)
                                                                  : static_cast<size_t>(next->first - result.start);

            return result;
        }

        const auto& reserved_region = entry->second;
        const auto& committed_regions = reserved_region.committed_regions;

        result.is_reserved = true;
        result.allocation_base = entry->first;
        result.allocation_length = reserved_region.length;
        result.initial_permissions = reserved_region.initial_permission;
        result.kind = reserved_region.kind;
        result.start = page_align_down(address);
        result.length = static_cast<size_t>(reserved_end - result.start);

        if (committed_regions.empty())
        {
            return result;
        }

        auto committed_bound = committed_regions.upper_bound(address);
        if (committed_bound == committed_regions.begin())
        {
            result.length = static_cast<size_t>(committed_bound->first - result.start);
            return result;
        }

        const auto committed_entry = --committed_bound;
        const auto committed_end = committed_entry->first + committed_entry->second.length;

        if (committed_end <= address)
        {
            auto next_committed = std::next(committed_entry);

            result.length = next_committed == committed_regions.end() ? static_cast<size_t>(reserved_end - result.start)
                                                                      : static_cast<size_t>(next_committed->first - result.start);

            return result;
        }

        result.is_committed = true;
        result.permissions = committed_entry->second.permissions;
        result.length = static_cast<size_t>(committed_end - result.start);

        return result;
    }

    std::optional<std::u16string> memory_manager::get_region_mapped_filename(const uint64_t address) const
    {
        if (this->reserved_regions_.empty())
        {
            return std::nullopt;
        }

        auto upper_bound = this->reserved_regions_.upper_bound(address);
        if (upper_bound == this->reserved_regions_.begin())
        {
            return std::nullopt;
        }

        const auto entry = --upper_bound;
        if (entry->first + entry->second.length <= address || entry->second.mapped_filename.empty())
        {
            return std::nullopt;
        }

        return entry->second.mapped_filename;
    }

    void memory_manager::set_region_mapped_filename(const uint64_t address, std::u16string filename)
    {
        const auto entry = this->find_reserved_region(address);
        if (entry == this->reserved_regions_.end())
        {
            return;
        }

        entry->second.mapped_filename = std::move(filename);
    }

    memory_manager::reserved_region_map::iterator memory_manager::find_reserved_region(const uint64_t address)
    {
        if (this->reserved_regions_.empty())
        {
            return this->reserved_regions_.end();
        }

        auto upper_bound = this->reserved_regions_.upper_bound(address);
        if (upper_bound == this->reserved_regions_.begin())
        {
            return this->reserved_regions_.end();
        }

        const auto entry = --upper_bound;
        if (entry->first + entry->second.length <= address)
        {
            return this->reserved_regions_.end();
        }

        return entry;
    }

    bool memory_manager::overlaps_reserved_region(const uint64_t address, const size_t size, const bool ignore_host_reserved) const
    {
        // reserved_regions_ maintains a pairwise-non-overlapping invariant (every insertion path
        // checks this same function first, and allocate_mmio additionally splits any host_reserved
        // coverage around its claim - see carve_host_reserved_hole), so a query range can only ever
        // overlap the region starting immediately before `address` and the regions starting within
        // [address, address + size) - an O(n) scan of every entry is unnecessary. This matters in practice:
        // reserve_host_memory_ranges() calls this once per host-reported range on every fixed-address
        // module map, so an O(n) scan here made repeated remaps of the same module (e.g. COM
        // registration DLL load/unload churn) cost O(n) work per remap as both the host VM map and
        // reserved_regions_ grow over a session.
        auto it = this->reserved_regions_.upper_bound(address);

        if (it != this->reserved_regions_.begin())
        {
            const auto& prev = *std::prev(it);
            if (!(ignore_host_reserved && prev.second.kind == memory_region_kind::host_reserved) &&
                regions_with_length_intersect(address, size, prev.first, prev.second.length))
            {
                return true;
            }
        }

        for (; it != this->reserved_regions_.end() && it->first < address + size; ++it)
        {
            if (ignore_host_reserved && it->second.kind == memory_region_kind::host_reserved)
            {
                continue;
            }

            if (regions_with_length_intersect(address, size, it->first, it->second.length))
            {
                return true;
            }
        }

        return false;
    }

    memory_region_kind memory_manager::get_region_kind(const uint64_t address) const
    {
        if (this->reserved_regions_.empty())
        {
            return memory_region_kind::free;
        }

        auto upper_bound = this->reserved_regions_.upper_bound(address);
        if (upper_bound == this->reserved_regions_.begin())
        {
            return memory_region_kind::free;
        }

        const auto entry = --upper_bound;
        if (entry->first + entry->second.length <= address)
        {
            return memory_region_kind::free;
        }

        return entry->second.kind;
    }

    void memory_manager::read_memory(const uint64_t address, void* data, const size_t size) const
    {
        this->memory_->read_memory(address, data, size);
    }

    bool memory_manager::try_read_memory(const uint64_t address, void* data, const size_t size) const
    {
        try
        {
            return this->memory_->try_read_memory(address, data, size);
        }
        catch (...)
        {
            return false;
        }
    }

    void memory_manager::write_memory(const uint64_t address, const void* data, const size_t size)
    {
        this->memory_->write_memory(address, data, size);
    }

    bool memory_manager::try_write_memory(const uint64_t address, const void* data, const size_t size)
    {
        try
        {
            return this->memory_->try_write_memory(address, data, size);
        }
        catch (...)
        {
            return false;
        }
    }

    void memory_manager::map_mmio(const uint64_t address, const size_t size, mmio_read_callback read_cb, mmio_write_callback write_cb)
    {
        this->memory_->map_mmio(address, size, std::move(read_cb), std::move(write_cb));
    }

    void memory_manager::map_memory(const uint64_t address, const size_t size, const memory_permission permissions)
    {
        this->memory_->map_memory(address, size, permissions);
    }

    void memory_manager::map_host_memory(const uint64_t address, const size_t size, void* host_pointer, const memory_permission permissions)
    {
        this->memory_->map_host_memory(address, size, host_pointer, permissions);
    }

    bool memory_manager::host_memory_aliasing_is_coherent() const
    {
        return this->memory_->host_memory_aliasing_is_coherent();
    }

    void memory_manager::flush_host_memory_cache(const void* host_pointer, const size_t size)
    {
        this->memory_->flush_host_memory_cache(host_pointer, size);
    }

    void memory_manager::unmap_memory(const uint64_t address, const size_t size)
    {
        this->memory_->unmap_memory(address, size);
    }

    void memory_manager::apply_memory_protection(const uint64_t address, const size_t size, const memory_permission permissions)
    {
        this->memory_->apply_memory_protection(address, size, permissions);
    }

} // namespace sogen
