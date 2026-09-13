#include "../std_include.hpp"
#include "mig_kernel_servers.hpp"

#include "../macos_emulator.hpp"

#include <cstring>

#include <address_utils.hpp>

#include <set>

namespace sogen::mach
{
    namespace
    {
        constexpr uint32_t VM_FLAGS_ANYWHERE = 0x1;

        // Request 4811's fields, byte offsets from args_offset. The port descriptor and NDR record that
        // precede them are what args_offset already accounts for.
        constexpr size_t VM_MAP_ADDRESS = 0;
        constexpr size_t VM_MAP_SIZE = 8;
        constexpr size_t VM_MAP_MASK = 16;
        constexpr size_t VM_MAP_FLAGS = 24;
        constexpr size_t VM_MAP_CUR_PROTECTION = 40;

        // Request 4807's fields, byte offsets from args_offset.
        constexpr size_t VM_COPY_SOURCE = 0;
        constexpr size_t VM_COPY_SIZE = 8;
        constexpr size_t VM_COPY_DEST = 16;

        // mach_vm_copy. Refusing it is not neutral: CoreFoundation copies a buffer this way while
        // parsing a property list and does not check the result, so the failure surfaces later as an
        // over-release of an object that was never populated.
        std::vector<uint8_t> vm_copy_routine(macos_emulator& emu, const mig_request& request)
        {
            if (!request.has_ndr())
            {
                return make_mig_error_bytes(request, mig_error::bad_arguments);
            }

            const auto source = request.arg_u64(VM_COPY_SOURCE);
            const auto size = request.arg_u64(VM_COPY_SIZE);
            const auto destination = request.arg_u64(VM_COPY_DEST);

            if (size == 0)
            {
                mig_reply_builder empty{request.call, emu.mach.ports};
                empty.append_ndr();
                empty.append_u32(kr::success);
                return empty.finish();
            }

            if (size > MACOS_MAX_MMAP_END_EXCL)
            {
                return make_mig_error_bytes(request, kr::invalid_argument);
            }

            // Both ranges must already be mapped: unlike mach_vm_map this routine copies into memory the
            // caller owns rather than creating any.
            std::vector<uint8_t> bytes(static_cast<size_t>(size), 0);
            if (!emu.memory.try_read_memory(source, bytes.data(), bytes.size()))
            {
                return make_mig_error_bytes(request, kr::invalid_address);
            }

            if (!emu.memory.try_write_memory(destination, bytes.data(), bytes.size()))
            {
                return make_mig_error_bytes(request, kr::protection_failure);
            }

            mig_reply_builder builder{request.call, emu.mach.ports};
            builder.append_ndr();
            builder.append_u32(kr::success);
            return builder.finish();
        }

        // Request 4813's fields, byte offsets from args_offset. src_task travels as a port descriptor,
        // which args_offset already accounts for. MIG packs its request structures to 4, so the 64-bit
        // src_address follows the 32-bit flags with no padding between them; the request libobjc sends
        // for its trampoline template is 92 bytes, which is 24 header + 4 body + 12 port descriptor +
        // 8 NDR + these 44.
        constexpr size_t VM_REMAP_TARGET_ADDRESS = 0;
        constexpr size_t VM_REMAP_SIZE = 8;
        constexpr size_t VM_REMAP_MASK = 16;
        constexpr size_t VM_REMAP_FLAGS = 24;
        constexpr size_t VM_REMAP_SRC_ADDRESS = 28;
        constexpr size_t VM_REMAP_COPY = 36;

