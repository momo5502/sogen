#include "../std_include.hpp"
#include "macos_layer_tree.hpp"

#include "macos_layer_compositor.hpp"
#include "macos_sdf_field.hpp"
#include "macos_ui_state.hpp"
#include "../macos_emulator.hpp"

#include <algorithm>
#include <cmath>
#include <set>

namespace sogen
{
    namespace
    {
        // Measured on 25G76 against CGColorGetNumberOfComponents / CGColorGetComponents, on eight live
        // colours under lldb and again on every colour of a real window's layer tree
        // (src/tools/macos-gui-probe/layerdump.m checks its own arithmetic against CoreGraphics).
        constexpr uint64_t CGCOLOR_COMPONENT_COUNT_OFFSET = 0x38;
        constexpr uint64_t CGCOLOR_COMPONENTS_OFFSET = 0x48;
        constexpr uint64_t CGCOLOR_MAX_COMPONENTS = 8;

        // Same CFConstantString form skylight_routines.cpp reads window titles out of: +0x08 info word,
        // +0x10 UTF-8 pointer, +0x18 length. Every kCAGravity* constant is one.
        constexpr uint64_t CF_CONSTANT_STRING_INFO = 0x7c8;
        constexpr size_t CF_CONSTANT_STRING_MAX_LENGTH = 64;

        // Measured on 25G76 against CGPathApply over rects, rounded rects and ellipses at several
        // sizes and with a creation transform folded in: +0x10 is a kind tag, +0x18 the six doubles of
        // the affine that maps the unit shape, +0x48/+0x50 a rounded rect's corner radii as fractions
        // of its width and height. Kinds 8 and 9 are CoreGraphics' element lists.
        constexpr uint64_t CGPATH_KIND_OFFSET = 0x10;
        constexpr uint64_t CGPATH_TRANSFORM_OFFSET = 0x18;
        constexpr uint64_t CGPATH_CORNER_OFFSET = 0x48;
        constexpr uint64_t CGPATH_KIND_RECT = 1;
        constexpr uint64_t CGPATH_KIND_ROUNDED_RECT = 2;
        constexpr uint64_t CGPATH_KIND_ELLIPSE = 4;

        // The two element-list forms. Measured on 25G76 against CGPathApply over ten paths, the glyph
        // outlines of the operator keys among them (src/tools/macos-gui-probe/pathprobe.c):
        //
        //   kind 8, inline: point count u16 at +0x18, element count u16 at +0x1a, the element types
        //     packed three bits each least-significant-first in a u32 at +0x1c, the points as pairs of
        //     doubles from +0x20.
        //   kind 9, on the heap: point count at +0x18, element count at +0x20, the points as pairs of
        //     doubles from the start of the buffer at +0x30, and the element types as bytes *descending*
        //     from the offset at +0x28 -- type[i] is buffer[offset - 1 - i].
        //
        // Both counts include one point per close: CoreGraphics stores the subpath's start point again
        // so a close needs no back-reference.
        constexpr uint64_t CGPATH_KIND_ELEMENTS_INLINE = 8;
        constexpr uint64_t CGPATH_KIND_ELEMENTS_HEAP = 9;
        constexpr uint64_t CGPATH_INLINE_COUNTS_OFFSET = 0x18;
        constexpr uint64_t CGPATH_INLINE_TYPES_OFFSET = 0x1c;
        constexpr uint64_t CGPATH_INLINE_POINTS_OFFSET = 0x20;
        constexpr uint64_t CGPATH_HEAP_POINT_COUNT_OFFSET = 0x18;
        constexpr uint64_t CGPATH_HEAP_ELEMENT_COUNT_OFFSET = 0x20;
        constexpr uint64_t CGPATH_HEAP_TYPES_END_OFFSET = 0x28;
        constexpr uint64_t CGPATH_HEAP_POINTS_OFFSET = 0x30;

        constexpr uint8_t CGPATH_ELEMENT_MOVE = 0;
        constexpr uint8_t CGPATH_ELEMENT_LINE = 1;
        constexpr uint8_t CGPATH_ELEMENT_QUAD = 2;
        constexpr uint8_t CGPATH_ELEMENT_CURVE = 3;
        constexpr uint8_t CGPATH_ELEMENT_CLOSE = 4;

        // CASDFFillEffect's only ivar; section 15 of the layer-tree spec lists the whole family.
        constexpr uint64_t CASDF_FILL_EFFECT_COLOR_OFFSET = 0x08;

        constexpr uint64_t OBJC_ARRAY_MAX_COUNT = 4096;
        constexpr uint64_t OBJC_POINTER_MASK = 0xFFFF000000000000ULL;

        // The array classes and their storage layouts are measured in
 //. There is no single NSArray
        // layout to read: AppKit hands -[CALayer setSublayers:] a private Foundation class or a bridged
        // Swift array, so the class has to be named before its storage can be read.
        constexpr uint64_t OBJC_ISA_MASK = 0x0000000FFFFFFFF8ULL;

        // Class metadata is reached through pointers the arm64e runtime signs in place. The signature
        // sits above the 47-bit user address, so masking it off is both the strip and a no-op for a
        // pointer that carries none. Measured: Calculator's array class holds ro_or_rw_ext as
        // 0x004d0001ee6b4a40 for the class_ro_t at 0x1ee6b4a40.
        constexpr uint64_t OBJC_POINTER_STRIP_MASK = 0x00007FFFFFFFFFFFULL;
        constexpr uint64_t OBJC_CLASS_BITS_OFFSET = 0x20;
        constexpr uint64_t OBJC_CLASS_DATA_MASK = 0x00007FFFFFFFFFF8ULL;
        constexpr uint64_t OBJC_CLASS_IS_SWIFT_STABLE = 0x2;
        constexpr uint64_t OBJC_CLASS_RW_RO_OFFSET = 0x08;
        constexpr uint32_t OBJC_CLASS_RW_REALIZED = 0x80000000u;
        constexpr uint64_t OBJC_CLASS_RO_NAME_OFFSET = 0x18;
        constexpr uint64_t SWIFT_CLASS_DESCRIPTOR_OFFSET = 0x40;
        constexpr uint64_t SWIFT_DESCRIPTOR_NAME_OFFSET = 0x08;
        constexpr size_t OBJC_CLASS_NAME_MAX_LENGTH = 128;

        constexpr uint64_t OBJC_ARRAY_I_COUNT_OFFSET = 0x08;
        constexpr uint64_t OBJC_ARRAY_I_OBJECTS_OFFSET = 0x10;
        constexpr uint64_t OBJC_SINGLE_OBJECT_ARRAY_OFFSET = 0x08;
        constexpr uint64_t OBJC_ARRAY_M_LIST_OFFSET = 0x10;
        constexpr uint64_t OBJC_ARRAY_M_OFFSET_AND_SIZE_OFFSET = 0x18;
        constexpr uint64_t OBJC_ARRAY_M_USED_AND_MUTATIONS_OFFSET = 0x20;
        constexpr uint64_t SWIFT_ARRAY_COUNT_OFFSET = 0x10;
        constexpr uint64_t SWIFT_ARRAY_CAPACITY_AND_FLAGS_OFFSET = 0x18;
        constexpr uint64_t SWIFT_ARRAY_ELEMENTS_OFFSET = 0x20;
        constexpr uint64_t SWIFT_DEFERRED_ARRAY_NATIVE_OFFSET = 0x18;

        constexpr uint32_t ARM64_LDR_X16_LITERAL_12 = 0x58000070;
        constexpr uint32_t ARM64_BR_X16 = 0xD61F0200;
        constexpr uint32_t ARM64_NOP = 0xD503201F;
        constexpr uint32_t ARM64_BRK_0 = 0xD4200000;

        constexpr size_t MACOS_LAYER_MAX_CONTENTS_REPORTS = 8;

        struct layer_state
        {
            macos_layer_tree tree{};
            std::map<uint64_t, uint64_t> trampolines{};
            std::set<std::string> reported{};
            uint64_t trampoline_page{};
            size_t trampoline_slots{};
            size_t trampolines_used{};
            size_t contents_reports{};
            std::vector<uint8_t> scratch{};
        };

        std::map<macos_emulator*, layer_state>& registry()
        {
            static std::map<macos_emulator*, layer_state> instances{};
            return instances;
        }

        layer_state& state_of(macos_emulator& emu)
        {
            return registry()[&emu];
        }

        void report_once(macos_emulator& emu, const std::string& key, const std::string& message)
        {
            auto& state = state_of(emu);
            if (state.reported.insert(key).second)
            {
                emu.log.warn("%s\n", message.c_str());
            }
        }

        std::string method_label(const macos_native_call& call)
        {
            return std::string{call.name};
        }

        // Hands control to the real implementation. macos_native_dispatch::invoke only forces pc back to
        // the link register when the handler left it at entry + 4, so writing pc here is what turns an
        // interception into an observation: every register the caller set, the FP bank included, is
        // still exactly as the method expects it.
        void continue_into_original(const macos_native_call& call)
        {
            auto& state = state_of(call.emu_ref);
            const auto found = state.trampolines.find(call.entry);
            if (found == state.trampolines.end())
            {
                report_once(call.emu_ref, "no-trampoline:" + method_label(call),
                            method_label(call) + " was intercepted without a pass-through trampoline; the real implementation "
                                                 "did not run and CoreAnimation state is now wrong");
                return;
            }

            call.emu.reg(arm64_register::pc, found->second);
        }

        macos_layer_tree& tree_of(const macos_native_call& call)
        {
            return state_of(call.emu_ref).tree;
        }

        macos_layer_rect rect_argument(const macos_native_call& call)
        {
            return {call.arg_double(0), call.arg_double(1), call.arg_double(2), call.arg_double(3)};
        }

        macos_layer_point point_argument(const macos_native_call& call)
        {
            return {call.arg_double(0), call.arg_double(1)};
        }

