#include <gtest/gtest.h>

#include "macos_test_utils.hpp"

#include <gui/macos_layer_contents.hpp>
#include <gui/macos_layer_tree.hpp>
#include <gui/macos_native_dispatch.hpp>
#include <gui/skylight_routines.hpp>
#include <screenshot_ui_backend.hpp>

#include <cstring>
#include <vector>

namespace
{
    using sogen::macos_layer_contents_kind;
    using sogen::macos_layer_contents_resolver;
    using sogen::macos_layer_tree;

    constexpr uint64_t code_base = 0x100000000ULL;
    constexpr uint64_t commit_base = 0x100004000ULL;

    constexpr uint64_t stub_base = 0x100100000ULL;
    constexpr uint64_t stub_stride = 0x1000ULL;

    constexpr uint64_t object_base = 0x100200000ULL;
    constexpr uint64_t counter_base = 0x100300000ULL;
    constexpr uint64_t objc_base = 0x100400000ULL;

    constexpr uint64_t fake_colorspace = 0x4321ULL;

    // A CGImage's type id is whatever CoreGraphics registered this boot; 110 and 112 are what
    // CGImageGetTypeID and CABackingStoreGetTypeID answered on 25G76, and 1 is what CFGetTypeID
    // answers for an ObjC instance that is not a CoreFoundation type -- CATintedImage and
    // NSViewBackingLayerContents both measure that way.
    constexpr uint64_t image_type_id = 110;
    constexpr uint64_t backing_store_type_id = 112;
    constexpr uint64_t plain_objc_type_id = 1;

    // The offsets CGImageGetWidth and CGImageGetHeight load from, read out of the real
    // CoreGraphics on 25G76: `cbz x0, ret ; ldr x0, [x0, #0x28]` and the same at #0x30. Neither checks
    // the type of what it is handed, which is why the chain has to.
    constexpr uint64_t image_width_offset = 0x28;
    constexpr uint64_t image_height_offset = 0x30;

    // The layout the stubs below impose on the fake objects: CFGetTypeID reads +0x00 and
    // CABackingStoreCopyCGImage reads +0x08.
    constexpr uint64_t object_type_id_offset = 0x00;
    constexpr uint64_t object_copy_result_offset = 0x08;

    constexpr uint32_t ret_word = 0xD65F03C0;

    enum class stub : uint32_t
    {
        cf_get_type_id,
        image_get_type_id,
        backing_store_get_type_id,
        backing_store_copy_image,
        image_get_width,
        image_get_height,
        image_release,
        sel_register_name,
        object_get_class,
        class_get_instance_method,
        method_get_type_encoding,
        class_get_name,
        msg_send,
        colorspace_create,
        bitmap_context_create,
        context_draw_image,
        context_set_fill_color,
        context_release,
        count,
    };

    constexpr uint64_t stub_address(const stub which)
    {
        return stub_base + static_cast<uint64_t>(which) * stub_stride;
    }

    constexpr uint64_t copy_call_counter = counter_base;
    constexpr uint64_t width_call_counter = counter_base + 8;
    constexpr uint64_t msg_send_call_counter = counter_base + 16;
    constexpr uint64_t bitmap_info_slot = counter_base + 24;
    // Non-zero makes the fake CGBitmapContextCreate exit the guest instead of returning, which is what
    // a guest call that parks a thread or faults looks like from the chain's side.
    constexpr uint64_t never_return_slot = counter_base + 40;

    // The CGColor the chain last handed to CGContextSetFillColorWithColor, so a test can see the tint
    // arrive. Zero means it never set one.
    constexpr uint64_t fill_colour_slot = counter_base + 48;

    // macos_layer_read_color reads a CGColor's component count at +0x38 and its components at +0x48.
    constexpr uint64_t color_count_offset = 0x38;
    constexpr uint64_t color_components_offset = 0x48;

    // The layout the fake runtime below imposes on an object, over and above the CF slots.
    constexpr uint64_t object_msg_send_result_offset = 0x10;
    constexpr uint64_t object_class_offset = 0x18;
    constexpr uint64_t object_tint_result_offset = 0x20;

    // ldr xd, [xn, #imm] with imm a multiple of 8.
    constexpr uint32_t ldr_x(const uint32_t dst, const uint32_t src, const uint64_t offset)
    {
        return 0xF9400000u | (static_cast<uint32_t>(offset / 8) << 10) | (src << 5) | dst;
    }

    constexpr uint32_t str_x(const uint32_t src, const uint32_t base, const uint64_t offset)
    {
        return 0xF9000000u | (static_cast<uint32_t>(offset / 8) << 10) | (base << 5) | src;
    }

    // Prefixes a stub with an increment of a guest counter, so a test can prove a call never happened.
    std::vector<uint32_t> counting(const uint64_t counter, const std::vector<uint32_t>& body)
    {
        std::vector<uint32_t> words{};
        macos_test::load_x(words, 9, counter);
        words.push_back(ldr_x(10, 9, 0));
        words.push_back(0x9100054A); // add x10, x10, #1
        words.push_back(str_x(10, 9, 0));
        words.insert(words.end(), body.begin(), body.end());
        return words;
    }

    void write_stub(sogen::macos_emulator& emu, const stub which, std::vector<uint32_t> words)
    {
        const auto address = stub_address(which);
        words.push_back(ret_word);
        emu.memory.write_memory(address, words.data(), words.size() * sizeof(uint32_t));
    }

    macos_layer_contents_resolver::objc_symbols stub_objc_symbols()
    {
        return macos_layer_contents_resolver::objc_symbols{
            .sel_register_name = stub_address(stub::sel_register_name),
            .object_get_class = stub_address(stub::object_get_class),
            .class_get_instance_method = stub_address(stub::class_get_instance_method),
            .method_get_type_encoding = stub_address(stub::method_get_type_encoding),
            .class_get_name = stub_address(stub::class_get_name),
            .msg_send = stub_address(stub::msg_send),
        };
    }