        // mach_vm_remap. The objc runtime uses it to give itself an executable copy of its trampoline
        // template pages, and reports "vm_remap trampolines failed" and aborts when it is refused --
        // which takes every Swift and AppKit process with it.
        //
        // sogen has one address space and no way to show the same physical bytes at two addresses, so a
        // remap is a copy. That is the whole of the deviation: a guest that writes through one view will
        // not see it in the other. It is reported by name when the source is writable, because that is
        // the only case where the difference is observable.
        std::vector<uint8_t> vm_remap_routine(macos_emulator& emu, const mig_request& request)
        {
            if (!request.has_ndr())
            {
                return make_mig_error_bytes(request, mig_error::bad_arguments);
            }

            const auto requested = request.arg_u64(VM_REMAP_TARGET_ADDRESS);
            const auto size = request.arg_u64(VM_REMAP_SIZE);
            const auto mask = request.arg_u64(VM_REMAP_MASK);
            const auto flags = request.arg_u32(VM_REMAP_FLAGS / sizeof(uint32_t));
            const auto source = request.arg_u64(VM_REMAP_SRC_ADDRESS);
            const auto copy = request.arg_u32(VM_REMAP_COPY / sizeof(uint32_t));

            if (size == 0 || size > MACOS_MAX_MMAP_END_EXCL)
            {
                return make_mig_error_bytes(request, kr::invalid_argument);
            }

            constexpr uint32_t VM_PROT_ALL = 0x7;

            const auto rounded = static_cast<size_t>(page_align_up(size, MACOS_PAGE_SIZE));
            const auto aligned_source = page_align_down(source, MACOS_PAGE_SIZE);
            const auto anywhere = (flags & VM_FLAGS_ANYWHERE) != 0;

            // A share (copy == 0) placed anywhere is satisfiable exactly: hand back the source itself.
            // Both views are then literally the same pages, which is what the caller asked for and what
            // a copy can never be -- AttributeGraph aliases its graph pages this way and aborts when the
            // two diverge. Only a share pinned to a caller-chosen address is beyond a single address
            // space, and that one still copies, reported by name below.
            if (copy == 0 && anywhere)
            {
                mig_reply_builder shared{request.call, emu.mach.ports};
                shared.append_ndr();
                shared.append_u32(kr::success);
                shared.append_u64(aligned_source);
                shared.append_u32(VM_PROT_ALL);
                shared.append_u32(VM_PROT_ALL);
                return shared.finish();
            }

            std::vector<uint8_t> bytes(rounded, 0);
            if (!emu.memory.try_read_memory(aligned_source, bytes.data(), bytes.size()))
            {
                return make_mig_error_bytes(request, kr::invalid_address);
            }

            auto target = anywhere ? emu.memory.find_free_allocation_base(rounded) : page_align_down(requested, MACOS_PAGE_SIZE);
            if (target == 0)
            {
                return make_mig_error_bytes(request, kr::no_space);
            }

            if (mask != 0 && (target & mask) != 0)
            {
                target = (target + mask) & ~mask;
            }

            // A share pinned to a caller-chosen address is the double-mapping idiom -- AttributeGraph
            // uses it so a growable arena wraps without a bounds check. It can be honoured exactly by
            // re-backing both ranges with one host allocation this manager owns: after that the two
            // guest addresses really are the same bytes, which a copy can never be.
            if (copy == 0 && target != aligned_source && !macos_memory_manager::is_reserved_range(target, rounded))
            {
                const auto existing = emu.memory.get_region_info(aligned_source);
                const auto source_permissions = existing.has_value() ? existing->permissions : memory_permission::read_write;

                if (auto* backing = emu.memory.acquire_shared_backing(rounded); backing != nullptr)
                {
                    std::memcpy(backing, bytes.data(), bytes.size());

                    emu.memory.release_memory(aligned_source, rounded);
                    emu.memory.release_memory(target, rounded);

                    if (emu.memory.map_host_file_memory(aligned_source, rounded, backing, source_permissions) &&
                        emu.memory.map_host_file_memory(target, rounded, backing, source_permissions))
                    {
                        mig_reply_builder aliased{request.call, emu.mach.ports};
                        aliased.append_ndr();
                        aliased.append_u32(kr::success);
                        aliased.append_u64(target);
                        aliased.append_u32(VM_PROT_ALL);
                        aliased.append_u32(VM_PROT_ALL);
                        return aliased.finish();
                    }

                    // Put the source back as plain memory, so a failed attempt leaves the guest no worse
                    // off than it was before it.
                    if (!emu.memory.get_region_info(aligned_source).has_value() &&
                        emu.memory.allocate_memory(aligned_source, rounded, source_permissions))
                    {
                        emu.memory.try_write_memory(aligned_source, bytes.data(), bytes.size());
                    }
                }
            }

            // VM_FLAGS_OVERWRITE, and the same rule MAP_FIXED follows: the caller named the range, so
            // whatever is there gives way.
            if (!anywhere && !macos_memory_manager::is_reserved_range(target, rounded))
            {
                emu.memory.release_memory(target, rounded);
            }

            // Read, write and execute: objc remaps its template as text it will then jump into, and the
            // real call asks for VM_PROT_ALL as its max protection.
            constexpr auto permissions = memory_permission::read | memory_permission::write | memory_permission::exec;
            if (!emu.memory.allocate_memory(target, rounded, permissions))
            {
                return make_mig_error_bytes(request, kr::no_space);
            }

            if (!emu.memory.try_write_memory(target, bytes.data(), bytes.size()))
            {
                emu.memory.release_memory(target, rounded);
                return make_mig_error_bytes(request, kr::invalid_address);
            }

            if (copy == 0)
            {
                static bool reported = false;
                if (!reported)
                {
                    reported = true;
                    emu.log.warn("mach_vm_remap asked for a shared mapping at a fixed address; sogen has one address space and cannot "
                                 "show the same pages twice, so it copies and writes through one view are not seen through the other\n");
                }
            }

            mig_reply_builder builder{request.call, emu.mach.ports};
            builder.append_ndr();
            builder.append_u32(kr::success);
            builder.append_u64(target);
            builder.append_u32(VM_PROT_ALL);
            builder.append_u32(VM_PROT_ALL);
            return builder.finish();
        }