        bool bool_argument(const macos_native_call& call)
        {
            return (call.arg(2) & 0xFFu) != 0;
        }

        std::optional<std::array<double, 6>> read_affine(macos_emulator& emu, const uint64_t address)
        {
            if (address == 0)
            {
                return std::nullopt;
            }

            std::array<double, 6> values{};
            if (!emu.memory.try_read_memory(address, values.data(), sizeof(values)))
            {
                return std::nullopt;
            }

            return values;
        }

        // CATransform3D is 16 doubles in row order; the affine part is m11, m12, m21, m22, m41, m42.
        // Measured: CATransform3DMakeScale(2,3,4) reads back [2,0,0,0, 0,3,0,0, 0,0,4,0, 0,0,0,1] and
        // MakeTranslation(5,6,7) puts 5, 6 and 7 at indices 12, 13 and 14.
        std::optional<std::array<double, 16>> read_transform3d(macos_emulator& emu, const uint64_t address)
        {
            if (address == 0)
            {
                return std::nullopt;
            }

            std::array<double, 16> values{};
            if (!emu.memory.try_read_memory(address, values.data(), sizeof(values)))
            {
                return std::nullopt;
            }

            return values;
        }

        std::optional<std::string> read_constant_string(macos_emulator& emu, const uint64_t address)
        {
            if (address == 0)
            {
                return std::nullopt;
            }

            std::array<uint64_t, 4> fields{};
            if (!emu.memory.try_read_memory(address, fields.data(), sizeof(fields)))
            {
                return std::nullopt;
            }

            if (fields[1] != CF_CONSTANT_STRING_INFO || fields[3] > CF_CONSTANT_STRING_MAX_LENGTH)
            {
                return std::nullopt;
            }

            if (fields[3] == 0)
            {
                return std::string{};
            }

            if (fields[2] == 0)
            {
                return std::nullopt;
            }

            std::string text(static_cast<size_t>(fields[3]), '\0');
            if (!emu.memory.try_read_memory(fields[2], text.data(), text.size()))
            {
                return std::nullopt;
            }

            return text;
        }

        bool plausible_object(const uint64_t value)
        {
            return value != 0 && (value & 7u) == 0 && (value & OBJC_POINTER_MASK) == 0;
        }

        std::optional<uint64_t> read_word(macos_emulator& emu, const uint64_t address)
        {
            uint64_t value = 0;
            if (!emu.memory.try_read_memory(address, &value, sizeof(value)))
            {
                return std::nullopt;
            }

            return value;
        }

        std::optional<uint32_t> read_half(macos_emulator& emu, const uint64_t address)
        {
            uint32_t value = 0;
            if (!emu.memory.try_read_memory(address, &value, sizeof(value)))
            {
                return std::nullopt;
            }

            return value;
        }

        std::optional<std::string> read_c_string(macos_emulator& emu, const uint64_t address, const size_t limit)
        {
            std::string text{};
            for (size_t index = 0; index < limit; ++index)
            {
                char character = 0;
                if (!emu.memory.try_read_memory(address + index, &character, sizeof(character)))
                {
                    return std::nullopt;
                }

                if (character == 0)
                {
                    return text;
                }

                text.push_back(character);
            }

            return std::nullopt;
        }

        // A Swift class carries its unmangled name in a type descriptor that is readable before the ObjC
        // runtime realizes the class, and a generic specialisation names the generic type rather than the
        // specialisation, so a bridged Swift array of any element type answers _ContiguousArrayStorage.
        // The ObjC name of the same class is only readable once realized, and the first setSublayers: of
        // a run arrives before that.
        //
        // arm64e signs the isa whether or not the nonpointer bit is set, and the signature occupies the
        // bits above the class pointer, so keying the mask off that bit -- which is what the two isa
        // layouts read like -- hands back a signature as part of the address for every object that
        // stores a raw one. A Swift class instance is exactly that case. Masking unconditionally is a
        // no-op on a genuine raw pointer, which cannot leave the 36-bit user address space.
        std::optional<std::string> read_class_name(macos_emulator& emu, const uint64_t object)
        {
            if (!plausible_object(object))
            {
                return std::nullopt;
            }

            const auto isa = read_word(emu, object);
            if (!isa)
            {
                return std::nullopt;
            }

            const auto class_object = *isa & OBJC_ISA_MASK;
            if (!plausible_object(class_object))
            {
                return std::nullopt;
            }

            const auto bits = read_word(emu, class_object + OBJC_CLASS_BITS_OFFSET);
            if (!bits)
            {
                return std::nullopt;
            }

            if ((*bits & OBJC_CLASS_IS_SWIFT_STABLE) != 0)
            {
                const auto descriptor = read_word(emu, class_object + SWIFT_CLASS_DESCRIPTOR_OFFSET);
                if (!descriptor || (*descriptor & OBJC_POINTER_STRIP_MASK) == 0)
                {
                    return std::nullopt;
                }

                const auto name_field = (*descriptor & OBJC_POINTER_STRIP_MASK) + SWIFT_DESCRIPTOR_NAME_OFFSET;
                const auto relative = read_half(emu, name_field);
                if (!relative)
                {
                    return std::nullopt;
                }

                return read_c_string(emu, name_field + static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(*relative))),
                                     OBJC_CLASS_NAME_MAX_LENGTH);
            }

            const auto data = *bits & OBJC_CLASS_DATA_MASK;
            if (data == 0)
            {
                return std::nullopt;
            }

            auto read_only = data;
            const auto flags = read_half(emu, data);
            if (!flags)
            {
                return std::nullopt;
            }

            if ((*flags & OBJC_CLASS_RW_REALIZED) != 0)
            {
                const auto ro_or_extension = read_word(emu, data + OBJC_CLASS_RW_RO_OFFSET);
                if (!ro_or_extension)
                {
                    return std::nullopt;
                }

                const auto stripped = *ro_or_extension & OBJC_POINTER_STRIP_MASK;
                if ((*ro_or_extension & 1) != 0)
                {
                    const auto indirect = read_word(emu, stripped & ~3ULL);
                    if (!indirect)
                    {
                        return std::nullopt;
                    }

                    read_only = *indirect & OBJC_POINTER_STRIP_MASK;
                }
                else
                {
                    read_only = stripped;
                }
            }

            const auto name = read_word(emu, read_only + OBJC_CLASS_RO_NAME_OFFSET);
            if (!name || (*name & OBJC_POINTER_STRIP_MASK) == 0)
            {
                return std::nullopt;
            }