    macos_layer_contents_resolver::symbols stub_symbols()
    {
        return macos_layer_contents_resolver::symbols{
            .backing_store_copy_image = stub_address(stub::backing_store_copy_image),
            .backing_store_get_type_id = stub_address(stub::backing_store_get_type_id),
            .image_get_width = stub_address(stub::image_get_width),
            .image_get_height = stub_address(stub::image_get_height),
            .image_get_type_id = stub_address(stub::image_get_type_id),
            .image_release = stub_address(stub::image_release),
            .colorspace_create_device_rgb = stub_address(stub::colorspace_create),
            .bitmap_context_create = stub_address(stub::bitmap_context_create),
            .context_draw_image = stub_address(stub::context_draw_image),
            .context_set_fill_color = stub_address(stub::context_set_fill_color),
            .context_release = stub_address(stub::context_release),
            .cf_get_type_id = stub_address(stub::cf_get_type_id),
        };
    }

    void install_stubs(sogen::macos_emulator& emu)
    {
        // One block rather than a page per stub: a fixed-address allocation inside a range the emulator
        // has already reserved for a neighbour is not guaranteed to be mapped, and adding a stub used to
        // be enough to push the last one over that line.
        emu.memory.allocate_memory(stub_base, stub_stride * static_cast<size_t>(stub::count), sogen::memory_permission::all);
        emu.memory.allocate_memory(counter_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        write_stub(emu, stub::cf_get_type_id, {ldr_x(0, 0, object_type_id_offset)});

        std::vector<uint32_t> image_id{};
        macos_test::load_x(image_id, 0, image_type_id);
        write_stub(emu, stub::image_get_type_id, image_id);

        std::vector<uint32_t> backing_id{};
        macos_test::load_x(backing_id, 0, backing_store_type_id);
        write_stub(emu, stub::backing_store_get_type_id, backing_id);

        write_stub(emu, stub::backing_store_copy_image, counting(copy_call_counter, {ldr_x(0, 0, object_copy_result_offset)}));
        write_stub(emu, stub::image_get_width, counting(width_call_counter, {ldr_x(0, 0, image_width_offset)}));
        write_stub(emu, stub::image_get_height, {ldr_x(0, 0, image_height_offset)});
        write_stub(emu, stub::image_release, {});
        write_stub(emu, stub::context_release, {});

        std::vector<uint32_t> colorspace{};
        macos_test::load_x(colorspace, 0, fake_colorspace);
        write_stub(emu, stub::colorspace_create, colorspace);

        // The pixel buffer doubles as the context handle, which is what lets the draw stub write into
        // the raster the resolver allocated. x6 is the bitmap info, kept so a test can read it back.
        std::vector<uint32_t> bitmap{};
        macos_test::load_x(bitmap, 9, bitmap_info_slot);
        bitmap.push_back(str_x(6, 9, 0));
        macos_test::load_x(bitmap, 11, never_return_slot);
        bitmap.push_back(ldr_x(11, 11, 0));
        const auto keep_going = bitmap.size();
        bitmap.push_back(0);
        bitmap.push_back(macos_test::movz_x(16, 1, 0));
        bitmap.push_back(0xD4001001u);                                                                     // svc #0x80
        bitmap[keep_going] = 0xB4000000u | (static_cast<uint32_t>(bitmap.size() - keep_going) << 5) | 11u; // cbz x11
        write_stub(emu, stub::bitmap_context_create, bitmap);

        write_stub(emu, stub::context_draw_image, {0x92800009, str_x(9, 0, 0)}); // movn x9, #0 ; str x9, [x0]

        // A selector is its own name pointer here, so a stub can tell two apart by their first letter.
        write_stub(emu, stub::sel_register_name, {});
        write_stub(emu, stub::object_get_class, {ldr_x(0, 0, object_class_offset)});
        write_stub(emu, stub::method_get_type_encoding, {ldr_x(0, 0, 0)});
        write_stub(emu, stub::class_get_name, {ldr_x(0, 0, 0x10)});

        // CGContextSetRGBFillColor takes its components in d0..d3; the red one is kept so a test can
        // see which colour arrived.
        std::vector<uint32_t> set_fill{};
        set_fill.push_back(0x9E660009); // fmov x9, d0
        macos_test::load_x(set_fill, 10, fill_colour_slot);
        set_fill.push_back(str_x(9, 10, 0));
        write_stub(emu, stub::context_set_fill_color, set_fill);
        // -tint (the only selector here starting with 't') answers the object's tint slot; every other
        // selector is an image accessor and answers the accessor slot.
        std::vector<uint32_t> send = counting(msg_send_call_counter, {});
        send.push_back(0x39400029); // ldrb w9, [x1]
        send.push_back(0x7101D129); // subs w9, w9, #0x74
        send.push_back(0x54000060); // b.eq +12
        send.push_back(ldr_x(0, 0, object_msg_send_result_offset));
        send.push_back(ret_word);
        send.push_back(ldr_x(0, 0, object_tint_result_offset));
        write_stub(emu, stub::msg_send, send);

        // A class here answers for at most two selectors, matched on the first letter of the name.
        write_stub(emu, stub::class_get_instance_method,
                   {
                       0x39400029, // ldrb w9, [x1]
                       ldr_x(10, 0, 0),
                       ldr_x(11, 0, 8),
                       0xEB0A013F, // cmp x9, x10
                       0x540000C0, // b.eq +24
                       ldr_x(10, 0, 0x18),
                       ldr_x(11, 0, 0x20),
                       0xEB0A013F, // cmp x9, x10
                       0x9A9F0160, // csel x0, x11, xzr, eq
                       ret_word,
                       0xAA0B03E0, // mov x0, x11
                   });
    }

    // One class in the fake runtime, laid out over a 0x80 slot. It answers class_getInstanceMethod for
    // at most two selectors, matched on the first letter of the name:
    //   +0x00 wanted letter A   +0x08 method A   +0x10 class name
    //   +0x18 wanted letter B   +0x20 method B
    //   +0x30 method A struct   +0x38 method B struct   (each holds a pointer to its type encoding)
    //   +0x40 encoding A        +0x58 encoding B        +0x70 class name text
    constexpr uint64_t fake_class_slot = 0x80;

    uint64_t g_objc_next = objc_base;

    void write_pointer(sogen::macos_emulator& emu, const uint64_t at, const uint64_t value)
    {
        emu.memory.write_memory(at, &value, sizeof(value));
    }

    void give_accessor(sogen::macos_emulator& emu, const uint64_t object, const char initial, const std::string& encoding,
                       const uint64_t answer)
    {
        auto& next = g_objc_next;
        if (!emu.memory.get_region_info(objc_base).has_value())
        {
            emu.memory.allocate_memory(objc_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);
        }

        const auto cls = next;
        next += fake_class_slot;

        write_pointer(emu, cls + 0x00, static_cast<uint64_t>(static_cast<uint8_t>(initial)));
        write_pointer(emu, cls + 0x08, cls + 0x30);
        write_pointer(emu, cls + 0x10, cls + 0x70);
        write_pointer(emu, cls + 0x18, 0);
        write_pointer(emu, cls + 0x20, 0);
        write_pointer(emu, cls + 0x30, cls + 0x40);
        emu.memory.write_memory(cls + 0x40, encoding.c_str(), encoding.size() + 1);
        emu.memory.write_memory(cls + 0x70, "FakeContents", sizeof("FakeContents"));

        write_pointer(emu, object + object_class_offset, cls);
        write_pointer(emu, object + object_msg_send_result_offset, answer);
    }

    // Adds a -tint to the class give_accessor already built for this object.
    void give_tint(sogen::macos_emulator& emu, const uint64_t object, const std::string& encoding, const uint64_t tint)
    {
        uint64_t cls = 0;
        emu.memory.read_memory(object + object_class_offset, &cls, sizeof(cls));

        write_pointer(emu, cls + 0x18, static_cast<uint64_t>(static_cast<uint8_t>('t')));
        write_pointer(emu, cls + 0x20, cls + 0x38);
        write_pointer(emu, cls + 0x38, cls + 0x58);
        emu.memory.write_memory(cls + 0x58, encoding.c_str(), encoding.size() + 1);
        write_pointer(emu, object + object_tint_result_offset, tint);
    }

    struct fake_object
    {
        uint64_t address{};
    };

    fake_object make_object(sogen::macos_emulator& emu, const uint64_t index, const uint64_t type_id, const uint64_t copy_result,
                            const uint64_t width, const uint64_t height)
    {
        const auto address = object_base + index * 0x100;
        if (!emu.memory.get_region_info(address).has_value())
        {
            emu.memory.allocate_memory(address & ~(sogen::MACOS_PAGE_SIZE - 1), sogen::MACOS_PAGE_SIZE,
                                       sogen::memory_permission::read_write);
        }

        emu.memory.write_memory(address + object_type_id_offset, &type_id, sizeof(type_id));
        emu.memory.write_memory(address + object_copy_result_offset, &copy_result, sizeof(copy_result));
        emu.memory.write_memory(address + image_width_offset, &width, sizeof(width));
        emu.memory.write_memory(address + image_height_offset, &height, sizeof(height));
        return fake_object{address};
    }

    // A CGColor shaped the way macos_layer_read_color expects.
    uint64_t make_color(sogen::macos_emulator& emu, const uint64_t index, const uint64_t count, const double r, const double g,
                        const double b, const double a)
    {
        const auto address = object_base + index * 0x100;
        if (!emu.memory.get_region_info(address).has_value())
        {
            emu.memory.allocate_memory(address & ~(sogen::MACOS_PAGE_SIZE - 1), sogen::MACOS_PAGE_SIZE,
                                       sogen::memory_permission::read_write);
        }

        emu.memory.write_memory(address + color_count_offset, &count, sizeof(count));
        const std::array<double, 4> components{r, g, b, a};
        emu.memory.write_memory(address + color_components_offset, components.data(), components.size() * sizeof(double));
        return address;
    }

    macos_layer_contents_resolver* g_resolver = nullptr;
    macos_layer_tree* g_tree = nullptr;
    bool g_started = false;

    void commit_handler(const sogen::macos_native_call& call)
    {
        g_started = g_resolver->resolve_one(call.emu_ref, *g_tree);
    }

    struct contents_fixture
    {
        std::unique_ptr<sogen::macos_emulator> emu{macos_test::make_emulator()};
        macos_layer_contents_resolver resolver{};
        macos_layer_tree tree{};
        sogen::macos_native_dispatch dispatch{};

        contents_fixture()
        {
            install_stubs(*this->emu);
            this->resolver.bind(stub_symbols());
            this->resolver.bind_objc(stub_objc_symbols());

            EXPECT_TRUE(this->emu->ui.calls.prepare(*this->emu));
            this->emu->set_guest_call_stack(&this->emu->ui.calls);

            this->emu->memory.allocate_memory(commit_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::all);
            this->emu->memory.write_memory(commit_base, &ret_word, sizeof(ret_word));

            this->dispatch.bind_entry(commit_base, "CommitProbe", commit_handler);
            EXPECT_TRUE(sogen::patch_native_entry(*this->emu, commit_base));
            this->emu->set_native_dispatch(&this->dispatch);

            g_resolver = &this->resolver;
            g_tree = &this->tree;
            g_started = false;
            g_objc_next = objc_base;
        }

        void add_layer(const uint64_t layer, const uint64_t object)
        {
            auto& node = this->tree.touch(layer);
            node.contents.kind = macos_layer_contents_kind::unresolved;
            node.contents.object = object;
        }

        void add_layer(const uint64_t layer, const uint64_t object, const double width, const double height, const double scale)
        {
            this->add_layer(layer, object);
            auto& node = *this->tree.find(layer);
            node.bounds = sogen::macos_layer_rect{0.0, 0.0, width, height};
            node.contents_scale = scale;
        }

        // Runs the guest until it calls the commit probe and every chain the probe started has
        // returned. The probe is an intercepted export that returns straight to its caller, which is
        // the only shape allowed to start a contents chain.
        void commit()
        {
            macos_test::write_guest_code(*this->emu, code_base,
                                         {
                                             0x94000000, // bl commit_base, patched below
                                             0xD2800020, // mov x0, #1
                                             0xD2800030, // mov x16, #1
                                             0xD4001001, // svc #0x80
                                         });

            const auto displacement = static_cast<uint32_t>((commit_base - code_base) / 4) & 0x03FFFFFFu;
            const uint32_t bl_word = 0x94000000u | displacement;
            this->emu->memory.write_memory(code_base, &bl_word, sizeof(bl_word));

            this->emu->start();
        }

        uint64_t counter(const uint64_t address) const
        {
            uint64_t value = 0;
            this->emu->memory.read_memory(address, &value, sizeof(value));
            return value;
        }

        uint64_t copy_calls() const
        {
            return this->counter(copy_call_counter);
        }

        uint64_t width_calls() const
        {
            return this->counter(width_call_counter);
        }

        uint64_t msg_send_calls() const
        {
            return this->counter(msg_send_call_counter);
        }

        uint64_t bitmap_info() const
        {
            return this->counter(bitmap_info_slot);
        }

        uint64_t fill_colour() const
        {
            return this->counter(fill_colour_slot);
        }

        void make_the_guest_never_return()
        {
            const uint64_t on = 1;
            this->emu->memory.write_memory(never_return_slot, &on, sizeof(on));
        }
    };

    TEST(LayerContents, RasterisesACGImageContentsObject)
    {
        contents_fixture fixture{};
        const auto image = make_object(*fixture.emu, 0, image_type_id, 0, 64, 32);
        fixture.add_layer(0x1000, image.address);

        fixture.commit();

        EXPECT_TRUE(g_started);
        EXPECT_EQ(fixture.resolver.resolved_count(), 1u);
        EXPECT_EQ(fixture.resolver.failed_count(), 0u);
        EXPECT_EQ(fixture.copy_calls(), 0u) << "a CGImage is drawn directly, never through a backing store";

        const auto* node = fixture.tree.find(0x1000);
        ASSERT_NE(node, nullptr);
        ASSERT_EQ(node->contents.kind, macos_layer_contents_kind::raster);
        EXPECT_EQ(node->contents.raster.width, 64u);
        EXPECT_EQ(node->contents.raster.height, 32u);
        EXPECT_EQ(node->contents.raster.stride, 64u * 4u);

        uint64_t drawn = 0;
        fixture.emu->memory.read_memory(node->contents.raster.pixels, &drawn, sizeof(drawn));
        EXPECT_EQ(drawn, ~uint64_t{0}) << "CGContextDrawImage ran against the raster the resolver allocated";
    }

    // macos_layer_raster documents BGRA8 premultiplied, and the compositor reads it that way. Measured
    // on 25G76 by filling a 1x1 context: kCGBitmapByteOrder32Little | kCGImageAlphaPremultipliedFirst
    // (0x2002) writes B, G, R, A, while ...PremultipliedLast (0x2001) writes A, B, G, R -- an opaque
    // grey comes back as `ff d9 d9 d9` and every colour reaches the compositor rotated.
    TEST(LayerContents, AsksCoreGraphicsForTheChannelOrderTheCompositorReads)
    {
        contents_fixture fixture{};
        const auto image = make_object(*fixture.emu, 0, image_type_id, 0, 8, 8);
        fixture.add_layer(0x1000, image.address);

        fixture.commit();

        ASSERT_EQ(fixture.resolver.resolved_count(), 1u);
        EXPECT_EQ(fixture.bitmap_info(), 0x2002u);
    }

    // The measured failure this chain was rewritten for. An NSViewBackingLayerContents reaches
    // -[CALayer setContents:] on every AppKit window; CFGetTypeID answers 1 for it, and the real
    // CABackingStoreCopyCGImage dereferences its argument as a backing store with no check at all --
    // it faults at +0x34 on a live host (EXC_BAD_ACCESS, address 0x10) when handed a CATintedImage.
    TEST(LayerContents, NeverAsksABackingStoreForAnObjectThatIsNotOne)
    {
        contents_fixture fixture{};
        const auto plain = make_object(*fixture.emu, 0, plain_objc_type_id, 0, 0, 0);
        fixture.add_layer(0x1000, plain.address);

        fixture.commit();

        EXPECT_EQ(fixture.copy_calls(), 0u) << "CABackingStoreCopyCGImage must not see a non-backing-store";
        EXPECT_EQ(fixture.width_calls(), 0u);
        EXPECT_EQ(fixture.resolver.resolved_count(), 0u);
        EXPECT_EQ(fixture.resolver.failed_count(), 1u);
        EXPECT_EQ(fixture.tree.find(0x1000)->contents.kind, macos_layer_contents_kind::unresolved);
    }

    TEST(LayerContents, RasterisesTheImageABackingStoreCopiesOut)
    {
        contents_fixture fixture{};
        const auto image = make_object(*fixture.emu, 1, image_type_id, 0, 48, 24);
        const auto store = make_object(*fixture.emu, 0, backing_store_type_id, image.address, 0, 0);
        fixture.add_layer(0x1000, store.address);

        fixture.commit();

        EXPECT_EQ(fixture.copy_calls(), 1u);
        EXPECT_EQ(fixture.resolver.resolved_count(), 1u);

        const auto* node = fixture.tree.find(0x1000);
        ASSERT_EQ(node->contents.kind, macos_layer_contents_kind::raster);
        EXPECT_EQ(node->contents.raster.width, 48u);
        EXPECT_EQ(node->contents.raster.height, 24u);
    }

    // CGImageGetWidth and CGImageGetHeight are unchecked field loads, so an answer that is not a
    // CGImage measures as whatever happens to sit at +0x28 and +0x30 rather than failing. Measured
    // under sogen before this check existed: 1992 and 6539417313, read out of an AppKit data page. A
    // range check on the answer is not enough -- the dimensions here are both perfectly plausible.
    TEST(LayerContents, RefusesACopiedImageThatIsNotACGImage)
    {
        contents_fixture fixture{};
        const auto impostor = make_object(*fixture.emu, 1, plain_objc_type_id, 0, 1992, 64);
        const auto store = make_object(*fixture.emu, 0, backing_store_type_id, impostor.address, 0, 0);
        fixture.add_layer(0x1000, store.address);

        fixture.commit();

        EXPECT_EQ(fixture.width_calls(), 0u) << "nothing but a CGImage may reach CGImageGetWidth";
        EXPECT_EQ(fixture.resolver.resolved_count(), 0u);
        EXPECT_EQ(fixture.resolver.failed_count(), 1u);
        EXPECT_EQ(fixture.tree.find(0x1000)->contents.kind, macos_layer_contents_kind::unresolved);
    }

    // CATintedImage is a layer's contents on every measured AppKit and SwiftUI frame and is not a CF
    // type at all, but it lends out a real CGImage through the nullary -image accessor
    // (encoding ^{CGImage=}16@0:8, ivar _image @8, measured 25G76).
    TEST(LayerContents, RasterisesTheCGImageAnObjcAccessorHandsOut)
    {
        contents_fixture fixture{};
        const auto image = make_object(*fixture.emu, 1, image_type_id, 0, 40, 20);
        const auto holder = make_object(*fixture.emu, 0, plain_objc_type_id, 0, 0, 0);
        give_accessor(*fixture.emu, holder.address, 'C', "^{CGImage=}16@0:8", image.address);
        fixture.add_layer(0x1000, holder.address);

        fixture.commit();

        EXPECT_EQ(fixture.copy_calls(), 0u) << "an ObjC object never reaches CABackingStoreCopyCGImage";
        EXPECT_EQ(fixture.msg_send_calls(), 1u);
        EXPECT_EQ(fixture.resolver.resolved_count(), 1u);

        const auto* node = fixture.tree.find(0x1000);
        ASSERT_EQ(node->contents.kind, macos_layer_contents_kind::raster);
        EXPECT_EQ(node->contents.raster.width, 40u);
        EXPECT_EQ(node->contents.raster.height, 20u);
    }

    // An accessor returning an integer, a float or a struct leaves something that is not an address in
    // x0, and CFGetTypeID dereferences whatever it is handed. The return type decides whether the
    // accessor may be called at all.
    TEST(LayerContents, NeverCallsAnAccessorThatDoesNotReturnAPointer)
    {
        contents_fixture fixture{};
        const auto holder = make_object(*fixture.emu, 0, plain_objc_type_id, 0, 0, 0);
        give_accessor(*fixture.emu, holder.address, 'C', "i16@0:8", 0x4141414141414141ULL);
        fixture.add_layer(0x1000, holder.address);

        fixture.commit();

        EXPECT_EQ(fixture.msg_send_calls(), 0u) << "the encoding is read before the accessor is called";
        EXPECT_EQ(fixture.resolver.resolved_count(), 0u);
        EXPECT_EQ(fixture.resolver.failed_count(), 1u);
    }

    TEST(LayerContents, RefusesAnAccessorWhoseAnswerIsNotACGImage)
    {
        contents_fixture fixture{};
        const auto impostor = make_object(*fixture.emu, 1, plain_objc_type_id, 0, 1992, 64);
        const auto holder = make_object(*fixture.emu, 0, plain_objc_type_id, 0, 0, 0);
        give_accessor(*fixture.emu, holder.address, 'C', "@16@0:8", impostor.address);
        fixture.add_layer(0x1000, holder.address);

        fixture.commit();

        EXPECT_EQ(fixture.msg_send_calls(), 1u);
        EXPECT_EQ(fixture.width_calls(), 0u) << "nothing but a CGImage may reach CGImageGetWidth";
        EXPECT_EQ(fixture.resolver.resolved_count(), 0u);
        EXPECT_EQ(fixture.resolver.failed_count(), 1u);
    }

    TEST(LayerContents, TriesEveryAccessorNameBeforeGivingUp)
    {
        contents_fixture fixture{};
        const auto image = make_object(*fixture.emu, 1, image_type_id, 0, 12, 6);
        const auto holder = make_object(*fixture.emu, 0, plain_objc_type_id, 0, 0, 0);
        give_accessor(*fixture.emu, holder.address, 'i', "^{CGImage=}16@0:8", image.address);
        fixture.add_layer(0x1000, holder.address);

        fixture.commit();

        EXPECT_EQ(fixture.resolver.resolved_count(), 1u) << "-image is reached after -CGImage does not exist";
        EXPECT_EQ(fixture.tree.find(0x1000)->contents.raster.width, 12u);
    }

    TEST(LayerContents, RefusesAnObjcObjectWhoseClassHandsOutNoImage)
    {
        contents_fixture fixture{};
        const auto holder = make_object(*fixture.emu, 0, plain_objc_type_id, 0, 0, 0);
        give_accessor(*fixture.emu, holder.address, 'Z', "^{CGImage=}16@0:8", 0);
        fixture.add_layer(0x1000, holder.address);

        fixture.commit();

        EXPECT_EQ(fixture.msg_send_calls(), 0u);
        EXPECT_EQ(fixture.resolver.failed_count(), 1u);
        EXPECT_EQ(fixture.tree.find(0x1000)->contents.kind, macos_layer_contents_kind::unresolved);
    }

    // The route is optional: a system whose libobjc is missing an entry point still resolves every CF
    // contents object, and an ObjC one is refused rather than guessed at.
    TEST(LayerContents, StaysOnTheCfRouteWithoutTheObjcRuntime)
    {
        contents_fixture fixture{};
        fixture.resolver.bind_objc({});

        const auto image = make_object(*fixture.emu, 1, image_type_id, 0, 40, 20);
        const auto holder = make_object(*fixture.emu, 0, plain_objc_type_id, 0, 0, 0);
        give_accessor(*fixture.emu, holder.address, 'C', "^{CGImage=}16@0:8", image.address);
        fixture.add_layer(0x1000, holder.address);
        fixture.add_layer(0x2000, image.address);

        fixture.commit();

        EXPECT_EQ(fixture.msg_send_calls(), 0u);
        EXPECT_EQ(fixture.resolver.resolved_count(), 1u) << "the CGImage still resolves";
        EXPECT_EQ(fixture.resolver.failed_count(), 1u);
    }

    // A guest call in the chain can fail to return: the contents object went stale under the guest's own
    // heap corruption, or the call parked a thread. The frames it pushed stay on the guest-call stack,
    // so a resolver that only asked "is the stack busy?" would rasterise nothing for the rest of the run.
    TEST(LayerContents, RecoversFromAChainTheGuestNeverReturnedFrom)
    {
        contents_fixture fixture{};
        fixture.make_the_guest_never_return();

        const auto image = make_object(*fixture.emu, 0, image_type_id, 0, 32, 16);
        fixture.add_layer(0x1000, image.address);

        fixture.commit();

        EXPECT_EQ(fixture.resolver.resolved_count(), 0u);
        EXPECT_EQ(fixture.resolver.failed_count(), 0u) << "nothing has given up on it yet";

        fixture.resolver.resolve_one(*fixture.emu, fixture.tree);
        EXPECT_EQ(fixture.resolver.failed_count(), 1u) << "the next commit drops the chain that never returned";

        fixture.resolver.resolve_one(*fixture.emu, fixture.tree);
        EXPECT_EQ(fixture.resolver.failed_count(), 1u) << "and does not retry it";
    }

    // A template image is a mask: the CGImage carries coverage and no colour, and CGContextDrawImage
    // paints it in the context's fill colour -- black by default, which is why an untinted template
    // renders dark. Measured on a live CATintedImage: image 15x32, 8bpp, alphaInfo 0, no colour space,
    // and a -tint returning a CGColor.
    TEST(LayerContents, PaintsATemplateImageInTheTintItCarries)
    {
        contents_fixture fixture{};
        const auto image = make_object(*fixture.emu, 1, image_type_id, 0, 40, 20);
        const auto colour = make_color(*fixture.emu, 2, 4, 0.25, 0.5, 0.75, 1.0);
        const auto holder = make_object(*fixture.emu, 0, plain_objc_type_id, 0, 0, 0);
        give_accessor(*fixture.emu, holder.address, 'C', "^{CGImage=}16@0:8", image.address);
        give_tint(*fixture.emu, holder.address, "^{CGColor=}16@0:8", colour);
        fixture.add_layer(0x1000, holder.address);

        fixture.commit();

        EXPECT_EQ(fixture.resolver.resolved_count(), 1u);

        double red = 0;
        const auto bits = fixture.fill_colour();
        std::memcpy(&red, &bits, sizeof(red));
        EXPECT_DOUBLE_EQ(red, 0.25) << "the tint's own components reach CGContextSetRGBFillColor";
    }

    TEST(LayerContents, LeavesTheFillColourAloneForAnImageWithNoTint)
    {
        contents_fixture fixture{};
        const auto image = make_object(*fixture.emu, 1, image_type_id, 0, 40, 20);
        const auto holder = make_object(*fixture.emu, 0, plain_objc_type_id, 0, 0, 0);
        give_accessor(*fixture.emu, holder.address, 'C', "^{CGImage=}16@0:8", image.address);
        fixture.add_layer(0x1000, holder.address);

        fixture.commit();

        EXPECT_EQ(fixture.resolver.resolved_count(), 1u);
        EXPECT_EQ(fixture.fill_colour(), 0u) << "a class with no -tint changes nothing about the context";
    }

    // A colour whose component count is not one sogen decodes is not a colour it may paint with.
    TEST(LayerContents, IgnoresATintItCannotRead)
    {
        contents_fixture fixture{};
        const auto image = make_object(*fixture.emu, 1, image_type_id, 0, 40, 20);
        const auto colour = make_color(*fixture.emu, 2, 3, 0.25, 0.5, 0.75, 1.0);
        const auto holder = make_object(*fixture.emu, 0, plain_objc_type_id, 0, 0, 0);
        give_accessor(*fixture.emu, holder.address, 'C', "^{CGImage=}16@0:8", image.address);
        give_tint(*fixture.emu, holder.address, "^{CGColor=}16@0:8", colour);
        fixture.add_layer(0x1000, holder.address);

        fixture.commit();

        EXPECT_EQ(fixture.resolver.resolved_count(), 1u) << "the image still resolves";
        EXPECT_EQ(fixture.fill_colour(), 0u);
    }

    TEST(LayerContents, NeverCallsATintAccessorThatDoesNotReturnAPointer)
    {
        contents_fixture fixture{};
        const auto image = make_object(*fixture.emu, 1, image_type_id, 0, 40, 20);
        const auto colour = make_color(*fixture.emu, 2, 4, 0.25, 0.5, 0.75, 1.0);
        const auto holder = make_object(*fixture.emu, 0, plain_objc_type_id, 0, 0, 0);
        give_accessor(*fixture.emu, holder.address, 'C', "^{CGImage=}16@0:8", image.address);
        give_tint(*fixture.emu, holder.address, "i16@0:8", colour);
        fixture.add_layer(0x1000, holder.address);

        fixture.commit();

        EXPECT_EQ(fixture.resolver.resolved_count(), 1u);
        EXPECT_EQ(fixture.msg_send_calls(), 1u) << "only the image accessor was sent";
        EXPECT_EQ(fixture.fill_colour(), 0u);
    }

    // One CATransaction commit is the only point a chain may start from, so a commit that resolved a
    // single object would need one frame per layer to dress a window.
    TEST(LayerContents, DrainsEveryUnresolvedObjectInOneCommit)
    {
        contents_fixture fixture{};
        for (uint64_t i = 0; i < 6; ++i)
        {
            const auto image = make_object(*fixture.emu, i, image_type_id, 0, 8 + i, 4 + i);
            fixture.add_layer(0x1000 + i, image.address);
        }

        fixture.commit();

        EXPECT_EQ(fixture.resolver.resolved_count(), 6u);
        for (uint64_t i = 0; i < 6; ++i)
        {
            const auto* node = fixture.tree.find(0x1000 + i);
            ASSERT_EQ(node->contents.kind, macos_layer_contents_kind::raster) << "layer " << i;
            EXPECT_EQ(node->contents.raster.width, 8u + i);
        }
    }

    TEST(LayerContents, RefusesTheSameObjectOnlyOnce)
    {
        contents_fixture fixture{};
        const auto plain = make_object(*fixture.emu, 0, plain_objc_type_id, 0, 0, 0);
        fixture.add_layer(0x1000, plain.address);

        fixture.commit();
        fixture.commit();
        fixture.commit();

        EXPECT_EQ(fixture.resolver.failed_count(), 1u) << "a refusal is remembered rather than retried every frame";
    }

    TEST(LayerContents, ReusesACachedRasterForAnObjectTwoLayersShare)
    {
        contents_fixture fixture{};
        const auto image = make_object(*fixture.emu, 0, image_type_id, 0, 16, 8);
        fixture.add_layer(0x1000, image.address);

        fixture.commit();
        ASSERT_EQ(fixture.resolver.resolved_count(), 1u);

        fixture.add_layer(0x2000, image.address);
        fixture.commit();

        EXPECT_EQ(fixture.resolver.resolved_count(), 1u) << "the second layer takes the raster out of the cache";

        const auto* second = fixture.tree.find(0x2000);
        ASSERT_EQ(second->contents.kind, macos_layer_contents_kind::raster);
        EXPECT_EQ(second->contents.raster.pixels, fixture.tree.find(0x1000)->contents.raster.pixels);
    }

    // CoreAnimation draws a redraw into the CABackingStore a layer already holds, so the raster taken
    // from it on an earlier frame is last frame's picture. -[CALayer display] is where that happens and
    // where the tree is told; without this the cache would answer with the stale copy forever.
    TEST(LayerContents, ARedrawnContentsObjectIsRasterisedAgain)
    {
        contents_fixture fixture{};
        const auto image = make_object(*fixture.emu, 0, image_type_id, 0, 16, 8);
        fixture.add_layer(0x1000, image.address);

        fixture.commit();
        ASSERT_EQ(fixture.resolver.resolved_count(), 1u);
        const auto first = fixture.tree.find(0x1000)->contents.raster.pixels;
        ASSERT_NE(first, 0u);

        fixture.resolver.forget(*fixture.emu, fixture.tree, image.address);

        const auto* forgotten = fixture.tree.find(0x1000);
        ASSERT_NE(forgotten, nullptr);
        EXPECT_EQ(forgotten->contents.kind, macos_layer_contents_kind::unresolved);
        EXPECT_EQ(forgotten->contents.object, image.address) << "the layer still points at the object it was given";

        fixture.commit();

        EXPECT_EQ(fixture.resolver.resolved_count(), 2u) << "the second frame takes a fresh raster instead of the cached one";

        const auto* again = fixture.tree.find(0x1000);
        ASSERT_EQ(again->contents.kind, macos_layer_contents_kind::raster);
        EXPECT_EQ(again->contents.raster.pixels, first) << "the block the stale raster lived in serves the new one";
    }

    // A commit composites before the drain it starts can finish, so a contents object first seen on this
    // commit is missing from the frame the guest just asked for. An application that redraws on a click
    // commits once and then waits for the next one, which is why the drain presents rather than leaving
    // the picture for a frame that may never come.
    TEST(LayerContents, ARasterIsPresentedOnTheCommitThatMadeIt)
    {
        contents_fixture fixture{};

        auto backend = sogen::create_screenshot_ui_backend();
        auto* shot = static_cast<sogen::screenshot_ui_backend*>(backend.get());
        fixture.emu->set_ui_backend(std::move(backend));

        auto& tree = sogen::macos_layer_tree_of(*fixture.emu);
        g_tree = &tree;

        auto* window = fixture.emu->ui.server.create_window(fixture.emu->ui.server.main_connection(), 0, 0, 16, 8);
        ASSERT_NE(window, nullptr);
        window->layer_context = 0xC1;
        fixture.emu->ui.sync_window(*fixture.emu, *window);

        const auto image = make_object(*fixture.emu, 0, image_type_id, 0, 16, 8);

        auto& root = tree.touch(0x9000);
        root.bounds = {0, 0, 16, 8};
        root.position = {0, 0};
        root.anchor_point = {0, 0};
        root.contents.kind = macos_layer_contents_kind::unresolved;
        root.contents.object = image.address;
        tree.set_context_root(0xC1, 0x9000);

        ASSERT_EQ(shot->present_count(), 0u);

        fixture.commit();

        EXPECT_EQ(fixture.resolver.resolved_count(), 1u);
        EXPECT_EQ(shot->present_count(), 1u) << "the drain presented the frame its raster belongs to";
        EXPECT_EQ(tree.find(0x9000)->contents.kind, macos_layer_contents_kind::raster);

        fixture.commit();
        EXPECT_EQ(shot->present_count(), 1u) << "a commit that rasterises nothing new presents nothing";

        sogen::macos_layer_tree_release(*fixture.emu);
    }

    // A run loop with a timer armed against its own wait port never reaches the host park: the scheduler
    // always has that deadline to fire instead, and the park used to be the only place a repaint that
    // moved nothing but CoreAnimation was rasterised. Measured on inputprobe through the click harness,
    // over one run: 1963 mk_timer deadlines serviced, 609 kqueue deadlines, and not a single park.
    TEST(LayerContents, AnArmedTimerDoesNotStrandTheFrameAWaitingThreadStillOwes)
    {
        contents_fixture fixture{};
        auto& emu = *fixture.emu;

        auto backend = sogen::create_screenshot_ui_backend();
        auto* shot = static_cast<sogen::screenshot_ui_backend*>(backend.get());
        shot->set_input_source(true);
        emu.set_ui_backend(std::move(backend));

        emu.ui.enabled = true;
        emu.ui.contents.bind(stub_symbols());
        emu.ui.contents.bind_objc(stub_objc_symbols());

        auto& tree = sogen::macos_layer_tree_of(emu);

        auto* window = emu.ui.server.create_window(emu.ui.server.main_connection(), 0, 0, 16, 8);
        ASSERT_NE(window, nullptr);
        window->layer_context = 0xC1;
        emu.ui.sync_window(emu, *window);

        const auto image = make_object(emu, 0, image_type_id, 0, 16, 8);
        auto& root = tree.touch(0x9000);
        root.bounds = {0, 0, 16, 8};
        root.position = {0, 0};
        root.anchor_point = {0, 0};
        root.contents.kind = macos_layer_contents_kind::unresolved;
        root.contents.object = image.address;
        tree.set_context_root(0xC1, 0x9000);

        const auto timer = emu.mach.create_timer();
        ASSERT_NE(timer, sogen::mach::PORT_NULL);
        emu.mach.arm_timer(timer, emu.emu().read_system_register(3, 3, 14, 0, 2) + 1'000'000);

        size_t polls = 0;
        size_t presents_at_the_first_park = 0;
        emu.on_host_idle = [&] {
            if (polls++ == 0)
            {
                presents_at_the_first_park = shot->present_count();
            }

            emu.stop();
        };

        constexpr uint64_t receive_base = 0x100010000ULL;
        constexpr uint64_t receive_stack = 0x328000000ULL;
        constexpr uint64_t message_base = 0x350000000ULL;
        ASSERT_TRUE(emu.memory.allocate_memory(message_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        ASSERT_TRUE(emu.memory.allocate_memory(receive_stack, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        macos_test::mach_msg2_args args{};
        args.buffer = message_base;
        args.options = sogen::mach::msg_option::rcv_msg;
        args.rcv_name = emu.mach.ports.allocate_receive_right();
        args.rcv_size = 256;
        args.timeout = 0;

        const auto words = macos_test::mach_msg2_words(args);
        macos_test::write_guest_code(emu, receive_base, words);

        const auto waiter = emu.process.create_thread(receive_stack, sogen::MACOS_PAGE_SIZE, receive_base);
        ASSERT_TRUE(emu.activate_thread(waiter));

        emu.start(200'000);

        EXPECT_EQ(tree.find(0x9000)->contents.kind, macos_layer_contents_kind::raster);
        EXPECT_EQ(shot->present_count(), 1u) << "the layer the guest committed reached the screen";
        EXPECT_GT(polls, 0u) << "the run ended on the park, so everything ahead of it had its turn";
        EXPECT_EQ(presents_at_the_first_park, 1u)
            << "the receive itself finished the frame; leaving it to the park strands it behind every armed timer";

        sogen::macos_layer_tree_release(emu);
    }

    TEST(LayerContents, StaysInertWithoutEverySymbolItNeeds)
    {
        contents_fixture fixture{};

        auto incomplete = stub_symbols();
        incomplete.backing_store_get_type_id = 0;
        fixture.resolver.bind(incomplete);

        const auto image = make_object(*fixture.emu, 0, image_type_id, 0, 16, 8);
        fixture.add_layer(0x1000, image.address);

        EXPECT_FALSE(fixture.resolver.bound());
        fixture.commit();

        EXPECT_EQ(fixture.resolver.resolved_count(), 0u);
        EXPECT_EQ(fixture.resolver.failed_count(), 0u);
    }
}