        std::vector<uint8_t> vm_map_routine(macos_emulator& emu, const mig_request& request)
        {
            if (!request.has_ndr())
            {
                return make_mig_error_bytes(request, mig_error::bad_arguments);
            }

            const auto requested = request.arg_u64(VM_MAP_ADDRESS);
            const auto size = request.arg_u64(VM_MAP_SIZE);
            const auto mask = request.arg_u64(VM_MAP_MASK);
            const auto flags = static_cast<uint32_t>(request.arg_u32(VM_MAP_FLAGS / sizeof(uint32_t)));
            const auto protection = static_cast<int32_t>(request.arg_u32(VM_MAP_CUR_PROTECTION / sizeof(uint32_t)));

            if (size == 0 || size > MACOS_MAX_MMAP_END_EXCL)
            {
                return make_mig_error_bytes(request, kr::invalid_argument);
            }

            // A memory entry is a handle over a range that already exists, so mapping one is a lookup,
            // not an allocation: sogen has one address space and cannot show the same bytes twice.
            const auto object = request.descriptor(0);
            if (object.has_value() && object->name != PORT_NULL)
            {
                const auto* entry = emu.mach.find_memory_entry(object->name);
                if (entry == nullptr)
                {
                    static std::set<port_name_t> reported{};
                    if (reported.insert(object->name).second)
                    {
                        emu.log.warn("mach_vm_map names memory entry 0x%x, which no routine in sogen made\n", object->name);
                    }

                    return make_mig_error_bytes(request, kr::invalid_argument);
                }

                if (size > entry->size)
                {
                    emu.log.warn("mach_vm_map asks for %llu bytes of a %llu byte memory entry\n", static_cast<unsigned long long>(size),
                                 static_cast<unsigned long long>(entry->size));
                    return make_mig_error_bytes(request, kr::invalid_argument);
                }

                mig_reply_builder mapped{request.call, emu.mach.ports};
                mapped.append_ndr();
                mapped.append_u32(static_cast<uint32_t>(kr::success));
                mapped.append_u64(entry->address);
                return mapped.finish();
            }

            const auto length = static_cast<size_t>(page_align_up(size, MACOS_PAGE_SIZE));
            const auto permissions = macos_prot_to_permission(protection);

            uint64_t base = 0;
            if ((flags & VM_FLAGS_ANYWHERE) != 0 || requested == 0)
            {
                base = emu.memory.find_free_allocation_base(length, 0);

                // dyld's mappings ask for no alignment mask, so a satisfied-first-try search is enough
                // here; a mask that the first hole cannot satisfy fails loudly rather than walking the
                // whole address space.
                if (mask != 0 && (base & mask) != 0)
                {
                    base = 0;
                }
            }
            else
            {
                base = page_align_down(requested, MACOS_PAGE_SIZE);
            }

            if (base == 0 || !emu.memory.allocate_memory(base, length, permissions))
            {
                return make_mig_error_bytes(request, kr::no_space);
            }

            mig_reply_builder builder{request.call, emu.mach.ports};
            builder.append_ndr();
            builder.append_u32(static_cast<uint32_t>(kr::success));
            builder.append_u64(base);
            return builder.finish();
        }