            return read_c_string(emu, *name & OBJC_POINTER_STRIP_MASK, OBJC_CLASS_NAME_MAX_LENGTH);
        }

        struct object_array_storage
        {
            uint64_t elements{};
            uint64_t count{};
        };

        std::optional<object_array_storage> locate_array_storage(macos_emulator& emu, uint64_t array, const std::string& class_name);

        std::optional<object_array_storage> locate_array_storage(macos_emulator& emu, const uint64_t array, const std::string& class_name)
        {
            if (class_name == "__NSArray0" || class_name == "__EmptyArrayStorage")
            {
                return object_array_storage{};
            }

            if (class_name == "__NSSingleObjectArrayI")
            {
                return object_array_storage{array + OBJC_SINGLE_OBJECT_ARRAY_OFFSET, 1};
            }

            if (class_name == "__NSArrayI" || class_name == "__NSArrayI_Transfer")
            {
                const auto count = read_word(emu, array + OBJC_ARRAY_I_COUNT_OFFSET);
                if (!count)
                {
                    return std::nullopt;
                }

                if (class_name == "__NSArrayI")
                {
                    return object_array_storage{array + OBJC_ARRAY_I_OBJECTS_OFFSET, *count};
                }

                const auto list = read_word(emu, array + OBJC_ARRAY_I_OBJECTS_OFFSET);
                if (!list || (*count != 0 && *list == 0))
                {
                    return std::nullopt;
                }

                return object_array_storage{*list, *count};
            }

            // A deque: the elements start _offset slots into _list rather than at its head.
            if (class_name == "__NSArrayM" || class_name == "__NSFrozenArrayM")
            {
                const auto list = read_word(emu, array + OBJC_ARRAY_M_LIST_OFFSET);
                const auto offset_and_size = read_word(emu, array + OBJC_ARRAY_M_OFFSET_AND_SIZE_OFFSET);
                const auto used_and_mutations = read_word(emu, array + OBJC_ARRAY_M_USED_AND_MUTATIONS_OFFSET);
                if (!list || !offset_and_size || !used_and_mutations)
                {
                    return std::nullopt;
                }

                const auto offset = *offset_and_size & 0xFFFFFFFFULL;
                const auto size = *offset_and_size >> 32;
                const auto used = *used_and_mutations >> 32;

                if (used == 0)
                {
                    return object_array_storage{};
                }

                if (*list == 0 || offset + used > size)
                {
                    return std::nullopt;
                }

                return object_array_storage{*list + offset * sizeof(uint64_t), used};
            }

            // A Swift Array bridged to NSArray lazily: the elements stay in the native storage until
            // something asks for an NSArray buffer, and _heapBufferBridged (+0x10) is nil until then, so
            // the deferred array has to be followed to _nativeStorage (+0x18) to be read at all.
            if (class_name == "__SwiftDeferredNSArray")
            {
                const auto native = read_word(emu, array + SWIFT_DEFERRED_ARRAY_NATIVE_OFFSET);
                if (!native || !plausible_object(*native))
                {
                    return std::nullopt;
                }

                const auto native_name = read_class_name(emu, *native);
                if (!native_name || *native_name == "__SwiftDeferredNSArray")
                {
                    return std::nullopt;
                }

                return locate_array_storage(emu, *native, *native_name);
            }

            if (class_name == "_ContiguousArrayStorage")
            {
                const auto count = read_word(emu, array + SWIFT_ARRAY_COUNT_OFFSET);
                const auto capacity_and_flags = read_word(emu, array + SWIFT_ARRAY_CAPACITY_AND_FLAGS_OFFSET);
                if (!count || !capacity_and_flags)
                {
                    return std::nullopt;
                }

                if (*count == 0)
                {
                    return object_array_storage{};
                }

                if ((*capacity_and_flags >> 1) < *count)
                {
                    return std::nullopt;
                }

                return object_array_storage{array + SWIFT_ARRAY_ELEMENTS_OFFSET, *count};
            }

            return std::nullopt;
        }

        std::optional<std::vector<uint64_t>> read_object_array(macos_emulator& emu, const uint64_t array, std::string& class_name)
        {
            if (array == 0)
            {
                return std::vector<uint64_t>{};
            }

            const auto name = read_class_name(emu, array);
            if (!name)
            {
                return std::nullopt;
            }

            class_name = *name;

            const auto storage = locate_array_storage(emu, array, class_name);
            if (!storage || storage->count > OBJC_ARRAY_MAX_COUNT)
            {
                return std::nullopt;
            }

            if (storage->count == 0)
            {
                return std::vector<uint64_t>{};
            }

            std::vector<uint64_t> objects(static_cast<size_t>(storage->count), 0);
            if (!emu.memory.try_read_memory(storage->elements, objects.data(), objects.size() * sizeof(uint64_t)))
            {
                return std::nullopt;
            }

            if (!std::ranges::all_of(objects, plausible_object))
            {
                return std::nullopt;
            }

            return objects;
        }

        bool is_pc_relative(const uint32_t instruction)
        {
            if ((instruction & 0x7C000000u) == 0x14000000u)
            {
                return true;
            }

            if ((instruction & 0xFF000010u) == 0x54000000u)
            {
                return true;
            }

            if ((instruction & 0x7E000000u) == 0x34000000u || (instruction & 0x7E000000u) == 0x36000000u)
            {
                return true;
            }

            if ((instruction & 0x1F000000u) == 0x10000000u)
            {
                return true;
            }

            return (instruction & 0x3B000000u) == 0x18000000u;
        }

        void note_unmodelled_contents(macos_emulator& emu, const uint64_t layer, const uint64_t object)
        {
            auto& state = state_of(emu);
            if (state.contents_reports >= MACOS_LAYER_MAX_CONTENTS_REPORTS)
            {
                return;
            }

            ++state.contents_reports;
            emu.log.warn("CALayer contents 0x%" PRIx64 " on layer 0x%" PRIx64
                         " has no raster sogen can read; the layer will composite without it\n",
                         object, layer);

            if (state.contents_reports == MACOS_LAYER_MAX_CONTENTS_REPORTS)
            {
                emu.log.warn("Further unreadable CALayer contents are counted in the compositor statistics rather than logged\n");
            }
        }

        void layer_set_bounds(const macos_native_call& call)
        {
            tree_of(call).touch(call.arg(0)).bounds = rect_argument(call).standardized();
            continue_into_original(call);
        }

        void layer_set_frame(const macos_native_call& call)
        {
            if (!tree_of(call).set_frame(call.arg(0), rect_argument(call)))
            {
                report_once(call.emu_ref, "frame-with-shear",
                            "-[CALayer setFrame:] was called on a layer whose transform has off-diagonal terms; "
                            "CoreAnimation leaves that case undefined and sogen used the diagonal form");
            }

            continue_into_original(call);
        }

        void layer_set_position(const macos_native_call& call)
        {
            tree_of(call).touch(call.arg(0)).position = point_argument(call);
            continue_into_original(call);
        }

        void layer_set_anchor_point(const macos_native_call& call)
        {
            tree_of(call).touch(call.arg(0)).anchor_point = point_argument(call);
            continue_into_original(call);
        }

        void layer_set_contents_rect(const macos_native_call& call)
        {
            tree_of(call).touch(call.arg(0)).contents_rect = rect_argument(call).standardized();
            continue_into_original(call);
        }

        void layer_set_opacity(const macos_native_call& call)
        {
            tree_of(call).touch(call.arg(0)).opacity = static_cast<double>(call.arg_float(0));
            continue_into_original(call);
        }

        void layer_set_corner_radius(const macos_native_call& call)
        {
            tree_of(call).touch(call.arg(0)).corner_radius = call.arg_double(0);
            continue_into_original(call);
        }

        void layer_set_border_width(const macos_native_call& call)
        {
            tree_of(call).touch(call.arg(0)).border_width = call.arg_double(0);
            continue_into_original(call);
        }

        void layer_set_contents_scale(const macos_native_call& call)
        {
            tree_of(call).touch(call.arg(0)).contents_scale = call.arg_double(0);
            continue_into_original(call);
        }

        void layer_set_z_position(const macos_native_call& call)
        {
            tree_of(call).touch(call.arg(0)).z_position = call.arg_double(0);
            continue_into_original(call);
        }

        void layer_set_hidden(const macos_native_call& call)
        {
            tree_of(call).touch(call.arg(0)).hidden = bool_argument(call);
            continue_into_original(call);
        }

        void layer_set_masks_to_bounds(const macos_native_call& call)
        {
            tree_of(call).touch(call.arg(0)).masks_to_bounds = bool_argument(call);
            continue_into_original(call);
        }

        void layer_set_geometry_flipped(const macos_native_call& call)
        {
            tree_of(call).touch(call.arg(0)).geometry_flipped = bool_argument(call);
            continue_into_original(call);
        }

        void layer_set_background_color(const macos_native_call& call)
        {
            tree_of(call).touch(call.arg(0)).background = macos_layer_read_color(call.emu_ref, call.arg(2));
            continue_into_original(call);
        }

        void layer_set_border_color(const macos_native_call& call)
        {
            tree_of(call).touch(call.arg(0)).border = macos_layer_read_color(call.emu_ref, call.arg(2));
            continue_into_original(call);
        }

        // The object is recorded, not classified. Every CF-backed contents kind -- CGImage and
        // CABackingStore both -- reports as __NSCFType through the ObjC runtime, so reading a class name
        // out of guest memory would cost a realized-class walk and still not tell the two apart.
        void layer_set_contents(const macos_native_call& call)
        {
            const auto layer = call.arg(0);
            const auto object = call.arg(2);
            auto& node = tree_of(call).touch(layer);

            if (object == 0)
            {
                node.contents = {};
                continue_into_original(call);
                return;
            }

            if (node.contents.kind == macos_layer_contents_kind::raster && node.contents.object == object)
            {
                continue_into_original(call);
                return;
            }

            node.contents = macos_layer_contents{
                .kind = macos_layer_contents_kind::unresolved,
                .object = object,
            };

            note_unmodelled_contents(call.emu_ref, layer, object);
            continue_into_original(call);
        }

        // Where a redraw becomes visible. CoreAnimation draws into the CABackingStore a layer already
        // holds instead of handing CALayer a new contents object, so nothing else on the setter surface
        // fires and the raster taken from the store on an earlier frame would be blitted forever.
        // -[CALayer setNeedsDisplay] cannot be trampolined on 25G76 -- its first instruction is
        // pc-relative -- and it is the wrong moment anyway: display is where the new pixels land.
        void layer_display(const macos_native_call& call)
        {
            auto& tree = tree_of(call);
            const auto* node = tree.find(call.arg(0));

            if (node != nullptr && node->contents.kind == macos_layer_contents_kind::raster)
            {
                call.emu_ref.ui.contents.forget(call.emu_ref, tree, node->contents.object);
            }

            continue_into_original(call);
        }

        void layer_set_contents_gravity(const macos_native_call& call)
        {
            const auto layer = call.arg(0);
            const auto name = read_constant_string(call.emu_ref, call.arg(2));
            if (!name)
            {
                report_once(call.emu_ref, "gravity-string",
                            "-[CALayer setContentsGravity:] was handed a CFString sogen cannot read; the layer keeps its "
                            "previous gravity");
                continue_into_original(call);
                return;
            }

            const auto gravity = macos_layer_gravity_from_name(*name);
            if (!gravity)
            {
                report_once(call.emu_ref, "gravity-name:" + *name,
                            "contentsGravity \"" + *name + "\" is not a CoreAnimation gravity sogen models");
                continue_into_original(call);
                return;
            }

            tree_of(call).touch(layer).gravity = *gravity;
            continue_into_original(call);
        }

        void layer_set_affine_transform(const macos_native_call& call)
        {
            const auto values = read_affine(call.emu_ref, call.arg(2));
            if (!values)
            {
                report_once(call.emu_ref, "affine-unreadable",
                            "-[CALayer setAffineTransform:] pointed at memory sogen could not read; the layer keeps its "
                            "previous transform");
                continue_into_original(call);
                return;
            }

            auto& node = tree_of(call).touch(call.arg(0));
            node.transform = {(*values)[0], (*values)[1], (*values)[2], (*values)[3], (*values)[4], (*values)[5]};
            continue_into_original(call);
        }

        macos_layer_affine affine_of(macos_emulator& emu, const std::array<double, 16>& matrix, const char* setter)
        {
            if (matrix[3] != 0.0 || matrix[7] != 0.0 || matrix[11] != 0.0)
            {
                report_once(emu, std::string{"transform3d-perspective:"} + setter,
                            std::string{setter} + " carried a perspective row, which sogen's compositor ignores");
            }

            return {matrix[0], matrix[1], matrix[4], matrix[5], matrix[12], matrix[13]};
        }

        void layer_set_transform(const macos_native_call& call)
        {
            const auto values = read_transform3d(call.emu_ref, call.arg(2));
            if (!values)
            {
                report_once(call.emu_ref, "transform-unreadable",
                            "-[CALayer setTransform:] pointed at memory sogen could not read; the layer keeps its previous "
                            "transform");
                continue_into_original(call);
                return;
            }

            tree_of(call).touch(call.arg(0)).transform = affine_of(call.emu_ref, *values, "-[CALayer setTransform:]");
            continue_into_original(call);
        }

        void layer_set_sublayer_transform(const macos_native_call& call)
        {
            const auto values = read_transform3d(call.emu_ref, call.arg(2));
            if (!values)
            {
                report_once(call.emu_ref, "sublayer-transform-unreadable",
                            "-[CALayer setSublayerTransform:] pointed at memory sogen could not read; the layer keeps its "
                            "previous sublayer transform");
                continue_into_original(call);
                return;
            }

            tree_of(call).touch(call.arg(0)).sublayer_transform = affine_of(call.emu_ref, *values, "-[CALayer setSublayerTransform:]");
            continue_into_original(call);
        }

        void layer_add_sublayer(const macos_native_call& call)
        {
            tree_of(call).add_sublayer(call.arg(0), call.arg(2));
            continue_into_original(call);
        }

        void layer_insert_sublayer_at_index(const macos_native_call& call)
        {
            tree_of(call).insert_sublayer(call.arg(0), call.arg(2), static_cast<size_t>(static_cast<uint32_t>(call.arg(3))));
            continue_into_original(call);
        }

        void layer_insert_sublayer_below(const macos_native_call& call)
        {
            tree_of(call).insert_sublayer_relative(call.arg(0), call.arg(2), call.arg(3), false);
            continue_into_original(call);
        }

        void layer_insert_sublayer_above(const macos_native_call& call)
        {
            tree_of(call).insert_sublayer_relative(call.arg(0), call.arg(2), call.arg(3), true);
            continue_into_original(call);
        }

        void layer_set_sublayers(const macos_native_call& call)
        {
            std::string class_name{};
            const auto children = read_object_array(call.emu_ref, call.arg(2), class_name);
            if (!children)
            {
                const auto named = class_name.empty() ? std::string{"an unnamed class"} : "a " + class_name;
                report_once(call.emu_ref, "sublayers-array:" + class_name,
                            "-[CALayer setSublayers:] was handed " + named +
                                ", whose storage sogen cannot enumerate; the layer keeps its previous sublayers");
                continue_into_original(call);
                return;
            }

            tree_of(call).replace_sublayers(call.arg(0), *children);
            continue_into_original(call);
        }

        void layer_remove_from_superlayer(const macos_native_call& call)
        {
            tree_of(call).remove_from_superlayer(call.arg(0));
            continue_into_original(call);
        }

        void layer_set_mask(const macos_native_call& call)
        {
            tree_of(call).set_mask(call.arg(0), call.arg(2));
            continue_into_original(call);
        }

        void layer_set_compositing_filter(const macos_native_call& call)
        {
            tree_of(call).touch(call.arg(0)).compositing_filter = call.arg(2);

            if (call.arg(2) != 0)
            {
                report_once(call.emu_ref, "layer-compositing-filter",
                            "-[CALayer setCompositingFilter:] set a filter, which sogen's compositor does not apply; the "
                            "layer will composite source-over");
            }

            continue_into_original(call);
        }

        void layer_set_shadow_opacity(const macos_native_call& call)
        {
            tree_of(call).touch(call.arg(0)).shadow_opacity = static_cast<double>(call.arg_float(0));

            if (call.arg_float(0) != 0.0f)
            {
                report_once(call.emu_ref, "layer-shadow",
                            "-[CALayer setShadowOpacity:] asked for a shadow, which sogen's compositor does not draw");
            }

            continue_into_original(call);
        }

        // Measured: AppKit and SwiftUI always pass NO, which is what makes a per-layer opacity multiply
        // correct. A layer that turns it on needs its subtree rendered offscreen first, which sogen does
        // not do.
        void layer_set_allows_group_opacity(const macos_native_call& call)
        {
            tree_of(call).touch(call.arg(0)).allows_group_opacity = bool_argument(call);

            if (bool_argument(call))
            {
                report_once(call.emu_ref, "group-opacity",
                            "-[CALayer setAllowsGroupOpacity:] was set to YES; sogen composites opacity per layer, so a "
                            "translucent group will show its overlaps");
            }

            continue_into_original(call);
        }

        void sdf_set_effect(const macos_native_call& call)
        {
            auto& sdf = tree_of(call).touch(call.arg(0)).sdf;
            sdf.effect = call.arg(2);
            sdf.effect_class.clear();
            sdf.effect_kind = macos_sdf_effect_kind::none;
            sdf.effect_color = {};

            if (call.arg(2) != 0)
            {
                if (const auto name = read_class_name(call.emu_ref, call.arg(2)))
                {
                    sdf.effect_class = *name;
                    sdf.effect_kind = macos_sdf_effect_kind_from_class_name(*name).value_or(macos_sdf_effect_kind::unmodelled);
                }
                else
                {
                    sdf.effect_kind = macos_sdf_effect_kind::unmodelled;
                }

                // CASDFFillEffect keeps its CGColor in the first ivar; measured offsets in
                if (sdf.effect_kind == macos_sdf_effect_kind::fill)
                {
                    if (const auto colour = read_word(call.emu_ref, call.arg(2) + CASDF_FILL_EFFECT_COLOR_OFFSET))
                    {
                        sdf.effect_color = macos_layer_read_color(call.emu_ref, *colour);
                    }
                }
            }

            continue_into_original(call);
        }

        void sdf_set_smoothness(const macos_native_call& call)
        {
            tree_of(call).touch(call.arg(0)).sdf.smoothness = call.arg_double(0);
            continue_into_original(call);
        }

        void sdf_set_gaussian_radius(const macos_native_call& call)
        {
            tree_of(call).touch(call.arg(0)).sdf.gaussian_radius = call.arg_double(0);
            continue_into_original(call);
        }

        void sdf_set_effect_offset(const macos_native_call& call)
        {
            tree_of(call).touch(call.arg(0)).sdf.effect_offset = call.arg_double(0);
            continue_into_original(call);
        }

        void sdf_set_merge_elements(const macos_native_call& call)
        {
            tree_of(call).touch(call.arg(0)).sdf.merge_elements = bool_argument(call);
            continue_into_original(call);
        }

        // mode and operation are NSStrings, not integers -- the rasteriser's contract, section 1.
        void sdf_element_set_mode(const macos_native_call& call)
        {
            const auto name = read_constant_string(call.emu_ref, call.arg(2));
            if (name)
            {
                if (const auto mode = macos_sdf_mode_from_name(*name))
                {
                    tree_of(call).touch(call.arg(0)).sdf.mode = *mode;
                }
                else
                {
                    report_once(call.emu_ref, "sdf-mode:" + *name,
                                "-[CASDFElementLayer setMode:] used \"" + *name + "\", which sogen's field rasteriser does not model");
                }
            }

            continue_into_original(call);
        }

        void sdf_element_set_operation(const macos_native_call& call)
        {
            const auto name = read_constant_string(call.emu_ref, call.arg(2));
            auto& sdf = tree_of(call).touch(call.arg(0)).sdf;
            sdf.operation = name ? macos_sdf_operation_from_name(*name) : std::nullopt;

            if (name && !sdf.operation)
            {
                report_once(call.emu_ref, "sdf-operation:" + *name,
                            "-[CASDFElementLayer setOperation:] used \"" + *name +
                                "\", which sogen's field rasteriser does not model; the element is left out of the field");
            }

            continue_into_original(call);
        }

        void sdf_element_set_zero_distance(const macos_native_call& call)
        {
            tree_of(call).touch(call.arg(0)).sdf.contents_zero_value_distance = call.arg_double(0);
            continue_into_original(call);
        }

        void sdf_element_set_one_distance(const macos_native_call& call)
        {
            tree_of(call).touch(call.arg(0)).sdf.contents_one_value_distance = call.arg_double(0);
            continue_into_original(call);
        }

        void sdf_element_set_ovalization(const macos_native_call& call)
        {
            tree_of(call).touch(call.arg(0)).sdf.gradient_ovalization = call.arg_double(0);
            continue_into_original(call);
        }

        void portal_set_source_layer(const macos_native_call& call)
        {
            tree_of(call).touch(call.arg(0)).portal_source = call.arg(2);
            continue_into_original(call);
        }

        void portal_set_hides_source_layer(const macos_native_call& call)
        {
            tree_of(call).touch(call.arg(0)).hides_source_layer = bool_argument(call);
            continue_into_original(call);
        }

        void shape_set_path(const macos_native_call& call)
        {
            auto& node = tree_of(call).touch(call.arg(0));
            auto measured = macos_layer_read_shape_path(call.emu_ref, call.arg(2));
            node.shape.kind = measured.kind;
            node.shape.transform = measured.transform;
            node.shape.corner_x = measured.corner_x;
            node.shape.corner_y = measured.corner_y;
            node.shape.edges = std::move(measured.edges);

            if (measured.kind == macos_layer_shape_kind::unmodelled)
            {
                report_once(call.emu_ref, "shape-path-elements",
                            "-[CAShapeLayer setPath:] was handed a path sogen does not decode; it rasterises the rect, "
                            "rounded-rect and ellipse forms and both element-list forms, so the shape will not be drawn");
            }

            continue_into_original(call);
        }

        void shape_set_fill_color(const macos_native_call& call)
        {
            tree_of(call).touch(call.arg(0)).shape.fill = macos_layer_read_color(call.emu_ref, call.arg(2));
            continue_into_original(call);
        }

        void shape_set_stroke_color(const macos_native_call& call)
        {
            tree_of(call).touch(call.arg(0)).shape.stroke = macos_layer_read_color(call.emu_ref, call.arg(2));
            continue_into_original(call);
        }

        void shape_set_line_width(const macos_native_call& call)
        {
            tree_of(call).touch(call.arg(0)).shape.line_width = call.arg_double(0);
            continue_into_original(call);
        }

        void shape_set_fill_rule(const macos_native_call& call)
        {
            const auto name = read_constant_string(call.emu_ref, call.arg(2));
            tree_of(call).touch(call.arg(0)).shape.even_odd = name && *name == "even-odd";
            continue_into_original(call);
        }

        void portal_set_matches_position(const macos_native_call& call)
        {
            tree_of(call).touch(call.arg(0)).portal_matches_position = bool_argument(call);
            continue_into_original(call);
        }

        void context_set_layer(const macos_native_call& call)
        {
            tree_of(call).set_context_root(call.arg(0), call.arg(2));
            continue_into_original(call);
        }

        void transaction_flush(const macos_native_call& call)
        {
            tree_of(call).note_flush();
            macos_layer_tree_present(call.emu_ref);
            continue_into_original(call);
        }

        bool read_pixels(void* context, const uint64_t address, void* destination, const size_t size)
        {
            return static_cast<macos_emulator*>(context)->memory.try_read_memory(address, destination, size);
        }
    }

    macos_layer_rect macos_layer_rect::standardized() const
    {
        macos_layer_rect result{this->x, this->y, this->width, this->height};

        if (!std::isfinite(result.x) || !std::isfinite(result.y) || !std::isfinite(result.width) || !std::isfinite(result.height))
        {
            return {};
        }

        if (result.width < 0.0)
        {
            result.x += result.width;
            result.width = -result.width;
        }

        if (result.height < 0.0)
        {
            result.y += result.height;
            result.height = -result.height;
        }

        return result;
    }

    bool macos_layer_rect::empty() const
    {
        return !(this->width > 0.0) || !(this->height > 0.0);
    }

    bool macos_layer_affine::is_identity() const
    {
        return this->a == 1.0 && this->b == 0.0 && this->c == 0.0 && this->d == 1.0 && this->tx == 0.0 && this->ty == 0.0;
    }

    macos_layer_point macos_layer_affine::apply(const macos_layer_point point) const
    {
        return {
            this->a * point.x + this->c * point.y + this->tx,
            this->b * point.x + this->d * point.y + this->ty,
        };
    }

    macos_layer_affine macos_layer_affine::then(const macos_layer_affine& outer) const
    {
        return {
            outer.a * this->a + outer.c * this->b,
            outer.b * this->a + outer.d * this->b,
            outer.a * this->c + outer.c * this->d,
            outer.b * this->c + outer.d * this->d,
            outer.a * this->tx + outer.c * this->ty + outer.tx,
            outer.b * this->tx + outer.d * this->ty + outer.ty,
        };
    }

    std::optional<macos_layer_affine> macos_layer_affine::inverse() const
    {
        const auto determinant = this->a * this->d - this->b * this->c;
        if (!std::isfinite(determinant) || determinant == 0.0)
        {
            return std::nullopt;
        }

        macos_layer_affine result{};
        result.a = this->d / determinant;
        result.b = -this->b / determinant;
        result.c = -this->c / determinant;
        result.d = this->a / determinant;
        result.tx = (this->c * this->ty - this->d * this->tx) / determinant;
        result.ty = (this->b * this->tx - this->a * this->ty) / determinant;

        if (!std::isfinite(result.a) || !std::isfinite(result.b) || !std::isfinite(result.c) || !std::isfinite(result.d) ||
            !std::isfinite(result.tx) || !std::isfinite(result.ty))
        {
            return std::nullopt;
        }

        return result;
    }

    macos_layer_affine macos_layer_affine::translation(const double tx, const double ty)
    {
        return {1.0, 0.0, 0.0, 1.0, tx, ty};
    }

    macos_layer_affine macos_layer_affine::scaling(const double sx, const double sy)
    {
        return {sx, 0.0, 0.0, sy, 0.0, 0.0};
    }

    bool macos_layer_raster::valid() const
    {
        return this->pixels != 0 && this->width != 0 && this->height != 0 && this->stride != 0;
    }

    std::optional<macos_layer_gravity> macos_layer_gravity_from_name(const std::string_view name)
    {
        static const std::map<std::string_view, macos_layer_gravity> names{
            {"center", macos_layer_gravity::center},
            {"top", macos_layer_gravity::top},
            {"bottom", macos_layer_gravity::bottom},
            {"left", macos_layer_gravity::left},
            {"right", macos_layer_gravity::right},
            {"topLeft", macos_layer_gravity::top_left},
            {"topRight", macos_layer_gravity::top_right},
            {"bottomLeft", macos_layer_gravity::bottom_left},
            {"bottomRight", macos_layer_gravity::bottom_right},
            {"resize", macos_layer_gravity::resize},
            {"resizeAspect", macos_layer_gravity::resize_aspect},
            {"resizeAspectFill", macos_layer_gravity::resize_aspect_fill},
        };

        const auto found = names.find(name);
        return found == names.end() ? std::nullopt : std::optional{found->second};
    }

    macos_layer_point macos_layer_node::anchor_in_bounds() const
    {
        const auto rect = this->bounds.standardized();
        return {
            rect.x + this->anchor_point.x * rect.width,
            rect.y + this->anchor_point.y * rect.height,
        };
    }

    macos_layer_affine macos_layer_node::to_superlayer() const
    {
        const auto anchor = this->anchor_in_bounds();
        const auto rect = this->bounds.standardized();

        auto mapping = macos_layer_affine{};
        if (this->geometry_flipped)
        {
            mapping = macos_layer_affine{1.0, 0.0, 0.0, -1.0, 0.0, 2.0 * rect.y + rect.height};
        }

        return mapping.then(macos_layer_affine::translation(-anchor.x, -anchor.y))
            .then(this->transform)
            .then(macos_layer_affine::translation(this->position.x, this->position.y));
    }

    macos_layer_affine macos_layer_node::sublayer_mapping() const
    {
        if (this->sublayer_transform.is_identity())
        {
            return {};
        }

        const auto anchor = this->anchor_in_bounds();
        return macos_layer_affine::translation(-anchor.x, -anchor.y)
            .then(this->sublayer_transform)
            .then(macos_layer_affine::translation(anchor.x, anchor.y));
    }

    macos_layer_node& macos_layer_tree::touch(const uint64_t layer)
    {
        auto& node = this->nodes_[layer];
        node.id = layer;
        return node;
    }

    const macos_layer_node* macos_layer_tree::find(const uint64_t layer) const
    {
        const auto found = this->nodes_.find(layer);
        return found == this->nodes_.end() ? nullptr : &found->second;
    }

    macos_layer_node* macos_layer_tree::find(const uint64_t layer)
    {
        const auto found = this->nodes_.find(layer);
        return found == this->nodes_.end() ? nullptr : &found->second;
    }

    void macos_layer_tree::detach(macos_layer_node& child)
    {
        if (child.parent == 0)
        {
            return;
        }

        if (auto* parent = this->find(child.parent))
        {
            std::erase(parent->children, child.id);

            if (parent->mask == child.id)
            {
                parent->mask = 0;
            }
        }

        child.parent = 0;
    }

    void macos_layer_tree::set_mask(const uint64_t layer, const uint64_t mask)
    {
        if (layer == 0 || layer == mask)
        {
            return;
        }

        auto& node = this->touch(layer);
        const auto previous = node.mask;

        if (previous != 0 && previous != mask)
        {
            if (auto* old = this->find(previous))
            {
                old->parent = 0;
            }
        }

        node.mask = mask;

        if (mask == 0)
        {
            return;
        }

        auto& shape = this->touch(mask);
        this->detach(shape);
        this->nodes_[layer].mask = mask;
        this->nodes_[mask].parent = layer;
    }

    void macos_layer_tree::add_sublayer(const uint64_t parent, const uint64_t child)
    {
        if (parent == 0 || child == 0 || parent == child)
        {
            return;
        }

        this->touch(parent);
        auto& node = this->touch(child);
        this->detach(node);

        this->nodes_[parent].children.push_back(child);
        this->nodes_[child].parent = parent;
    }

    void macos_layer_tree::insert_sublayer(const uint64_t parent, const uint64_t child, const size_t index)
    {
        if (parent == 0 || child == 0 || parent == child)
        {
            return;
        }

        this->touch(parent);
        auto& node = this->touch(child);
        this->detach(node);

        auto& children = this->nodes_[parent].children;
        const auto at = std::min(index, children.size());
        children.insert(children.begin() + static_cast<std::ptrdiff_t>(at), child);
        this->nodes_[child].parent = parent;
    }

    void macos_layer_tree::insert_sublayer_relative(const uint64_t parent, const uint64_t child, const uint64_t sibling, const bool above)
    {
        if (parent == 0 || child == 0 || parent == child)
        {
            return;
        }

        this->touch(parent);
        this->touch(child);

        auto& children = this->nodes_[parent].children;
        const auto found = std::ranges::find(children, sibling);
        const auto index = found == children.end() ? (above ? children.size() : 0) : static_cast<size_t>(found - children.begin());
        this->insert_sublayer(parent, child, above ? index + 1 : index);
    }

    void macos_layer_tree::replace_sublayers(const uint64_t parent, const std::vector<uint64_t>& children)
    {
        if (parent == 0)
        {
            return;
        }

        this->touch(parent);

        const auto previous = this->nodes_[parent].children;
        for (const auto child : previous)
        {
            if (auto* node = this->find(child))
            {
                node->parent = 0;
            }
        }

        this->nodes_[parent].children.clear();

        for (const auto child : children)
        {
            if (child == 0 || child == parent)
            {
                continue;
            }

            auto& node = this->touch(child);
            this->detach(node);
            this->nodes_[parent].children.push_back(child);
            this->nodes_[child].parent = parent;
        }
    }

    void macos_layer_tree::remove_from_superlayer(const uint64_t child)
    {
        if (auto* node = this->find(child))
        {
            this->detach(*node);
        }
    }

    bool macos_layer_tree::set_frame(const uint64_t layer, const macos_layer_rect frame)
    {
        auto& node = this->touch(layer);
        const auto rect = frame.standardized();

        const auto diagonal = node.transform.b == 0.0 && node.transform.c == 0.0;
        const auto sx = std::abs(node.transform.a);
        const auto sy = std::abs(node.transform.d);

        node.bounds.width = sx > 0.0 && std::isfinite(sx) ? rect.width / sx : rect.width;
        node.bounds.height = sy > 0.0 && std::isfinite(sy) ? rect.height / sy : rect.height;
        node.position = {
            rect.x + node.anchor_point.x * rect.width,
            rect.y + node.anchor_point.y * rect.height,
        };

        return diagonal;
    }

    void macos_layer_tree::attach_contents_raster(const uint64_t layer, const macos_layer_raster& raster)
    {
        auto& node = this->touch(layer);
        node.contents.kind = raster.valid() ? macos_layer_contents_kind::raster : macos_layer_contents_kind::none;
        node.contents.raster = raster;
    }

    void macos_layer_tree::discard_contents_raster(const uint64_t layer)
    {
        auto* node = this->find(layer);
        if (node == nullptr || node->contents.kind != macos_layer_contents_kind::raster)
        {
            return;
        }

        node->contents.kind = macos_layer_contents_kind::unresolved;
        node->contents.raster = {};
    }

    void macos_layer_tree::set_context_root(const uint64_t context, const uint64_t layer)
    {
        if (context == 0)
        {
            return;
        }

        if (layer == 0)
        {
            this->context_roots_.erase(context);
            return;
        }

        this->touch(layer);
        this->context_roots_[context] = layer;
    }

    uint64_t macos_layer_tree::root_for_context(const uint64_t context) const
    {
        const auto found = this->context_roots_.find(context);
        return found == this->context_roots_.end() ? 0 : found->second;
    }

    void macos_layer_tree::clear()
    {
        this->nodes_.clear();
        this->context_roots_.clear();
        this->flush_count_ = 0;
    }

    macos_layer_tree& macos_layer_tree_of(macos_emulator& emu)
    {
        return state_of(emu).tree;
    }

    void macos_layer_tree_release(macos_emulator& emu)
    {
        registry().erase(&emu);
    }

    macos_layer_color macos_layer_read_color(macos_emulator& emu, const uint64_t color)
    {
        if (color == 0)
        {
            return {};
        }

        uint64_t count = 0;
        if (!emu.memory.try_read_memory(color + CGCOLOR_COMPONENT_COUNT_OFFSET, &count, sizeof(count)))
        {
            report_once(emu, "cgcolor-unreadable", "A CGColor handed to a CALayer setter could not be read out of guest memory");
            return {};
        }

        if (count == 0 || count > CGCOLOR_MAX_COMPONENTS)
        {
            report_once(emu, "cgcolor-count:" + std::to_string(count),
                        "A CGColor reported " + std::to_string(count) + " components, which sogen does not decode");
            return {};
        }

        std::array<double, CGCOLOR_MAX_COMPONENTS> components{};
        if (!emu.memory.try_read_memory(color + CGCOLOR_COMPONENTS_OFFSET, components.data(), sizeof(double) * count))
        {
            report_once(emu, "cgcolor-unreadable", "A CGColor handed to a CALayer setter could not be read out of guest memory");
            return {};
        }

        if (count == 2)
        {
            return {true, components[0], components[0], components[0], components[1]};
        }

        if (count == 4)
        {
            return {true, components[0], components[1], components[2], components[3]};
        }

        report_once(emu, "cgcolor-count:" + std::to_string(count),
                    "A CGColor reported " + std::to_string(count) + " components, which sogen does not decode");
        return {};
    }

    namespace
    {
        size_t cgpath_points_for(const uint8_t type)
        {
            switch (type)
            {
            case CGPATH_ELEMENT_MOVE:
            case CGPATH_ELEMENT_LINE:
            case CGPATH_ELEMENT_CLOSE:
                return 1;
            case CGPATH_ELEMENT_QUAD:
                return 2;
            case CGPATH_ELEMENT_CURVE:
                return 3;
            default:
                return 0;
            }
        }

        // Enough subdivisions that the flattened curve stays inside half a device pixel at the sizes a
        // glyph outline is drawn at, without making the segment count depend on a transform the tree
        // does not have. The control polygon's length is the only scale information available here.
        size_t cgpath_curve_steps(const std::span<const macos_layer_point> control)
        {
            double length = 0.0;
            for (size_t index = 1; index < control.size(); ++index)
            {
                length += std::hypot(control[index].x - control[index - 1].x, control[index].y - control[index - 1].y);
            }

            return static_cast<size_t>(std::clamp(std::ceil(length), 4.0, 32.0));
        }

        void cgpath_flatten_quad(std::vector<macos_layer_path_edge>& edges, const macos_layer_point from, const macos_layer_point control,
                                 const macos_layer_point to)
        {
            const std::array<macos_layer_point, 3> polygon{from, control, to};
            const auto steps = cgpath_curve_steps(polygon);

            auto previous = from;
            for (size_t step = 1; step <= steps; ++step)
            {
                const auto t = static_cast<double>(step) / static_cast<double>(steps);
                const auto u = 1.0 - t;
                const macos_layer_point at{
                    u * u * from.x + 2.0 * u * t * control.x + t * t * to.x,
                    u * u * from.y + 2.0 * u * t * control.y + t * t * to.y,
                };

                edges.push_back({previous, at});
                previous = at;
            }
        }

        void cgpath_flatten_cubic(std::vector<macos_layer_path_edge>& edges, const macos_layer_point from, const macos_layer_point first,
                                  const macos_layer_point second, const macos_layer_point to)
        {
            const std::array<macos_layer_point, 4> polygon{from, first, second, to};
            const auto steps = cgpath_curve_steps(polygon);

            auto previous = from;
            for (size_t step = 1; step <= steps; ++step)
            {
                const auto t = static_cast<double>(step) / static_cast<double>(steps);
                const auto u = 1.0 - t;
                const macos_layer_point at{
                    u * u * u * from.x + 3.0 * u * u * t * first.x + 3.0 * u * t * t * second.x + t * t * t * to.x,
                    u * u * u * from.y + 3.0 * u * u * t * first.y + 3.0 * u * t * t * second.y + t * t * t * to.y,
                };

                edges.push_back({previous, at});
                previous = at;
            }
        }

        // CoreGraphics fills an open subpath as though it were closed, so every subpath is closed here
        // whether the element list said so or not.
        std::optional<std::vector<macos_layer_path_edge>> cgpath_flatten(const std::span<const uint8_t> types,
                                                                         const std::span<const macos_layer_point> points)
        {
            std::vector<macos_layer_path_edge> edges{};
            macos_layer_point current{};
            macos_layer_point start{};
            bool open = false;
            size_t next = 0;

            const auto close_subpath = [&] {
                if (open && (current.x != start.x || current.y != start.y))
                {
                    edges.push_back({current, start});
                }

                open = false;
            };

            for (const auto type : types)
            {
                const auto needed = cgpath_points_for(type);
                if (needed == 0 || next + needed > points.size() || edges.size() > MACOS_LAYER_MAX_PATH_EDGES)
                {
                    return std::nullopt;
                }

                switch (type)
                {
                case CGPATH_ELEMENT_MOVE:
                    close_subpath();
                    current = points[next];
                    start = current;
                    open = true;
                    break;

                case CGPATH_ELEMENT_LINE:
                    edges.push_back({current, points[next]});
                    current = points[next];
                    break;

                case CGPATH_ELEMENT_QUAD:
                    cgpath_flatten_quad(edges, current, points[next], points[next + 1]);
                    current = points[next + 1];
                    break;

                case CGPATH_ELEMENT_CURVE:
                    cgpath_flatten_cubic(edges, current, points[next], points[next + 1], points[next + 2]);
                    current = points[next + 2];
                    break;

                case CGPATH_ELEMENT_CLOSE:
                    close_subpath();
                    current = points[next];
                    start = current;
                    break;

                default:
                    return std::nullopt;
                }

                next += needed;
            }

            close_subpath();

            if (edges.empty() || edges.size() > MACOS_LAYER_MAX_PATH_EDGES)
            {
                return std::nullopt;
            }

            return edges;
        }

        bool cgpath_read_inline(macos_emulator& emu, const uint64_t path, std::vector<uint8_t>& types,
                                std::vector<macos_layer_point>& points)
        {
            std::array<uint16_t, 2> counts{};
            uint32_t packed = 0;
            if (!emu.memory.try_read_memory(path + CGPATH_INLINE_COUNTS_OFFSET, counts.data(), sizeof(counts)) ||
                !emu.memory.try_read_memory(path + CGPATH_INLINE_TYPES_OFFSET, &packed, sizeof(packed)))
            {
                return false;
            }

            const size_t point_count = counts[0];
            const size_t element_count = counts[1];

            // Ten element types is all a u32 holds at three bits each, and the inline form is only ever
            // chosen for a path that fits.
            if (element_count == 0 || element_count > 10 || point_count == 0 || point_count > 32)
            {
                return false;
            }

            types.resize(element_count);
            for (size_t index = 0; index < element_count; ++index)
            {
                types[index] = static_cast<uint8_t>((packed >> (index * 3)) & 0x7u);
            }

            points.resize(point_count);
            return emu.memory.try_read_memory(path + CGPATH_INLINE_POINTS_OFFSET, points.data(), point_count * sizeof(macos_layer_point));
        }

        bool cgpath_read_heap(macos_emulator& emu, const uint64_t path, std::vector<uint8_t>& types, std::vector<macos_layer_point>& points)
        {
            const auto point_count = read_word(emu, path + CGPATH_HEAP_POINT_COUNT_OFFSET);
            const auto element_count = read_word(emu, path + CGPATH_HEAP_ELEMENT_COUNT_OFFSET);
            const auto types_end = read_word(emu, path + CGPATH_HEAP_TYPES_END_OFFSET);
            const auto buffer = read_word(emu, path + CGPATH_HEAP_POINTS_OFFSET);

            if (!point_count || !element_count || !types_end || !buffer || *buffer == 0)
            {
                return false;
            }

            if (*element_count == 0 || *element_count > MACOS_LAYER_MAX_PATH_EDGES || *point_count == 0 ||
                *point_count > MACOS_LAYER_MAX_PATH_EDGES || *types_end < *element_count ||
                *types_end < *point_count * sizeof(macos_layer_point))
            {
                return false;
            }

            points.resize(*point_count);
            if (!emu.memory.try_read_memory(*buffer, points.data(), points.size() * sizeof(macos_layer_point)))
            {
                return false;
            }

            types.resize(*element_count);
            if (!emu.memory.try_read_memory(*buffer + *types_end - *element_count, types.data(), types.size()))
            {
                return false;
            }

            std::ranges::reverse(types);
            return true;
        }
    }

    macos_layer_shape macos_layer_read_shape_path(macos_emulator& emu, const uint64_t path)
    {
        macos_layer_shape shape{};
        if (path == 0)
        {
            return shape;
        }

        const auto kind = read_word(emu, path + CGPATH_KIND_OFFSET);
        if (!kind)
        {
            return shape;
        }

        std::array<double, 6> matrix{};
        if (!emu.memory.try_read_memory(path + CGPATH_TRANSFORM_OFFSET, matrix.data(), sizeof(matrix)))
        {
            return shape;
        }

        shape.transform = {matrix[0], matrix[1], matrix[2], matrix[3], matrix[4], matrix[5]};

        switch (*kind)
        {
        case CGPATH_KIND_RECT:
            shape.kind = macos_layer_shape_kind::rect;
            return shape;

        case CGPATH_KIND_ELLIPSE:
            shape.kind = macos_layer_shape_kind::ellipse;
            return shape;

        case CGPATH_KIND_ROUNDED_RECT: {
            std::array<double, 2> corner{};
            if (!emu.memory.try_read_memory(path + CGPATH_CORNER_OFFSET, corner.data(), sizeof(corner)))
            {
                shape.kind = macos_layer_shape_kind::unmodelled;
                return shape;
            }

            shape.kind = macos_layer_shape_kind::rounded_rect;
            shape.corner_x = corner[0];
            shape.corner_y = corner[1];
            return shape;
        }

        case CGPATH_KIND_ELEMENTS_INLINE:
        case CGPATH_KIND_ELEMENTS_HEAP: {
            // The element forms carry no transform of their own: a path built through one is stored in
            // absolute coordinates, with any creation transform already folded into the points.
            shape.transform = {};

            std::vector<uint8_t> types{};
            std::vector<macos_layer_point> points{};
            const auto read = *kind == CGPATH_KIND_ELEMENTS_INLINE ? cgpath_read_inline(emu, path, types, points)
                                                                   : cgpath_read_heap(emu, path, types, points);

            auto edges = read ? cgpath_flatten(types, points) : std::nullopt;
            if (!edges)
            {
                shape.kind = macos_layer_shape_kind::unmodelled;
                return shape;
            }

            shape.kind = macos_layer_shape_kind::path;
            shape.edges = std::move(*edges);
            return shape;
        }

        default:
            shape.kind = macos_layer_shape_kind::unmodelled;
            shape.transform = {};
            return shape;
        }
    }

    std::optional<std::string> macos_layer_read_class_name(macos_emulator& emu, const uint64_t object)
    {
        return read_class_name(emu, object);
    }

    std::vector<macos_objc_method> macos_layer_tree_methods()
    {
        const std::string quartz{MACOS_QUARTZ_CORE_IMAGE_PATH};

        const auto layer = [&](const char* selector, const macos_native_handler handler) {
            return macos_objc_method{quartz, "CALayer", selector, false, handler};
        };

        return {
            layer("setBounds:", layer_set_bounds),
            layer("setFrame:", layer_set_frame),
            layer("setPosition:", layer_set_position),
            layer("setAnchorPoint:", layer_set_anchor_point),
            layer("setContentsRect:", layer_set_contents_rect),
            layer("setOpacity:", layer_set_opacity),
            layer("setCornerRadius:", layer_set_corner_radius),
            layer("setBorderWidth:", layer_set_border_width),
            layer("setContentsScale:", layer_set_contents_scale),
            layer("setZPosition:", layer_set_z_position),
            layer("setHidden:", layer_set_hidden),
            layer("setMasksToBounds:", layer_set_masks_to_bounds),
            layer("setGeometryFlipped:", layer_set_geometry_flipped),
            layer("setBackgroundColor:", layer_set_background_color),
            layer("setBorderColor:", layer_set_border_color),
            layer("setContents:", layer_set_contents),
            layer("setContentsGravity:", layer_set_contents_gravity),
            layer("display", layer_display),
            layer("setAffineTransform:", layer_set_affine_transform),
            layer("setTransform:", layer_set_transform),
            layer("setSublayerTransform:", layer_set_sublayer_transform),
            layer("addSublayer:", layer_add_sublayer),
            layer("insertSublayer:atIndex:", layer_insert_sublayer_at_index),
            layer("insertSublayer:below:", layer_insert_sublayer_below),
            layer("insertSublayer:above:", layer_insert_sublayer_above),
            layer("setSublayers:", layer_set_sublayers),
            layer("removeFromSuperlayer", layer_remove_from_superlayer),
            layer("setMask:", layer_set_mask),
            layer("setCompositingFilter:", layer_set_compositing_filter),
            layer("setShadowOpacity:", layer_set_shadow_opacity),
            layer("setAllowsGroupOpacity:", layer_set_allows_group_opacity),
            macos_objc_method{quartz, "CAShapeLayer", "setPath:", false, shape_set_path},
            macos_objc_method{quartz, "CAShapeLayer", "setFillColor:", false, shape_set_fill_color},
            macos_objc_method{quartz, "CAShapeLayer", "setStrokeColor:", false, shape_set_stroke_color},
            macos_objc_method{quartz, "CAShapeLayer", "setLineWidth:", false, shape_set_line_width},
            macos_objc_method{quartz, "CAShapeLayer", "setFillRule:", false, shape_set_fill_rule},
            macos_objc_method{quartz, "CASDFLayer", "setEffect:", false, sdf_set_effect},
            macos_objc_method{quartz, "CASDFLayer", "setSmoothness:", false, sdf_set_smoothness},
            macos_objc_method{quartz, "CASDFLayer", "setGaussianRadius:", false, sdf_set_gaussian_radius},
            macos_objc_method{quartz, "CASDFLayer", "setEffectOffset:", false, sdf_set_effect_offset},
            macos_objc_method{quartz, "CASDFLayer", "setMergeElements:", false, sdf_set_merge_elements},
            macos_objc_method{quartz, "CASDFElementLayer", "setMode:", false, sdf_element_set_mode},
            macos_objc_method{quartz, "CASDFElementLayer", "setOperation:", false, sdf_element_set_operation},
            macos_objc_method{quartz, "CASDFElementLayer", "setContentsZeroValueDistance:", false, sdf_element_set_zero_distance},
            macos_objc_method{quartz, "CASDFElementLayer", "setContentsOneValueDistance:", false, sdf_element_set_one_distance},
            macos_objc_method{quartz, "CASDFElementLayer", "setGradientOvalization:", false, sdf_element_set_ovalization},
            macos_objc_method{quartz, "CAPortalLayer", "setSourceLayer:", false, portal_set_source_layer},
            macos_objc_method{quartz, "CAPortalLayer", "setHidesSourceLayer:", false, portal_set_hides_source_layer},
            macos_objc_method{quartz, "CAPortalLayer", "setMatchesPosition:", false, portal_set_matches_position},
            macos_objc_method{quartz, "CAContext", "setLayer:", false, context_set_layer},
            macos_objc_method{quartz, "CATransaction", "flush", true, transaction_flush},
        };
    }

    bool macos_layer_tree_continue_into_original(const macos_native_call& call)
    {
        auto& state = state_of(call.emu_ref);
        const auto found = state.trampolines.find(call.entry);
        if (found == state.trampolines.end())
        {
            return false;
        }

        call.emu.reg(arm64_register::pc, found->second);
        return true;
    }

    bool macos_layer_tree_reinstall_export(macos_emulator& emu, const dyld_shared_cache_reader& cache, macos_native_dispatch& dispatch,
                                           const uint64_t entry, std::string name, const macos_native_handler handler)
    {
        if (entry == 0)
        {
            return false;
        }

        uint32_t pristine = 0;
        try
        {
            const auto bytes = cache.read_at_address(entry, sizeof(pristine));
            if (bytes.size() != sizeof(pristine))
            {
                throw std::runtime_error{"short read"};
            }

            std::memcpy(&pristine, bytes.data(), sizeof(pristine));
        }
        catch (const std::exception& e)
        {
            emu.log.warn("%s stayed replaced: its original instruction is not readable from the shared cache (%s)\n", name.c_str(),
                         e.what());
            return false;
        }

        if (!emu.memory.try_write_memory(entry, &pristine, sizeof(pristine)))
        {
            emu.log.warn("%s stayed replaced: its original instruction could not be restored\n", name.c_str());
            return false;
        }

        return macos_layer_tree_install(emu, dispatch, entry, std::move(name), handler);
    }

    bool macos_layer_tree_install(macos_emulator& emu, macos_native_dispatch& dispatch, const uint64_t imp, std::string name,
                                  const macos_native_handler handler)
    {
        if (handler == nullptr || imp == 0 || (imp & 3u) != 0)
        {
            return false;
        }

        auto& state = state_of(emu);

        if (state.trampoline_page == 0)
        {
            const auto bytes = MACOS_PAGE_SIZE;
            if (!emu.memory.allocate_memory(MACOS_LAYER_TRAMPOLINE_BASE, bytes, memory_permission::read_exec))
            {
                emu.log.warn("Unable to map the CA layer trampoline page at 0x%" PRIx64 "\n", MACOS_LAYER_TRAMPOLINE_BASE);
                return false;
            }

            const std::vector<uint32_t> filler(bytes / sizeof(uint32_t), ARM64_BRK_0);
            if (!emu.memory.try_write_memory(MACOS_LAYER_TRAMPOLINE_BASE, filler.data(), bytes))
            {
                return false;
            }

            state.trampoline_page = MACOS_LAYER_TRAMPOLINE_BASE;
            state.trampoline_slots = bytes / MACOS_LAYER_TRAMPOLINE_SLOT;
            state.trampolines_used = 0;
        }

        if (state.trampolines_used >= state.trampoline_slots)
        {
            emu.log.warn("CA layer interception ran out of trampoline slots before %s\n", name.c_str());
            return false;
        }

        uint32_t displaced = 0;
        if (!emu.memory.try_read_memory(imp, &displaced, sizeof(displaced)))
        {
            emu.log.warn("CA layer interception could not read the first instruction of %s\n", name.c_str());
            return false;
        }

        if (displaced == MACOS_ARM64_SVC_80)
        {
            emu.log.warn("CA layer interception found %s already trapped; refusing to relocate the trap\n", name.c_str());
            return false;
        }

        if (is_pc_relative(displaced))
        {
            emu.log.warn("CA layer method %s cannot be observed without replacing it: its first instruction 0x%08x is "
                         "pc-relative and cannot be relocated into a trampoline\n",
                         name.c_str(), displaced);
            return false;
        }

        // A far branch, not a `b`: the trampoline page is more than 128 MiB from the shared cache. x16 is
        // IP0, which AAPCS64 reserves for veneers like this one, and at a method's first instruction it
        // holds nothing but the dead copy of the IMP objc_msgSend loaded. Landing on imp + 4 would trip
        // BTI on real hardware; unicorn does not enforce it and the trampoline never leaves the emulator.
        const auto slot = state.trampoline_page + state.trampolines_used * MACOS_LAYER_TRAMPOLINE_SLOT;
        const std::array<uint32_t, 4> code{displaced, ARM64_LDR_X16_LITERAL_12, ARM64_BR_X16, ARM64_NOP};
        const uint64_t resume = imp + 4;

        if (!emu.memory.try_write_memory(slot, code.data(), sizeof(code)) ||
            !emu.memory.try_write_memory(slot + 0x10, &resume, sizeof(resume)))
        {
            emu.log.warn("CA layer interception could not write the trampoline for %s\n", name.c_str());
            return false;
        }

        if (!patch_native_entry(emu, imp))
        {
            emu.log.warn("CA layer interception could not install the trap for %s\n", name.c_str());
            return false;
        }

        ++state.trampolines_used;
        state.trampolines[imp] = slot;
        dispatch.bind_entry(imp, std::move(name), handler);
        return true;
    }

    std::vector<macos_layer_tree_binding> macos_layer_tree_bind(macos_emulator& emu, const dyld_shared_cache_reader& cache,
                                                                const macos_cache_symbols& symbols, macos_native_dispatch& dispatch)
    {
        // The registry is keyed by emulator address, and a freed emulator's address can come back for a
        // new one. Binding happens once per emulator before anything else touches the tree, which makes
        // it the one point where inherited state can be dropped.
        macos_layer_tree_release(emu);

        const auto bindings = macos_objc_bind_with_pass_through(emu, cache, symbols, dispatch, macos_layer_tree_methods());

        size_t observed = 0;
        for (const auto& binding : bindings)
        {
            observed += binding.observed ? 1 : 0;
        }

        emu.log.info("CA layer tree: %zu of %zu methods observed\n", observed, bindings.size());
        return bindings;
    }

    std::vector<macos_layer_tree_binding> macos_objc_bind_with_pass_through(macos_emulator& emu, const dyld_shared_cache_reader& cache,
                                                                            const macos_cache_symbols& symbols,
                                                                            macos_native_dispatch& dispatch,
                                                                            const std::vector<macos_objc_method>& methods)
    {
        const auto resolved = bind_objc_methods(emu, cache, symbols, dispatch, methods);

        std::vector<macos_layer_tree_binding> bindings{};
        bindings.reserve(resolved.size());

        for (size_t index = 0; index < resolved.size(); ++index)
        {
            macos_layer_tree_binding binding{.name = resolved[index].name, .imp = resolved[index].imp};

            if (!resolved[index].bound)
            {
                binding.refusal = "not resolved in the shared cache";
                bindings.push_back(std::move(binding));
                continue;
            }

            // bind_objc_methods has already written the trap over the method's first instruction, so the
            // original word is only still available in the cache file. Putting it back before deciding
            // whether a trampoline is possible means a method sogen cannot observe is left working
            // rather than left broken.
            uint32_t pristine = 0;
            try
            {
                const auto bytes = cache.read_at_address(resolved[index].imp, sizeof(pristine));
                if (bytes.size() != sizeof(pristine))
                {
                    throw std::runtime_error{"short read"};
                }

                std::memcpy(&pristine, bytes.data(), sizeof(pristine));
            }
            catch (const std::exception& e)
            {
                emu.log.warn("CA layer method %s stayed patched: its original instruction is not readable from the shared "
                             "cache (%s), so the real implementation can no longer run\n",
                             binding.name.c_str(), e.what());
                binding.refusal = "original instruction unreadable";
                bindings.push_back(std::move(binding));
                continue;
            }

            if (!emu.memory.try_write_memory(resolved[index].imp, &pristine, sizeof(pristine)))
            {
                emu.log.warn("CA layer method %s stayed patched: its original instruction could not be restored\n", binding.name.c_str());
                binding.refusal = "original instruction not restorable";
                bindings.push_back(std::move(binding));
                continue;
            }

            if (!macos_layer_tree_install(emu, dispatch, resolved[index].imp, binding.name, methods[index].handler))
            {
                binding.refusal = "no pass-through trampoline";
                bindings.push_back(std::move(binding));
                continue;
            }

            binding.trampoline = state_of(emu).trampolines[resolved[index].imp];
            binding.observed = true;
            bindings.push_back(std::move(binding));
        }

        return bindings;
    }

    bool macos_layer_tree_resolve_contents(macos_emulator& emu)
    {
        return emu.ui.contents.resolve_one(emu, state_of(emu).tree);
    }

    // The compositor has no way to reach guest memory, so which CoreAnimation class a layer is has to
    // be decided here. A layer's class never changes, so this runs once per layer; an id whose object
    // has been freed and its memory reused simply resolves to whatever is there now, which is why a
    // failed read is remembered as `plain` rather than retried every frame.
    namespace
    {
        void resolve_roles(macos_emulator& emu, macos_layer_tree& tree)
        {
            std::vector<uint64_t> pending{};
            for (const auto& [id, node] : tree.nodes())
            {
                if (!node.role_resolved)
                {
                    pending.push_back(id);
                }
            }

            for (const auto id : pending)
            {
                auto* node = tree.find(id);
                if (node == nullptr)
                {
                    continue;
                }

                node->role_resolved = true;

                const auto name = read_class_name(emu, id);
                if (!name)
                {
                    continue;
                }

                if (*name == "SDFLayer")
                {
                    node->role = macos_layer_role::sdf_group;
                }
                else if (*name == "CASDFLayer")
                {
                    node->role = macos_layer_role::sdf_container;
                }
                else if (*name == "CASDFElementLayer")
                {
                    node->role = macos_layer_role::sdf_element;
                }
                else if (*name == "CABackdropLayer")
                {
                    node->role = macos_layer_role::backdrop;
                }
            }
        }
    }

    size_t macos_layer_tree_present(macos_emulator& emu)
    {
        auto& state = state_of(emu);
        resolve_roles(emu, state.tree);
        auto& ui = emu.ui;
        size_t presented = 0;

        for (auto& window : ui.server.windows())
        {
            if (window.layer_context == 0)
            {
                continue;
            }

            const auto root = state.tree.root_for_context(window.layer_context);
            if (root == 0 || state.tree.find(root) == nullptr)
            {
                continue;
            }

            if (!ui.ensure_backing_store(emu, window))
            {
                continue;
            }

            const auto bytes = window.backing_bytes();
            if (bytes == 0)
            {
                continue;
            }

            state.scratch.assign(bytes, 0);

            const macos_layer_surface surface{
                state.scratch.data(),
                window.width,
                window.height,
                static_cast<int32_t>(window.backing_stride),
            };

            const macos_layer_pixel_source source{read_pixels, &emu};
            const auto stats = macos_layer_composite(state.tree, root, surface, {}, source);

            emu.log.info("window %u composited %zu of %zu layers reachable from root 0x%" PRIx64 " of %zu in the tree, into %dx%d: %zu "
                         "pixels, %zu contents blitted, %zu unresolved, %zu unreadable, %zu masks applied, %zu masks skipped, "
                         "%zu portal layers not modelled, %zu shapes filled, %zu shape paths not modelled, %zu SDF fields drawn "
                         "(%zu as the measured flat-glass approximation), %zu not modelled, %zu SDF elements\n",
                         window.id, stats.layers_drawn, stats.layers_visited, root, state.tree.size(), window.width, window.height,
                         stats.pixels_written, stats.contents_blits, stats.contents_unresolved, stats.contents_unreadable,
                         stats.masks_applied, stats.masks_skipped, stats.portals_unmodelled, stats.shapes_filled, stats.shapes_unmodelled,
                         stats.sdf_fields_drawn, stats.sdf_fields_approximated, stats.sdf_fields_unmodelled, stats.sdf_elements_used);

            if (stats.pixels_written == 0)
            {
                static std::set<uint32_t> reported{};
                if (reported.insert(window.id).second)
                {
                    emu.log.warn("window %u composited %zu of the %zu layers sogen has recorded and wrote no pixels; the window will "
                                 "present as fully transparent\n",
                                 window.id, stats.layers_visited, state.tree.size());
                }
            }

            if (!emu.memory.try_write_memory(window.backing_address, state.scratch.data(), state.scratch.size()))
            {
                continue;
            }

            if (ui.present(emu, window, window.rect()))
            {
                ++presented;
            }
        }

        return presented;
    }
}