        std::vector<uint8_t> reclamation_buffer_allocate_routine(macos_emulator& emu, const mig_request& request)
        {
            if (!request.has_ndr())
            {
                return make_mig_error_bytes(request, mig_error::bad_arguments);
            }

            const auto max_len = static_cast<uint64_t>(request.arg_u32(0));
            if (max_len == 0 || max_len > MACOS_MAX_MMAP_END_EXCL)
            {
                return make_mig_error_bytes(request, kr::invalid_argument);
            }

            const auto length = static_cast<size_t>(page_align_up(max_len, MACOS_PAGE_SIZE));
            const auto base = emu.memory.find_free_allocation_base(length, 0);

            if (base == 0 || !emu.memory.allocate_memory(base, length, memory_permission::read_write))
            {
                return make_mig_error_bytes(request, kr::no_space);
            }

            mig_reply_builder builder{request.call, emu.mach.ports};
            builder.append_ndr();
            builder.append_u32(static_cast<uint32_t>(kr::success));
            builder.append_u64(base);
            builder.append_u64(0);
            return builder.finish();
        }

        std::vector<uint8_t> make_memory_entry_routine(macos_emulator& emu, const mig_request& request)
        {
            if (!request.has_ndr())
            {
                return make_mig_error_bytes(request, mig_error::bad_arguments);
            }

            const auto size = request.arg_u64(0);
            if (size == 0)
            {
                return make_mig_error_bytes(request, kr::invalid_argument);
            }

            // The entry is a handle over a range the guest already mapped, named here so a later
            // mach_vm_map of it hands the same range back. The reply carries no RetCode: size is inout,
            // and the generated stub type-checks msgh_size == 56 exactly.
            const auto entry = emu.mach.create_memory_entry(request.arg_u64(sizeof(uint64_t)), size);

            mig_reply_builder builder{request.call, emu.mach.ports};
            builder.append_port_descriptor({.name = entry, .disposition = disposition::make_send, .type = descriptor_type::port});
            builder.append_ndr();
            builder.append_u64(size);
            return builder.finish();
        }
    }

    // mach_vm routines are sent to the TASK port, not a memory port -- the trace shows remote = the task
    // port for every 4811 send.
    void register_vm_routines(mig_server_table& table)
    {
        table.register_routine(kernel_object_kind::task, 4811, vm_map_routine, "_kernelrpc_mach_vm_map");
        table.register_routine(kernel_object_kind::task, 4807, vm_copy_routine, "mach_vm_copy");
        table.register_routine(kernel_object_kind::task, 4813, vm_remap_routine, "mach_vm_remap");
        table.register_routine(kernel_object_kind::task, 4817, make_memory_entry_routine, "mach_make_memory_entry_64");
        table.register_routine(kernel_object_kind::task, 4822, reclamation_buffer_allocate_routine,
                               "mach_vm_deferred_reclamation_buffer_allocate");
    }
}
