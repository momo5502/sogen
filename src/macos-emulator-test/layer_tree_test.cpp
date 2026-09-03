#include <gtest/gtest.h>

#include "macos_test_utils.hpp"

#include <gui/macos_appkit_intercept.hpp>
#include <gui/macos_layer_tree.hpp>
#include <gui/macos_native_dispatch.hpp>

#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    using sogen::macos_layer_affine;
    using sogen::macos_layer_contents_kind;
    using sogen::macos_layer_gravity;
    using sogen::macos_layer_rect;
    using sogen::macos_layer_tree;

    constexpr uint64_t code_base = 0x100000000ULL;
    constexpr uint64_t imp_base = 0x100004000ULL;
    constexpr uint64_t data_base = 0x100008000ULL;
    constexpr uint64_t object_base = 0x100010000ULL;

    constexpr uint64_t objc_class_offset = 0x100;
    constexpr uint64_t objc_rw_offset = 0x180;
    constexpr uint64_t objc_ro_offset = 0x1C0;
    constexpr uint64_t objc_rw_extension_offset = 0x200;
    constexpr uint64_t objc_name_offset = 0x240;
    constexpr uint64_t swift_class_offset = 0x300;
    constexpr uint64_t swift_descriptor_offset = 0x380;
    constexpr uint64_t swift_name_offset = 0x3C0;
    constexpr uint64_t second_swift_class_offset = 0x400;
    constexpr uint64_t second_swift_descriptor_offset = 0x480;
    constexpr uint64_t second_swift_name_offset = 0x4C0;
    constexpr uint64_t deferred_storage_offset = 0x900;
    constexpr uint64_t element_storage_offset = 0x800;

    constexpr uint32_t objc_rw_realized = 0x80000000u;
    constexpr uint64_t swift_stable_bit = 0x2;

    // The trampoline runs this instruction and then jumps to imp + 4, where the body reads x9 back. A
    // caller that exits with 0x56 has proved that both halves ran with the register state intact.
    constexpr uint32_t displaced_instruction = 0xD2800AA9; // mov x9, #0x55
    constexpr uint32_t body_instruction = 0x91000520;      // add x0, x9, #1
    constexpr uint32_t ret_instruction = 0xD65F03C0;
    constexpr int pass_through_result = 0x56;

    sogen::macos_native_handler handler_for(const std::string_view selector)
    {
        for (const auto& table : {sogen::macos_layer_tree_methods(), sogen::macos_appkit_methods()})
        {
            for (const auto& method : table)
            {
                if (method.selector == selector)
                {
                    return method.handler;
                }
            }
        }

        return nullptr;
    }

    struct intercept_call
    {
        std::array<uint64_t, 4> x{};
        std::array<double, 4> d{};
        float s0{};
        bool load_s0{};
    };

    // Drives one guest call into a patched method implementation the way objc_msgSend would: self in x0,
    // the selector in x1, the remaining integer arguments in x2/x3 and the floating-point ones in the
    // vector bank. Register placement per method is measured in
    class intercept_fixture
    {
      public:
        explicit intercept_fixture(const std::string_view selector)
            : emu_(macos_test::make_emulator())
        {
            this->handler_ = handler_for(selector);
            this->emu_->memory.allocate_memory(imp_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::all);
            this->emu_->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);
            this->emu_->memory.allocate_memory(object_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

            const std::array<uint32_t, 3> method{displaced_instruction, body_instruction, ret_instruction};
            this->emu_->memory.write_memory(imp_base, method.data(), sizeof(method));
        }

        ~intercept_fixture()
        {
            sogen::macos_layer_tree_release(*this->emu_);
        }

        intercept_fixture(const intercept_fixture&) = delete;
        intercept_fixture& operator=(const intercept_fixture&) = delete;
        intercept_fixture(intercept_fixture&&) = delete;
        intercept_fixture& operator=(intercept_fixture&&) = delete;

        sogen::macos_emulator& emu() const
        {
            return *this->emu_;
        }

        bool install()
        {
            return this->handler_ != nullptr &&
                   sogen::macos_layer_tree_install(*this->emu_, this->dispatch_, imp_base, "test-method", this->handler_);
        }

        void write_object(const uint64_t offset, const std::span<const uint64_t> words) const
        {
            this->emu_->memory.write_memory(object_base + offset, words.data(), words.size() * sizeof(uint64_t));
        }

        void write_doubles(const uint64_t offset, const std::span<const double> values) const
        {
            this->emu_->memory.write_memory(object_base + offset, values.data(), values.size() * sizeof(double));
        }

        void write_word(const uint64_t offset, const uint64_t value) const
        {
            this->emu_->memory.write_memory(object_base + offset, &value, sizeof(value));
        }

        void write_half(const uint64_t offset, const uint32_t value) const
        {
            this->emu_->memory.write_memory(object_base + offset, &value, sizeof(value));
        }

        void write_string(const uint64_t offset, const std::string_view text) const
        {
            this->emu_->memory.write_memory(object_base + offset, text.data(), text.size());
            const char terminator = 0;
            this->emu_->memory.write_memory(object_base + offset + text.size(), &terminator, sizeof(terminator));
        }

        // An objc_class whose name the runtime would report as `name`: bits at +0x20 point at a
        // class_rw_t whose +0x08 reaches the class_ro_t holding the name pointer at +0x18. An
        // unrealized class has no class_rw_t, so bits point at the class_ro_t directly.
        uint64_t write_objc_class(const std::string_view name, const bool realized, const bool through_extension) const
        {
            this->write_string(objc_name_offset, name);
            this->write_word(objc_ro_offset + 0x18, object_base + objc_name_offset);

            if (!realized)
            {
                this->write_half(objc_ro_offset, 0);
                this->write_word(objc_class_offset + 0x20, object_base + objc_ro_offset);
                return object_base + objc_class_offset;
            }

            this->write_half(objc_rw_offset, objc_rw_realized);

            if (through_extension)
            {
                this->write_word(objc_rw_extension_offset, object_base + objc_ro_offset);
                this->write_word(objc_rw_offset + 0x08, (object_base + objc_rw_extension_offset) | 1);
            }
            else
            {
                this->write_word(objc_rw_offset + 0x08, object_base + objc_ro_offset);
            }

            this->write_word(objc_class_offset + 0x20, object_base + objc_rw_offset);
            return object_base + objc_class_offset;
        }

        // What the arm64e runtime leaves in memory: every pointer the class walk follows is signed in
        // place, so its upper bits are a signature rather than address.
        void sign_class_pointers() const
        {
            constexpr uint64_t signature = 0x004D000000000000ULL;

            uint64_t bits = 0;
            uint64_t ro_or_extension = 0;
            uint64_t name = 0;
            this->emu_->memory.read_memory(object_base + objc_class_offset + 0x20, &bits, sizeof(bits));
            this->emu_->memory.read_memory(object_base + objc_rw_offset + 0x08, &ro_or_extension, sizeof(ro_or_extension));
            this->emu_->memory.read_memory(object_base + objc_ro_offset + 0x18, &name, sizeof(name));

            this->write_word(objc_class_offset + 0x20, bits | signature);
            this->write_word(objc_rw_offset + 0x08, ro_or_extension | signature);
            this->write_word(objc_ro_offset + 0x18, name | signature);
        }

        // A Swift class: bits carry the stable-ABI marker, the type descriptor sits at +0x40 and names
        // the type through a signed 32-bit offset relative to the field that holds it.
        uint64_t write_swift_class_at(const uint64_t class_offset, const uint64_t descriptor_offset, const uint64_t name_offset,
                                      const std::string_view name) const
        {
            this->write_string(name_offset, name);
            this->write_word(class_offset + 0x20, swift_stable_bit);
            this->write_word(class_offset + 0x40, object_base + descriptor_offset);
            this->write_half(descriptor_offset + 0x08,
                             static_cast<uint32_t>(static_cast<int32_t>(name_offset - (descriptor_offset + 0x08))));
            return object_base + class_offset;
        }

        uint64_t write_swift_class(const std::string_view name) const
        {
            return this->write_swift_class_at(swift_class_offset, swift_descriptor_offset, swift_name_offset, name);
        }

        uint64_t write_second_swift_class(const std::string_view name) const
        {
            return this->write_swift_class_at(second_swift_class_offset, second_swift_descriptor_offset, second_swift_name_offset, name);
        }

        void run(const intercept_call& call)
        {
            this->emu_->memory.write_memory(data_base, call.d.data(), call.d.size() * sizeof(double));
            this->emu_->memory.write_memory(data_base + 64, &call.s0, sizeof(call.s0));

            std::vector<uint32_t> code{};
            macos_test::load_x(code, 4, data_base);
            code.push_back(0xFD400080); // ldr d0, [x4]
            code.push_back(0xFD400481); // ldr d1, [x4, #8]
            code.push_back(0xFD400882); // ldr d2, [x4, #16]
            code.push_back(0xFD400C83); // ldr d3, [x4, #24]

            if (call.load_s0)
            {
                code.push_back(0xBD404080); // ldr s0, [x4, #64]
            }

            for (uint32_t reg = 0; reg < call.x.size(); ++reg)
            {
                macos_test::load_x(code, reg, call.x[reg]);
            }

            const auto call_index = code.size();
            code.push_back(0x94000000); // bl (patched below)
            code.push_back(0xD2800030); // mov x16, #1
            code.push_back(0xD4001001); // svc #0x80

            const auto call_pc = code_base + call_index * sizeof(uint32_t);
            code[call_index] = 0x94000000u | ((static_cast<uint32_t>((imp_base - call_pc) / 4)) & 0x03FFFFFFu);

            macos_test::write_guest_code(*this->emu_, code_base, code);
            this->emu_->set_native_dispatch(&this->dispatch_);
            this->emu_->start();
        }

        macos_layer_tree& tree() const
        {
            return sogen::macos_layer_tree_of(*this->emu_);
        }

        std::optional<int> exit_status() const
        {
            return this->emu_->process.exit_status;
        }

        uint64_t returned() const
        {
            return this->emu_->emu().reg(sogen::arm64_register::x0);
        }

      private:
        std::unique_ptr<sogen::macos_emulator> emu_{};
        sogen::macos_native_dispatch dispatch_{};
        sogen::macos_native_handler handler_{};
    };

    // A layer sogen never recorded and a layer with no sublayers are both "no children" to a caller
    // reading the tree back, and keeping the distinction out of the assertions is what makes a wrong
    // decode fail rather than crash the test.
    std::vector<uint64_t> children_of(const intercept_fixture& fixture, const uint64_t layer)
    {
        const auto* node = fixture.tree().find(layer);
        return node == nullptr ? std::vector<uint64_t>{} : node->children;
    }

    TEST(LayerTree, StandardizesRectsTheWayCALayerDoes)
    {
        const auto negative = macos_layer_rect{0, 0, -40, -20}.standardized();
        EXPECT_DOUBLE_EQ(negative.x, -40.0);
        EXPECT_DOUBLE_EQ(negative.y, -20.0);
        EXPECT_DOUBLE_EQ(negative.width, 40.0);
        EXPECT_DOUBLE_EQ(negative.height, 20.0);

        EXPECT_TRUE((macos_layer_rect{0, 0, 0, 5}).empty());
        EXPECT_TRUE((macos_layer_rect{0, 0, 5, 0}).empty());
        EXPECT_FALSE((macos_layer_rect{0, 0, 5, 5}).empty());

        const auto nan_rect = macos_layer_rect{std::nan(""), 1, 2, 3}.standardized();
        EXPECT_DOUBLE_EQ(nan_rect.width, 0.0);
        EXPECT_TRUE(nan_rect.empty());
    }

    TEST(LayerTree, DerivesBoundsAndPositionFromAFrame)
    {
        macos_layer_tree tree{};

        tree.touch(1).anchor_point = {0, 0};
        EXPECT_TRUE(tree.set_frame(1, {10, 20, 30, 40}));
        EXPECT_DOUBLE_EQ(tree.find(1)->bounds.width, 30.0);
        EXPECT_DOUBLE_EQ(tree.find(1)->bounds.height, 40.0);
        EXPECT_DOUBLE_EQ(tree.find(1)->position.x, 10.0);
        EXPECT_DOUBLE_EQ(tree.find(1)->position.y, 20.0);

        tree.touch(2).anchor_point = {0.5, 0.5};
        EXPECT_TRUE(tree.set_frame(2, {10, 20, 30, 40}));
        EXPECT_DOUBLE_EQ(tree.find(2)->position.x, 25.0);
        EXPECT_DOUBLE_EQ(tree.find(2)->position.y, 40.0);

        tree.touch(3).anchor_point = {1, 1};
        EXPECT_TRUE(tree.set_frame(3, {10, 20, 30, 40}));
        EXPECT_DOUBLE_EQ(tree.find(3)->position.x, 40.0);
        EXPECT_DOUBLE_EQ(tree.find(3)->position.y, 60.0);

        tree.touch(4).anchor_point = {0, 0};
        tree.find(4)->bounds = {5, 7, 1, 1};
        EXPECT_TRUE(tree.set_frame(4, {10, 20, 30, 40}));
        EXPECT_DOUBLE_EQ(tree.find(4)->bounds.x, 5.0) << "setFrame: leaves the bounds origin alone";
        EXPECT_DOUBLE_EQ(tree.find(4)->bounds.y, 7.0);

        tree.touch(5).anchor_point = {0, 0};
        tree.find(5)->transform = macos_layer_affine::scaling(2, 3);
        EXPECT_TRUE(tree.set_frame(5, {10, 20, 30, 40}));
        EXPECT_DOUBLE_EQ(tree.find(5)->bounds.width, 15.0);
        EXPECT_NEAR(tree.find(5)->bounds.height, 40.0 / 3.0, 1e-9);

        tree.touch(6).anchor_point = {0, 0};
        EXPECT_TRUE(tree.set_frame(6, {10, 20, -30, -40}));
        EXPECT_DOUBLE_EQ(tree.find(6)->position.x, -20.0) << "a negative frame is standardized first";
        EXPECT_DOUBLE_EQ(tree.find(6)->position.y, -20.0);
        EXPECT_DOUBLE_EQ(tree.find(6)->bounds.width, 30.0);

        tree.touch(7).transform = {1, 0.5, 0.5, 1, 0, 0};
        EXPECT_FALSE(tree.set_frame(7, {0, 0, 10, 10})) << "CoreAnimation leaves a sheared frame undefined";
    }

    TEST(LayerTree, MaintainsTheHierarchyThroughEveryAttachmentForm)
    {
        macos_layer_tree tree{};

        tree.add_sublayer(1, 2);
        tree.add_sublayer(1, 3);
        EXPECT_EQ(tree.find(1)->children, (std::vector<uint64_t>{2, 3}));
        EXPECT_EQ(tree.find(2)->parent, 1u);

        tree.insert_sublayer(1, 4, 0);
        EXPECT_EQ(tree.find(1)->children, (std::vector<uint64_t>{4, 2, 3}));

        tree.insert_sublayer(1, 5, 99);
        EXPECT_EQ(tree.find(1)->children, (std::vector<uint64_t>{4, 2, 3, 5}));

        tree.insert_sublayer_relative(1, 6, 2, false);
        EXPECT_EQ(tree.find(1)->children, (std::vector<uint64_t>{4, 6, 2, 3, 5}));

        tree.insert_sublayer_relative(1, 7, 2, true);
        EXPECT_EQ(tree.find(1)->children, (std::vector<uint64_t>{4, 6, 2, 7, 3, 5}));

        tree.insert_sublayer_relative(1, 8, 999, true);
        EXPECT_EQ(tree.find(1)->children.back(), 8u) << "an unknown sibling puts an above-insert at the end";

        tree.remove_from_superlayer(2);
        EXPECT_EQ(tree.find(2)->parent, 0u);
        EXPECT_EQ(std::ranges::count(tree.find(1)->children, 2u), 0);

        tree.add_sublayer(10, 3);
        EXPECT_EQ(tree.find(3)->parent, 10u) << "attaching elsewhere reparents";
        EXPECT_EQ(std::ranges::count(tree.find(1)->children, 3u), 0);

        tree.replace_sublayers(1, {5, 4});
        EXPECT_EQ(tree.find(1)->children, (std::vector<uint64_t>{5, 4}));
        EXPECT_EQ(tree.find(6)->parent, 0u) << "a replaced-away child loses its parent";
        EXPECT_EQ(tree.find(5)->parent, 1u);

        tree.replace_sublayers(1, {});
        EXPECT_TRUE(tree.find(1)->children.empty());

        tree.add_sublayer(1, 1);
        EXPECT_TRUE(tree.find(1)->children.empty()) << "a layer cannot be its own sublayer";

        tree.add_sublayer(0, 2);
        tree.add_sublayer(1, 0);
        EXPECT_EQ(tree.find(0), nullptr);

        tree.replace_sublayers(1, {0, 1, 20});
        EXPECT_EQ(tree.find(1)->children, (std::vector<uint64_t>{20}));
    }

    TEST(LayerTree, AMaskIsNotASublayer)
    {
        macos_layer_tree tree{};
        tree.add_sublayer(1, 2);
        tree.add_sublayer(1, 3);
        ASSERT_EQ(tree.find(1)->children, (std::vector<uint64_t>{2, 3}));

        tree.set_mask(1, 3);
        EXPECT_EQ(tree.find(1)->children, (std::vector<uint64_t>{2})) << "assigning a sublayer as a mask takes it out of the sublayers";
        EXPECT_EQ(tree.find(1)->mask, 3u);
        EXPECT_EQ(tree.find(3)->parent, 1u) << "the mask's superlayer is the layer it masks";

        tree.set_mask(1, 4);
        EXPECT_EQ(tree.find(1)->mask, 4u);
        EXPECT_EQ(tree.find(3)->parent, 0u) << "the replaced mask keeps no superlayer";
        EXPECT_EQ(tree.find(1)->children, (std::vector<uint64_t>{2}));

        tree.set_mask(1, 0);
        EXPECT_EQ(tree.find(1)->mask, 0u);
        EXPECT_EQ(tree.find(4)->parent, 0u);

        tree.set_mask(1, 5);
        tree.add_sublayer(1, 5);
        EXPECT_EQ(tree.find(1)->mask, 0u) << "a layer is either a mask or a sublayer, never both";
        EXPECT_EQ(tree.find(1)->children, (std::vector<uint64_t>{2, 5}));
    }

    TEST(LayerTree, MovingAChildWithinItsParentDoesNotDuplicateIt)
    {
        macos_layer_tree tree{};
        tree.add_sublayer(1, 2);
        tree.add_sublayer(1, 3);
        tree.add_sublayer(1, 2);

        EXPECT_EQ(tree.find(1)->children, (std::vector<uint64_t>{3, 2}));
        EXPECT_EQ(std::ranges::count(tree.find(1)->children, 2u), 1);
    }

    TEST(LayerTree, RemembersTheRootLayerOfEachContext)
    {
        macos_layer_tree tree{};
        tree.set_context_root(0x1000, 0x2000);
        EXPECT_EQ(tree.root_for_context(0x1000), 0x2000u);
        EXPECT_NE(tree.find(0x2000), nullptr) << "the root is registered as a layer";
        EXPECT_EQ(tree.root_for_context(0x9999), 0u);

        tree.set_context_root(0x1000, 0x3000);
        EXPECT_EQ(tree.root_for_context(0x1000), 0x3000u);

        tree.set_context_root(0x1000, 0);
        EXPECT_EQ(tree.root_for_context(0x1000), 0u);

        tree.set_context_root(0, 0x4000);
        EXPECT_TRUE(tree.context_roots().empty());
    }

    TEST(LayerTree, AttachingARasterMakesContentsDrawable)
    {
        macos_layer_tree tree{};
        tree.attach_contents_raster(1, {0x5000, 4, 4, 16});
        EXPECT_EQ(tree.find(1)->contents.kind, macos_layer_contents_kind::raster);

        tree.attach_contents_raster(1, {});
        EXPECT_EQ(tree.find(1)->contents.kind, macos_layer_contents_kind::none);
    }

    TEST(LayerTree, MapsEveryCoreAnimationGravityName)
    {
        const std::pair<const char*, macos_layer_gravity> names[] = {
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

        for (const auto& [name, gravity] : names)
        {
            const auto parsed = sogen::macos_layer_gravity_from_name(name);
            ASSERT_TRUE(parsed.has_value()) << name;
            EXPECT_EQ(*parsed, gravity) << name;
        }

        EXPECT_FALSE(sogen::macos_layer_gravity_from_name("").has_value());
        EXPECT_FALSE(sogen::macos_layer_gravity_from_name("Resize").has_value());
        EXPECT_FALSE(sogen::macos_layer_gravity_from_name("resizeAspectFillExtra").has_value());
    }

    TEST(LayerTree, MethodTableIsCompleteAndUnique)
    {
        const auto methods = sogen::macos_layer_tree_methods();
        EXPECT_GE(methods.size(), 30u);

        std::set<std::string> seen{};
        size_t class_methods = 0;

        for (const auto& method : methods)
        {
            EXPECT_NE(method.handler, nullptr) << method.selector;
            EXPECT_FALSE(method.class_name.empty());
            EXPECT_FALSE(method.selector.empty());
            EXPECT_EQ(method.image, sogen::MACOS_QUARTZ_CORE_IMAGE_PATH);
            EXPECT_TRUE(seen.insert(method.class_name + " " + method.selector).second) << method.selector;

            if (method.class_method)
            {
                ++class_methods;
            }
        }

        EXPECT_EQ(class_methods, 1u) << "+[CATransaction flush] is the only class method";
        EXPECT_NE(handler_for("setBounds:"), nullptr);
        EXPECT_NE(handler_for("setSublayers:"), nullptr);
        EXPECT_NE(handler_for("setSourceLayer:"), nullptr);
        EXPECT_NE(handler_for("setHidesSourceLayer:"), nullptr);
        EXPECT_NE(handler_for("setMatchesPosition:"), nullptr);
        EXPECT_EQ(handler_for("setNotAThing:"), nullptr);
    }

    TEST(LayerTree, ReadsAGrayAndAnRgbCGColorOutOfGuestMemory)
    {
        const auto emu = macos_test::make_emulator();
        emu->memory.allocate_memory(object_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        const uint64_t gray_count = 2;
        const std::array<double, 2> gray{0.25, 0.5};
        emu->memory.write_memory(object_base + 0x38, &gray_count, sizeof(gray_count));
        emu->memory.write_memory(object_base + 0x48, gray.data(), sizeof(gray));

        const auto read_gray = sogen::macos_layer_read_color(*emu, object_base);
        EXPECT_TRUE(read_gray.present);
        EXPECT_DOUBLE_EQ(read_gray.r, 0.25);
        EXPECT_DOUBLE_EQ(read_gray.g, 0.25);
        EXPECT_DOUBLE_EQ(read_gray.b, 0.25);
        EXPECT_DOUBLE_EQ(read_gray.a, 0.5);

        const uint64_t rgb_count = 4;
        const std::array<double, 4> rgb{0.0, 0.0, 0.0, 0.098039215686274508};
        emu->memory.write_memory(object_base + 0x100 + 0x38, &rgb_count, sizeof(rgb_count));
        emu->memory.write_memory(object_base + 0x100 + 0x48, rgb.data(), sizeof(rgb));

        const auto read_rgb = sogen::macos_layer_read_color(*emu, object_base + 0x100);
        EXPECT_TRUE(read_rgb.present);
        EXPECT_DOUBLE_EQ(read_rgb.a, 0.098039215686274508);

        EXPECT_FALSE(sogen::macos_layer_read_color(*emu, 0).present);

        const uint64_t bad_count = 3;
        emu->memory.write_memory(object_base + 0x200 + 0x38, &bad_count, sizeof(bad_count));
        EXPECT_FALSE(sogen::macos_layer_read_color(*emu, object_base + 0x200).present);

        const uint64_t huge_count = 99;
        emu->memory.write_memory(object_base + 0x300 + 0x38, &huge_count, sizeof(huge_count));
        EXPECT_FALSE(sogen::macos_layer_read_color(*emu, object_base + 0x300).present);

        EXPECT_FALSE(sogen::macos_layer_read_color(*emu, 0xDEAD0000).present) << "unmapped memory is a refusal, not a crash";

        sogen::macos_layer_tree_release(*emu);
    }

    TEST(LayerTree, InterceptedSetterRecordsTheRectAndStillRunsTheRealMethod)
    {
        intercept_fixture fixture{"setBounds:"};
        ASSERT_TRUE(fixture.install());

        fixture.run({.x = {0xAAAA, 0xBBBB, 0, 0}, .d = {1.0, 2.0, 30.0, 40.0}});

        const auto* node = fixture.tree().find(0xAAAA);
        ASSERT_NE(node, nullptr);
        EXPECT_DOUBLE_EQ(node->bounds.x, 1.0);
        EXPECT_DOUBLE_EQ(node->bounds.y, 2.0);
        EXPECT_DOUBLE_EQ(node->bounds.width, 30.0);
        EXPECT_DOUBLE_EQ(node->bounds.height, 40.0);

        EXPECT_EQ(fixture.exit_status(), std::optional{pass_through_result})
            << "the displaced instruction and the rest of the method both ran after the observation";
    }

    TEST(LayerTree, InterceptedSettersUseTheMeasuredRegisterPlacement)
    {
        {
            intercept_fixture fixture{"setPosition:"};
            ASSERT_TRUE(fixture.install());
            fixture.run({.x = {7, 0, 0, 0}, .d = {11.5, -3.25, 0, 0}});
            EXPECT_DOUBLE_EQ(fixture.tree().find(7)->position.x, 11.5);
            EXPECT_DOUBLE_EQ(fixture.tree().find(7)->position.y, -3.25);
            EXPECT_EQ(fixture.exit_status(), std::optional{pass_through_result});
        }
        {
            intercept_fixture fixture{"setAnchorPoint:"};
            ASSERT_TRUE(fixture.install());
            fixture.run({.x = {7, 0, 0, 0}, .d = {0.25, 0.75, 0, 0}});
            EXPECT_DOUBLE_EQ(fixture.tree().find(7)->anchor_point.x, 0.25);
            EXPECT_DOUBLE_EQ(fixture.tree().find(7)->anchor_point.y, 0.75);
        }
        {
            intercept_fixture fixture{"setOpacity:"};
            ASSERT_TRUE(fixture.install());
            fixture.run({.x = {7, 0, 0, 0}, .s0 = 0.5f, .load_s0 = true});
            EXPECT_DOUBLE_EQ(fixture.tree().find(7)->opacity, 0.5);
        }
        {
            intercept_fixture fixture{"setCornerRadius:"};
            ASSERT_TRUE(fixture.install());
            fixture.run({.x = {7, 0, 0, 0}, .d = {6.0, 0, 0, 0}});
            EXPECT_DOUBLE_EQ(fixture.tree().find(7)->corner_radius, 6.0);
        }
        {
            intercept_fixture fixture{"setContentsScale:"};
            ASSERT_TRUE(fixture.install());
            fixture.run({.x = {7, 0, 0, 0}, .d = {2.0, 0, 0, 0}});
            EXPECT_DOUBLE_EQ(fixture.tree().find(7)->contents_scale, 2.0);
        }
        {
            intercept_fixture fixture{"setBorderWidth:"};
            ASSERT_TRUE(fixture.install());
            fixture.run({.x = {7, 0, 0, 0}, .d = {0.25, 0, 0, 0}});
            EXPECT_DOUBLE_EQ(fixture.tree().find(7)->border_width, 0.25);
        }
        {
            intercept_fixture fixture{"setZPosition:"};
            ASSERT_TRUE(fixture.install());
            fixture.run({.x = {7, 0, 0, 0}, .d = {-4.0, 0, 0, 0}});
            EXPECT_DOUBLE_EQ(fixture.tree().find(7)->z_position, -4.0);
        }
        {
            intercept_fixture fixture{"setContentsRect:"};
            ASSERT_TRUE(fixture.install());
            fixture.run({.x = {7, 0, 0, 0}, .d = {0.1, 0.2, 0.3, 0.4}});
            EXPECT_DOUBLE_EQ(fixture.tree().find(7)->contents_rect.width, 0.3);
        }
    }

    TEST(LayerTree, InterceptedBooleanSettersReadTheLowByteOfX2)
    {
        {
            intercept_fixture fixture{"setHidden:"};
            ASSERT_TRUE(fixture.install());
            fixture.run({.x = {7, 0, 1, 0}});
            EXPECT_TRUE(fixture.tree().find(7)->hidden);
        }
        {
            intercept_fixture fixture{"setHidden:"};
            ASSERT_TRUE(fixture.install());
            fixture.run({.x = {7, 0, 0x100, 0}});
            EXPECT_FALSE(fixture.tree().find(7)->hidden) << "only the low byte of a BOOL is significant";
        }
        {
            intercept_fixture fixture{"setMasksToBounds:"};
            ASSERT_TRUE(fixture.install());
            fixture.run({.x = {7, 0, 1, 0}});
            EXPECT_TRUE(fixture.tree().find(7)->masks_to_bounds);
        }
        {
            intercept_fixture fixture{"setGeometryFlipped:"};
            ASSERT_TRUE(fixture.install());
            fixture.run({.x = {7, 0, 1, 0}});
            EXPECT_TRUE(fixture.tree().find(7)->geometry_flipped);
        }
    }

    TEST(LayerTree, InterceptedTransformSettersFollowThePointerInX2)
    {
        {
            intercept_fixture fixture{"setAffineTransform:"};
            ASSERT_TRUE(fixture.install());
            const std::array<double, 6> affine{1.5, 0.25, -0.5, 2.0, 7.0, -3.0};
            fixture.write_doubles(0, affine);
            fixture.run({.x = {7, 0, object_base, 0}});

            const auto& transform = fixture.tree().find(7)->transform;
            EXPECT_DOUBLE_EQ(transform.a, 1.5);
            EXPECT_DOUBLE_EQ(transform.b, 0.25);
            EXPECT_DOUBLE_EQ(transform.c, -0.5);
            EXPECT_DOUBLE_EQ(transform.d, 2.0);
            EXPECT_DOUBLE_EQ(transform.tx, 7.0);
            EXPECT_DOUBLE_EQ(transform.ty, -3.0);
            EXPECT_EQ(fixture.exit_status(), std::optional{pass_through_result});
        }
        {
            intercept_fixture fixture{"setTransform:"};
            ASSERT_TRUE(fixture.install());
            std::array<double, 16> matrix{};
            matrix[0] = 2.0;
            matrix[5] = 3.0;
            matrix[10] = 4.0;
            matrix[12] = 5.0;
            matrix[13] = 6.0;
            matrix[14] = 7.0;
            matrix[15] = 1.0;
            fixture.write_doubles(0, matrix);
            fixture.run({.x = {7, 0, object_base, 0}});

            const auto& transform = fixture.tree().find(7)->transform;
            EXPECT_DOUBLE_EQ(transform.a, 2.0);
            EXPECT_DOUBLE_EQ(transform.d, 3.0);
            EXPECT_DOUBLE_EQ(transform.tx, 5.0);
            EXPECT_DOUBLE_EQ(transform.ty, 6.0) << "the z translation at index 14 is not part of the affine";
        }
        {
            intercept_fixture fixture{"setSublayerTransform:"};
            ASSERT_TRUE(fixture.install());
            std::array<double, 16> matrix{};
            matrix[0] = 2.0;
            matrix[5] = 0.5;
            matrix[15] = 1.0;
            fixture.write_doubles(0, matrix);
            fixture.run({.x = {7, 0, object_base, 0}});
            EXPECT_DOUBLE_EQ(fixture.tree().find(7)->sublayer_transform.a, 2.0);
            EXPECT_DOUBLE_EQ(fixture.tree().find(7)->sublayer_transform.d, 0.5);
        }
        {
            intercept_fixture fixture{"setAffineTransform:"};
            ASSERT_TRUE(fixture.install());
            fixture.tree().touch(7).transform = macos_layer_affine::scaling(9, 9);
            fixture.run({.x = {7, 0, 0xDEAD0000, 0}});
            EXPECT_DOUBLE_EQ(fixture.tree().find(7)->transform.a, 9.0) << "an unreadable transform leaves the layer alone";
            EXPECT_EQ(fixture.exit_status(), std::optional{pass_through_result}) << "and the real method still runs";
        }
    }

    TEST(LayerTree, InterceptedColorSettersDecodeTheGuestCGColor)
    {
        intercept_fixture fixture{"setBackgroundColor:"};
        ASSERT_TRUE(fixture.install());

        const uint64_t count = 4;
        const std::array<double, 4> components{0.1, 0.2, 0.3, 0.4};
        fixture.write_object(0x38, std::span{&count, 1});
        fixture.write_doubles(0x48, components);
        fixture.run({.x = {7, 0, object_base, 0}});

        const auto& background = fixture.tree().find(7)->background;
        EXPECT_TRUE(background.present);
        EXPECT_DOUBLE_EQ(background.r, 0.1);
        EXPECT_DOUBLE_EQ(background.g, 0.2);
        EXPECT_DOUBLE_EQ(background.b, 0.3);
        EXPECT_DOUBLE_EQ(background.a, 0.4);
    }

    TEST(LayerTree, InterceptedContentsGravityReadsTheConstantString)
    {
        {
            intercept_fixture fixture{"setContentsGravity:"};
            ASSERT_TRUE(fixture.install());

            const std::string text{"topLeft"};
            fixture.emu().memory.write_memory(object_base + 0x100, text.data(), text.size());
            const std::array<uint64_t, 4> header{0, 0x7c8, object_base + 0x100, text.size()};
            fixture.write_object(0, header);
            fixture.run({.x = {7, 0, object_base, 0}});
            EXPECT_EQ(fixture.tree().find(7)->gravity, macos_layer_gravity::top_left);
        }
        {
            intercept_fixture fixture{"setContentsGravity:"};
            ASSERT_TRUE(fixture.install());

            const std::array<uint64_t, 4> header{0, 0x123, 0, 0};
            fixture.write_object(0, header);
            fixture.tree().touch(7).gravity = macos_layer_gravity::center;
            fixture.run({.x = {7, 0, object_base, 0}});
            EXPECT_EQ(fixture.tree().find(7)->gravity, macos_layer_gravity::center) << "an unreadable string leaves the layer alone";
            EXPECT_EQ(fixture.exit_status(), std::optional{pass_through_result});
        }
    }

    TEST(LayerTree, InterceptedContentsRecordsTheObjectAsUnresolved)
    {
        intercept_fixture fixture{"setContents:"};
        ASSERT_TRUE(fixture.install());

        fixture.run({.x = {7, 0, 0xC0FFEE, 0}});
        EXPECT_EQ(fixture.tree().find(7)->contents.kind, macos_layer_contents_kind::unresolved);
        EXPECT_EQ(fixture.tree().find(7)->contents.object, 0xC0FFEEu);
    }

    TEST(LayerTree, InterceptedContentsClearsWhenHandedNil)
    {
        intercept_fixture fixture{"setContents:"};
        ASSERT_TRUE(fixture.install());

        fixture.tree().attach_contents_raster(7, {0x5000, 2, 2, 8});
        fixture.run({.x = {7, 0, 0, 0}});
        EXPECT_EQ(fixture.tree().find(7)->contents.kind, macos_layer_contents_kind::none);
    }

    TEST(LayerTree, InterceptedHierarchySettersBuildTheTree)
    {
        {
            intercept_fixture fixture{"addSublayer:"};
            ASSERT_TRUE(fixture.install());
            fixture.run({.x = {0x10, 0, 0x20, 0}});
            EXPECT_EQ(fixture.tree().find(0x10)->children, (std::vector<uint64_t>{0x20}));
            EXPECT_EQ(fixture.exit_status(), std::optional{pass_through_result});
        }
        {
            intercept_fixture fixture{"insertSublayer:atIndex:"};
            ASSERT_TRUE(fixture.install());
            fixture.tree().add_sublayer(0x10, 0x20);
            fixture.run({.x = {0x10, 0, 0x30, 0}});
            EXPECT_EQ(fixture.tree().find(0x10)->children, (std::vector<uint64_t>{0x30, 0x20}));
        }
        {
            intercept_fixture fixture{"insertSublayer:above:"};
            ASSERT_TRUE(fixture.install());
            fixture.tree().add_sublayer(0x10, 0x20);
            fixture.run({.x = {0x10, 0, 0x30, 0x20}});
            EXPECT_EQ(fixture.tree().find(0x10)->children, (std::vector<uint64_t>{0x20, 0x30}));
        }
        {
            intercept_fixture fixture{"removeFromSuperlayer"};
            ASSERT_TRUE(fixture.install());
            fixture.tree().add_sublayer(0x10, 0x20);
            fixture.run({.x = {0x20, 0, 0, 0}});
            EXPECT_TRUE(fixture.tree().find(0x10)->children.empty());
        }
    }

    // Every layout is measured against the arrays three real AppKit windows hand to
    // -[CALayer setSublayers:].
    TEST(LayerTree, InterceptedSetSublayersEnumeratesEveryMeasuredArrayClass)
    {
        {
            intercept_fixture fixture{"setSublayers:"};
            ASSERT_TRUE(fixture.install());

            const auto array_class = fixture.write_objc_class("__NSArrayI", true, false);
            const std::array<uint64_t, 5> array{array_class, 3, 0x1000, 0x2000, 0x3000};
            fixture.write_object(0, array);
            fixture.run({.x = {0x10, 0, object_base, 0}});
            EXPECT_EQ(children_of(fixture, 0x10), (std::vector<uint64_t>{0x1000, 0x2000, 0x3000}))
                << "__NSArrayI stores its count at +0x08 and its objects inline from +0x10";
        }
        {
            intercept_fixture fixture{"setSublayers:"};
            ASSERT_TRUE(fixture.install());

            const auto array_class = fixture.write_objc_class("__NSArrayI_Transfer", true, false);
            const std::array<uint64_t, 3> array{array_class, 2, object_base + element_storage_offset};
            const std::array<uint64_t, 2> elements{0x1000, 0x2000};
            fixture.write_object(0, array);
            fixture.write_object(element_storage_offset, elements);
            fixture.run({.x = {0x10, 0, object_base, 0}});
            EXPECT_EQ(children_of(fixture, 0x10), (std::vector<uint64_t>{0x1000, 0x2000}))
                << "__NSArrayI_Transfer keeps its objects behind the pointer at +0x10";
        }
        {
            intercept_fixture fixture{"setSublayers:"};
            ASSERT_TRUE(fixture.install());

            const auto array_class = fixture.write_objc_class("__NSSingleObjectArrayI", true, false);
            const std::array<uint64_t, 2> array{array_class, 0x100020000ULL};
            fixture.write_object(0, array);
            fixture.run({.x = {0x10, 0, object_base, 0}});
            EXPECT_EQ(children_of(fixture, 0x10), (std::vector<uint64_t>{0x100020000ULL}))
                << "a single-object array stores the object where a count would be";
        }
        {
            intercept_fixture fixture{"setSublayers:"};
            ASSERT_TRUE(fixture.install());

            const auto array_class = fixture.write_objc_class("__NSArrayM", true, false);
            const std::array<uint64_t, 5> array{array_class, 0, object_base + element_storage_offset, (uint64_t{4} << 32) | 1,
                                                uint64_t{2} << 32};
            const std::array<uint64_t, 4> elements{0x1000, 0x2000, 0x3000, 0x4000};
            fixture.write_object(0, array);
            fixture.write_object(element_storage_offset, elements);
            fixture.run({.x = {0x10, 0, object_base, 0}});
            EXPECT_EQ(children_of(fixture, 0x10), (std::vector<uint64_t>{0x2000, 0x3000}))
                << "__NSArrayM is a deque: _used elements start _offset slots into _list";
        }
        {
            intercept_fixture fixture{"setSublayers:"};
            ASSERT_TRUE(fixture.install());

            const auto array_class = fixture.write_objc_class("__NSFrozenArrayM", true, false);
            const std::array<uint64_t, 5> array{array_class, 0x9999, object_base + element_storage_offset, (uint64_t{2} << 32) | 0,
                                                uint64_t{2} << 32};
            const std::array<uint64_t, 2> elements{0x1000, 0x2000};
            fixture.write_object(0, array);
            fixture.write_object(element_storage_offset, elements);
            fixture.run({.x = {0x10, 0, object_base, 0}});
            EXPECT_EQ(children_of(fixture, 0x10), (std::vector<uint64_t>{0x1000, 0x2000}))
                << "__NSFrozenArrayM shares __NSArrayM's storage layout and differs only at +0x08";
        }
        {
            intercept_fixture fixture{"setSublayers:"};
            ASSERT_TRUE(fixture.install());

            const auto array_class = fixture.write_swift_class("_ContiguousArrayStorage");
            const std::array<uint64_t, 6> array{array_class, 0, 3, (uint64_t{3} << 1) | 1, 0x1000, 0x2000};
            const std::array<uint64_t, 1> tail{0x3000};
            fixture.write_object(0, array);
            fixture.write_object(0x30, tail);
            fixture.run({.x = {0x10, 0, object_base, 0}});
            EXPECT_EQ(children_of(fixture, 0x10), (std::vector<uint64_t>{0x1000, 0x2000, 0x3000}))
                << "a bridged Swift array counts at +0x10 and stores its elements inline from +0x20";
        }
        {
            intercept_fixture fixture{"setSublayers:"};
            ASSERT_TRUE(fixture.install());

            // arm64e signs a raw isa too, and a Swift class instance stores one: the nonpointer bit is
            // clear and the bits above the class pointer are a signature.
            constexpr uint64_t isa_signature = 0x003C000000000000ULL;
            const auto array_class = fixture.write_swift_class("_ContiguousArrayStorage");
            const std::array<uint64_t, 6> array{array_class | isa_signature, 0, 2, (uint64_t{2} << 1) | 1, 0x1000, 0x2000};
            fixture.write_object(0, array);
            fixture.run({.x = {0x10, 0, object_base, 0}});
            EXPECT_EQ(children_of(fixture, 0x10), (std::vector<uint64_t>{0x1000, 0x2000}))
                << "a signed raw isa names its class once the signature is masked off";
        }
        {
            intercept_fixture fixture{"setSublayers:"};
            ASSERT_TRUE(fixture.install());

            const auto deferred_class = fixture.write_swift_class("__SwiftDeferredNSArray");
            const auto storage_class = fixture.write_second_swift_class("_ContiguousArrayStorage");
            const std::array<uint64_t, 4> deferred{deferred_class, 0, 0, object_base + deferred_storage_offset};
            const std::array<uint64_t, 7> storage{storage_class, 0, 3, (uint64_t{3} << 1) | 1, 0x1000, 0x2000, 0x3000};
            fixture.write_object(0, deferred);
            fixture.write_object(deferred_storage_offset, storage);
            fixture.run({.x = {0x10, 0, object_base, 0}});
            EXPECT_EQ(children_of(fixture, 0x10), (std::vector<uint64_t>{0x1000, 0x2000, 0x3000}))
                << "a lazily bridged Swift array keeps its elements in _nativeStorage at +0x18 while "
                   "_heapBufferBridged at +0x10 is still nil";
        }
        {
            intercept_fixture fixture{"setSublayers:"};
            ASSERT_TRUE(fixture.install());
            fixture.tree().add_sublayer(0x10, 0x20);

            const auto deferred_class = fixture.write_swift_class("__SwiftDeferredNSArray");
            const std::array<uint64_t, 4> deferred{deferred_class, 0, 0, 0};
            fixture.write_object(0, deferred);
            fixture.run({.x = {0x10, 0, object_base, 0}});
            EXPECT_EQ(children_of(fixture, 0x10), (std::vector<uint64_t>{0x20}))
                << "a deferred array with no native storage is refused rather than read as empty";
        }
        {
            intercept_fixture fixture{"setSublayers:"};
            ASSERT_TRUE(fixture.install());
            fixture.tree().add_sublayer(0x10, 0x20);

            const auto array_class = fixture.write_swift_class("__EmptyArrayStorage");
            const std::array<uint64_t, 2> array{array_class, 0};
            fixture.write_object(0, array);
            fixture.run({.x = {0x10, 0, object_base, 0}});
            EXPECT_TRUE(children_of(fixture, 0x10).empty()) << "Swift's empty-array singleton clears the sublayers";
        }
        {
            intercept_fixture fixture{"setSublayers:"};
            ASSERT_TRUE(fixture.install());
            fixture.tree().add_sublayer(0x10, 0x20);

            const auto array_class = fixture.write_objc_class("__NSArray0", true, false);
            const std::array<uint64_t, 2> array{array_class, 0};
            fixture.write_object(0, array);
            fixture.run({.x = {0x10, 0, object_base, 0}});
            EXPECT_TRUE(children_of(fixture, 0x10).empty()) << "an empty array clears the sublayers";
        }
        {
            intercept_fixture fixture{"setSublayers:"};
            ASSERT_TRUE(fixture.install());
            fixture.tree().add_sublayer(0x10, 0x20);

            const auto array_class = fixture.write_objc_class("__NSArrayM", true, false);
            const std::array<uint64_t, 5> array{array_class, 0, object_base + element_storage_offset, uint64_t{4} << 32, 0};
            fixture.write_object(0, array);
            fixture.run({.x = {0x10, 0, object_base, 0}});
            EXPECT_TRUE(children_of(fixture, 0x10).empty()) << "a mutable array with _used 0 clears the sublayers";
        }
    }

    TEST(LayerTree, InterceptedSetSublayersFindsTheClassNameThroughEveryRuntimeForm)
    {
        {
            intercept_fixture fixture{"setSublayers:"};
            ASSERT_TRUE(fixture.install());

            const auto array_class = fixture.write_objc_class("__NSArrayI", true, true);
            const std::array<uint64_t, 3> array{array_class, 1, 0x1000};
            fixture.write_object(0, array);
            fixture.run({.x = {0x10, 0, object_base, 0}});
            EXPECT_EQ(children_of(fixture, 0x10), (std::vector<uint64_t>{0x1000}))
                << "a realized class can reach its class_ro_t through a tagged class_rw_ext_t";
        }
        {
            intercept_fixture fixture{"setSublayers:"};
            ASSERT_TRUE(fixture.install());

            const auto array_class = fixture.write_objc_class("__NSArrayI", false, false);
            const std::array<uint64_t, 3> array{array_class, 1, 0x1000};
            fixture.write_object(0, array);
            fixture.run({.x = {0x10, 0, object_base, 0}});
            EXPECT_EQ(children_of(fixture, 0x10), (std::vector<uint64_t>{0x1000}))
                << "an unrealized class points at its class_ro_t directly";
        }
        {
            intercept_fixture fixture{"setSublayers:"};
            ASSERT_TRUE(fixture.install());

            fixture.write_objc_class("__NSArrayI", true, false);
            fixture.sign_class_pointers();

            const std::array<uint64_t, 3> array{object_base + objc_class_offset, 1, 0x1000};
            fixture.write_object(0, array);
            fixture.run({.x = {0x10, 0, object_base, 0}});
            EXPECT_EQ(children_of(fixture, 0x10), (std::vector<uint64_t>{0x1000}))
                << "class metadata pointers carry an arm64e signature above the 47-bit address";
        }
        {
            intercept_fixture fixture{"setSublayers:"};
            ASSERT_TRUE(fixture.install());

            const auto array_class = fixture.write_objc_class("__NSArrayI", true, false);

            // A real isa measured under sogen: shiftcls is 33 bits wide, and everything above it is
            // retain-count bookkeeping. Calculator's arrays reach setSublayers: at a retain count high
            // enough to set bits 48 and up, which a mask any wider than this would read as address.
            const std::array<uint64_t, 3> array{0x013D000000000001ULL | array_class, 1, 0x1000};
            fixture.write_object(0, array);
            fixture.run({.x = {0x10, 0, object_base, 0}});
            EXPECT_EQ(children_of(fixture, 0x10), (std::vector<uint64_t>{0x1000}))
                << "a non-pointer isa carries the class in shiftcls, below the retain-count bits";
        }
    }

    TEST(LayerTree, InterceptedSetSublayersRefusesRatherThanClearingWhatItCannotRead)
    {
        {
            intercept_fixture fixture{"setSublayers:"};
            ASSERT_TRUE(fixture.install());
            fixture.tree().add_sublayer(0x10, 0x20);

            const auto array_class = fixture.write_objc_class("NSSomethingElse", true, false);
            const std::array<uint64_t, 3> array{array_class, 1, 0x1000};
            fixture.write_object(0, array);
            fixture.run({.x = {0x10, 0, object_base, 0}});
            EXPECT_EQ(children_of(fixture, 0x10), (std::vector<uint64_t>{0x20}))
                << "an array class whose storage sogen has not measured keeps the previous sublayers";
            EXPECT_EQ(fixture.exit_status(), std::optional{pass_through_result});
        }
        {
            intercept_fixture fixture{"setSublayers:"};
            ASSERT_TRUE(fixture.install());
            fixture.tree().add_sublayer(0x10, 0x20);

            const std::array<uint64_t, 3> array{0, 1, 0x1000};
            fixture.write_object(0, array);
            fixture.run({.x = {0x10, 0, object_base, 0}});
            EXPECT_EQ(children_of(fixture, 0x10), (std::vector<uint64_t>{0x20}))
                << "an object whose isa names no class keeps the previous sublayers";
        }
        {
            intercept_fixture fixture{"setSublayers:"};
            ASSERT_TRUE(fixture.install());
            fixture.tree().add_sublayer(0x10, 0x20);

            const auto array_class = fixture.write_objc_class("__NSArrayI", true, false);
            const std::array<uint64_t, 4> array{array_class, 2, 0x1000, 3};
            fixture.write_object(0, array);
            fixture.run({.x = {0x10, 0, object_base, 0}});
            EXPECT_EQ(children_of(fixture, 0x10), (std::vector<uint64_t>{0x20}))
                << "an array whose entries are not object pointers is refused, not guessed at";
        }
        {
            intercept_fixture fixture{"setSublayers:"};
            ASSERT_TRUE(fixture.install());
            fixture.tree().add_sublayer(0x10, 0x20);

            const auto array_class = fixture.write_objc_class("__NSArrayM", true, false);
            const std::array<uint64_t, 5> array{array_class, 0, object_base + element_storage_offset, (uint64_t{2} << 32) | 1,
                                                uint64_t{2} << 32};
            const std::array<uint64_t, 2> elements{0x1000, 0x2000};
            fixture.write_object(0, array);
            fixture.write_object(element_storage_offset, elements);
            fixture.run({.x = {0x10, 0, object_base, 0}});
            EXPECT_EQ(children_of(fixture, 0x10), (std::vector<uint64_t>{0x20}))
                << "a deque whose _offset plus _used runs past _size is refused";
        }
        {
            intercept_fixture fixture{"setSublayers:"};
            ASSERT_TRUE(fixture.install());
            fixture.tree().add_sublayer(0x10, 0x20);
            fixture.run({.x = {0x10, 0, 0xDEAD0000, 0}});
            EXPECT_EQ(children_of(fixture, 0x10), (std::vector<uint64_t>{0x20}))
                << "an array outside mapped memory keeps the previous sublayers";
        }
    }

    TEST(LayerTree, InterceptedSetMaskRecordsTheMaskAndUnparentsIt)
    {
        intercept_fixture fixture{"setMask:"};
        ASSERT_TRUE(fixture.install());
        fixture.tree().add_sublayer(0x10, 0x20);

        fixture.run({.x = {0x10, 0, 0x20, 0}});
        EXPECT_EQ(fixture.tree().find(0x10)->mask, 0x20u);
        EXPECT_TRUE(fixture.tree().find(0x10)->children.empty());
        EXPECT_EQ(fixture.exit_status(), std::optional{pass_through_result});
    }

    TEST(LayerTree, InterceptedPortalSettersRecordTheSourceAndItsVisibility)
    {
        {
            intercept_fixture fixture{"setSourceLayer:"};
            ASSERT_TRUE(fixture.install());
            fixture.tree().touch(0x10);

            fixture.run({.x = {0x10, 0, 0x20, 0}});
            EXPECT_EQ(fixture.tree().find(0x10)->portal_source, 0x20u);
            EXPECT_EQ(fixture.exit_status(), std::optional{pass_through_result});
        }
        {
            intercept_fixture fixture{"setHidesSourceLayer:"};
            ASSERT_TRUE(fixture.install());
            fixture.tree().touch(0x10);

            fixture.run({.x = {0x10, 0, 1, 0}});
            EXPECT_TRUE(fixture.tree().find(0x10)->hides_source_layer);
        }
        {
            intercept_fixture fixture{"setHidesSourceLayer:"};
            ASSERT_TRUE(fixture.install());
            fixture.tree().touch(0x10).hides_source_layer = true;

            fixture.run({.x = {0x10, 0, 0, 0}});
            EXPECT_FALSE(fixture.tree().find(0x10)->hides_source_layer);
        }
        {
            intercept_fixture fixture{"setMatchesPosition:"};
            ASSERT_TRUE(fixture.install());
            fixture.tree().touch(0x10);

            fixture.run({.x = {0x10, 0, 1, 0}});
            EXPECT_TRUE(fixture.tree().find(0x10)->portal_matches_position);
        }
    }

    // CGPath offsets and the kind tag are measured in section 14 of
    // The application icon is the one name sogen refuses, because the iconservices agent that would
    // draw it does not exist here and AppKit asserts rather than degrading when its reply is nil.
    // Every other name reaches the real +[NSImage imageNamed:], which the fake method reports as 0x56.
    TEST(AppKitIntercept, AnswersNilOnlyForTheApplicationIcon)
    {
        {
            intercept_fixture fixture{"imageNamed:"};
            ASSERT_TRUE(fixture.install());
            fixture.write_word(0x08, 0x7c8);
            fixture.write_word(0x10, object_base + 0x40);
            fixture.write_word(0x18, std::string_view{"NSApplicationIcon"}.size());
            fixture.write_string(0x40, "NSApplicationIcon");

            fixture.run({.x = {0, 0, object_base, 0}});
            EXPECT_EQ(fixture.returned(), 0u) << "the icon the absent agent would have drawn is refused";
        }
        {
            intercept_fixture fixture{"imageNamed:"};
            ASSERT_TRUE(fixture.install());
            fixture.write_word(0x08, 0x7c8);
            fixture.write_word(0x10, object_base + 0x40);
            fixture.write_word(0x18, std::string_view{"NSCaution"}.size());
            fixture.write_string(0x40, "NSCaution");

            fixture.run({.x = {0, 0, object_base, 0}});
            EXPECT_EQ(fixture.returned(), 0x56u) << "every other name reaches the real implementation";
        }
        {
            intercept_fixture fixture{"imageNamed:"};
            ASSERT_TRUE(fixture.install());

            fixture.run({.x = {0, 0, 0, 0}});
            EXPECT_EQ(fixture.returned(), 0x56u) << "a name sogen cannot read is passed through rather than guessed at";
        }
    }

    TEST(LayerTree, ReadsTheClosedFormsOfACGPathOutOfGuestMemory)
    {
        auto emu = macos_test::make_emulator();
        emu->memory.allocate_memory(object_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        const auto write = [&](const uint64_t kind, const std::array<double, 6>& matrix, const std::array<double, 2>& corner) {
            emu->memory.write_memory(object_base + 0x10, &kind, sizeof(kind));
            emu->memory.write_memory(object_base + 0x18, matrix.data(), sizeof(matrix));
            emu->memory.write_memory(object_base + 0x48, corner.data(), sizeof(corner));
        };

        write(1, {7, 0, 0, 8, 5, 6}, {0, 0});
        auto shape = sogen::macos_layer_read_shape_path(*emu, object_base);
        EXPECT_EQ(shape.kind, sogen::macos_layer_shape_kind::rect);
        EXPECT_DOUBLE_EQ(shape.transform.a, 7.0);
        EXPECT_DOUBLE_EQ(shape.transform.d, 8.0);
        EXPECT_DOUBLE_EQ(shape.transform.tx, 5.0) << "a creation transform is folded into the affine";
        EXPECT_DOUBLE_EQ(shape.transform.ty, 6.0);

        write(2, {40, 0, 0, 30, 2, 3}, {0.175, 0.3});
        shape = sogen::macos_layer_read_shape_path(*emu, object_base);
        EXPECT_EQ(shape.kind, sogen::macos_layer_shape_kind::rounded_rect);
        EXPECT_DOUBLE_EQ(shape.corner_x, 0.175) << "corner radii are fractions of the shape's width and height";
        EXPECT_DOUBLE_EQ(shape.corner_y, 0.3);

        write(4, {60, 0, 0, 20, 4, 5}, {0, 0});
        shape = sogen::macos_layer_read_shape_path(*emu, object_base);
        EXPECT_EQ(shape.kind, sogen::macos_layer_shape_kind::ellipse);
        EXPECT_DOUBLE_EQ(shape.transform.a, 60.0);

        write(9, {1, 0, 0, 1, 0, 0}, {0, 0});
        shape = sogen::macos_layer_read_shape_path(*emu, object_base);
        EXPECT_EQ(shape.kind, sogen::macos_layer_shape_kind::unmodelled)
            << "an element-list path whose counts do not decode is named unmodelled rather than guessed at";
        EXPECT_TRUE(shape.transform.is_identity()) << "and carries no transform a caller might act on";

        EXPECT_EQ(sogen::macos_layer_read_shape_path(*emu, 0).kind, sogen::macos_layer_shape_kind::none);
        sogen::macos_layer_tree_release(*emu);
    }

    // The two element-list forms, laid out as CGPathApply reported them on 25G76 over ten paths
    // including the operator keys' own glyph outlines (src/tools/macos-gui-probe/pathprobe.c).
    TEST(LayerTree, ReadsTheElementListFormsOfACGPathOutOfGuestMemory)
    {
        auto emu = macos_test::make_emulator();
        emu->memory.allocate_memory(object_base, sogen::MACOS_PAGE_SIZE * 2, sogen::memory_permission::read_write);

        const uint64_t inline_kind = 8;
        const std::array<uint16_t, 2> counts{3, 3};
        const uint32_t packed_types = 0x108;
        const std::array<double, 6> inline_points{1, 2, 3, 4, 1, 2};
        emu->memory.write_memory(object_base + 0x10, &inline_kind, sizeof(inline_kind));
        emu->memory.write_memory(object_base + 0x18, counts.data(), sizeof(counts));
        emu->memory.write_memory(object_base + 0x1c, &packed_types, sizeof(packed_types));
        emu->memory.write_memory(object_base + 0x20, inline_points.data(), sizeof(inline_points));

        auto shape = sogen::macos_layer_read_shape_path(*emu, object_base);
        ASSERT_EQ(shape.kind, sogen::macos_layer_shape_kind::path);
        ASSERT_EQ(shape.edges.size(), 2u) << "a move, a line and a close are one segment plus the closing one";
        EXPECT_DOUBLE_EQ(shape.edges.front().from.x, 1.0);
        EXPECT_DOUBLE_EQ(shape.edges.front().from.y, 2.0);
        EXPECT_DOUBLE_EQ(shape.edges.front().to.x, 3.0);
        EXPECT_DOUBLE_EQ(shape.edges.back().to.x, 1.0) << "an open subpath is closed the way CoreGraphics fills it";
        EXPECT_DOUBLE_EQ(shape.edges.back().to.y, 2.0);

        const uint64_t heap_kind = 9;
        const uint64_t point_count = 8;
        const uint64_t element_count = 5;
        const uint64_t types_end = 0x18c;
        const uint64_t buffer = object_base + sogen::MACOS_PAGE_SIZE;
        const std::array<double, 16> heap_points{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 1, 2};
        const std::array<uint8_t, 5> reversed_types{4, 3, 2, 1, 0};

        emu->memory.write_memory(object_base + 0x10, &heap_kind, sizeof(heap_kind));
        emu->memory.write_memory(object_base + 0x18, &point_count, sizeof(point_count));
        emu->memory.write_memory(object_base + 0x20, &element_count, sizeof(element_count));
        emu->memory.write_memory(object_base + 0x28, &types_end, sizeof(types_end));
        emu->memory.write_memory(object_base + 0x30, &buffer, sizeof(buffer));
        emu->memory.write_memory(buffer, heap_points.data(), sizeof(heap_points));
        emu->memory.write_memory(buffer + types_end - reversed_types.size(), reversed_types.data(), reversed_types.size());

        shape = sogen::macos_layer_read_shape_path(*emu, object_base);
        ASSERT_EQ(shape.kind, sogen::macos_layer_shape_kind::path) << "the types are stored descending from the offset at +0x28";
        EXPECT_GT(shape.edges.size(), 4u) << "the quadratic and the cubic are flattened into several segments each";
        EXPECT_DOUBLE_EQ(shape.edges.front().from.x, 1.0);
        EXPECT_DOUBLE_EQ(shape.edges.front().from.y, 2.0);
        EXPECT_DOUBLE_EQ(shape.edges.back().from.x, 13.0) << "the cubic ends on its last control point";
        EXPECT_DOUBLE_EQ(shape.edges.back().to.x, 1.0);

        const uint64_t past_the_buffer = 8;
        emu->memory.write_memory(object_base + 0x28, &past_the_buffer, sizeof(past_the_buffer));
        EXPECT_EQ(sogen::macos_layer_read_shape_path(*emu, object_base).kind, sogen::macos_layer_shape_kind::unmodelled)
            << "a types offset that would overlap the points is refused rather than read";

        sogen::macos_layer_tree_release(*emu);
    }

    TEST(LayerTree, InterceptedShapeSettersRecordThePathAndItsPaint)
    {
        {
            intercept_fixture fixture{"setPath:"};
            ASSERT_TRUE(fixture.install());

            const std::array<uint64_t, 2> kind{0, 4};
            const std::array<double, 6> matrix{10, 0, 0, 20, 1, 2};
            fixture.write_object(0x08, kind);
            fixture.write_doubles(0x18, matrix);
            fixture.run({.x = {0x10, 0, object_base, 0}});

            const auto& shape = fixture.tree().find(0x10)->shape;
            EXPECT_EQ(shape.kind, sogen::macos_layer_shape_kind::ellipse);
            EXPECT_DOUBLE_EQ(shape.transform.d, 20.0);
            EXPECT_EQ(fixture.exit_status(), std::optional{pass_through_result});
        }
        {
            intercept_fixture fixture{"setFillColor:"};
            ASSERT_TRUE(fixture.install());

            const std::array<uint64_t, 1> count{4};
            const std::array<double, 4> components{1.0, 0.5, 0.0, 1.0};
            fixture.write_object(0x38, count);
            fixture.write_doubles(0x48, components);
            fixture.run({.x = {0x10, 0, object_base, 0}});

            const auto& fill = fixture.tree().find(0x10)->shape.fill;
            EXPECT_TRUE(fill.present);
            EXPECT_DOUBLE_EQ(fill.r, 1.0);
            EXPECT_DOUBLE_EQ(fill.g, 0.5);
        }
        {
            intercept_fixture fixture{"setLineWidth:"};
            ASSERT_TRUE(fixture.install());
            fixture.run({.x = {0x10, 0, 0, 0}, .d = {3.5, 0, 0, 0}});
            EXPECT_DOUBLE_EQ(fixture.tree().find(0x10)->shape.line_width, 3.5);
        }
    }

    TEST(LayerTree, InterceptedContextSetLayerRegistersTheRoot)
    {
        intercept_fixture fixture{"setLayer:"};
        ASSERT_TRUE(fixture.install());

        fixture.run({.x = {0xC1, 0, 0x9000, 0}});
        EXPECT_EQ(fixture.tree().root_for_context(0xC1), 0x9000u);
        EXPECT_EQ(fixture.exit_status(), std::optional{pass_through_result});
    }

    TEST(LayerTree, InterceptedFlushCountsAndStillReturnsToTheCaller)
    {
        intercept_fixture fixture{"flush"};
        ASSERT_TRUE(fixture.install());

        fixture.run({});
        EXPECT_EQ(fixture.tree().flush_count(), 1u);
        EXPECT_EQ(fixture.exit_status(), std::optional{pass_through_result});
    }

    TEST(LayerTree, InstallRefusesAPcRelativeFirstInstruction)
    {
        const uint32_t refused[] = {
            0x90000000, // adrp x0, #0
            0x10000000, // adr  x0, #0
            0x14000001, // b    #4
            0x94000001, // bl   #4
            0x54000020, // b.eq #4
            0xB4000020, // cbz  x0, #4
            0x37000020, // tbnz w0, #0, #4
            0x58000020, // ldr  x0, #4
        };

        for (const auto instruction : refused)
        {
            const auto emu = macos_test::make_emulator();
            sogen::macos_native_dispatch dispatch{};
            emu->memory.allocate_memory(imp_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::all);
            emu->memory.write_memory(imp_base, &instruction, sizeof(instruction));

            EXPECT_FALSE(sogen::macos_layer_tree_install(*emu, dispatch, imp_base, "test-method", handler_for("setBounds:")))
                << std::hex << instruction;

            uint32_t after = 0;
            emu->memory.read_memory(imp_base, &after, sizeof(after));
            EXPECT_EQ(after, instruction) << "a refused method is left unpatched rather than left broken";
            EXPECT_FALSE(dispatch.handles(imp_base));

            sogen::macos_layer_tree_release(*emu);
        }
    }

    TEST(LayerTree, InstallRefusesBadArgumentsAndAnAlreadyTrappedEntry)
    {
        const auto emu = macos_test::make_emulator();
        sogen::macos_native_dispatch dispatch{};
        emu->memory.allocate_memory(imp_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::all);
        emu->memory.write_memory(imp_base, &displaced_instruction, sizeof(displaced_instruction));

        EXPECT_FALSE(sogen::macos_layer_tree_install(*emu, dispatch, imp_base, "test", nullptr));
        EXPECT_FALSE(sogen::macos_layer_tree_install(*emu, dispatch, 0, "test", handler_for("setBounds:")));
        EXPECT_FALSE(sogen::macos_layer_tree_install(*emu, dispatch, imp_base + 2, "test", handler_for("setBounds:")));
        EXPECT_FALSE(sogen::macos_layer_tree_install(*emu, dispatch, 0xDEAD0000, "test", handler_for("setBounds:")));

        EXPECT_TRUE(sogen::macos_layer_tree_install(*emu, dispatch, imp_base, "test", handler_for("setBounds:")));
        EXPECT_FALSE(sogen::macos_layer_tree_install(*emu, dispatch, imp_base, "test", handler_for("setBounds:")))
            << "relocating a trap that is already installed would relocate the trap itself";

        sogen::macos_layer_tree_release(*emu);
    }

    TEST(LayerTree, TheTrampolineJumpsBackIntoTheMethodBody)
    {
        const auto emu = macos_test::make_emulator();
        sogen::macos_native_dispatch dispatch{};
        emu->memory.allocate_memory(imp_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::all);

        const std::array<uint32_t, 3> method{displaced_instruction, body_instruction, ret_instruction};
        emu->memory.write_memory(imp_base, method.data(), sizeof(method));
        ASSERT_TRUE(sogen::macos_layer_tree_install(*emu, dispatch, imp_base, "test", handler_for("setBounds:")));

        uint32_t patched = 0;
        emu->memory.read_memory(imp_base, &patched, sizeof(patched));
        EXPECT_EQ(patched, sogen::MACOS_ARM64_SVC_80);

        std::array<uint32_t, 4> trampoline{};
        emu->memory.read_memory(sogen::MACOS_LAYER_TRAMPOLINE_BASE, trampoline.data(), sizeof(trampoline));
        EXPECT_EQ(trampoline[0], displaced_instruction);
        EXPECT_EQ(trampoline[1], 0x58000070u) << "ldr x16, #12";
        EXPECT_EQ(trampoline[2], 0xD61F0200u) << "br x16";

        uint64_t resume = 0;
        emu->memory.read_memory(sogen::MACOS_LAYER_TRAMPOLINE_BASE + 0x10, &resume, sizeof(resume));
        EXPECT_EQ(resume, imp_base + 4);

        sogen::macos_layer_tree_release(*emu);
    }

    namespace
    {
        // An arm64e prologue signs the return address with pacibsp before it builds its frame and
        // authenticates it with retab after tearing it down. Both use SP as the modifier, so the sequence
        // only round-trips when SP is identical at the two points.
        constexpr uint32_t PACIBSP = 0xD503237F;
        constexpr uint32_t SUB_SP_32 = 0xD10083FF;
        constexpr uint32_t ADD_SP_32 = 0x910083FF;
        constexpr uint32_t MOV_X0_0x56 = 0xD2800AC0;
        constexpr uint32_t RETAB = 0xD65F0FFF;

        std::optional<int> call_method(const std::span<const uint32_t> method, const bool intercept)
        {
            const auto emu = macos_test::make_emulator();
            sogen::macos_native_dispatch dispatch{};
            emu->memory.allocate_memory(imp_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::all);
            emu->memory.write_memory(imp_base, method.data(), method.size() * sizeof(uint32_t));

            if (intercept && !sogen::macos_layer_tree_install(*emu, dispatch, imp_base, "test", handler_for("setBounds:")))
            {
                sogen::macos_layer_tree_release(*emu);
                return std::nullopt;
            }

            std::vector<uint32_t> code{
                0x94000000,
                0xD2800030, // mov x16, #1
                0xD4001001, // svc #0x80
            };
            code[0] = 0x94000000u | (static_cast<uint32_t>((imp_base - code_base) / 4) & 0x03FFFFFFu);

            macos_test::write_guest_code(*emu, code_base, code);
            emu->set_native_dispatch(&dispatch);

            try
            {
                emu->start();
            }
            catch (const std::exception&)
            {
                sogen::macos_layer_tree_release(*emu);
                return std::nullopt;
            }

            const auto status = emu->process.exit_status;
            sogen::macos_layer_tree_release(*emu);
            return status;
        }
    }

    // The pass-through trampoline runs the method's own first instruction at the trampoline rather than
    // at imp. For an arm64e method that instruction is pacibsp, so the trap has to leave SP and lr
    // exactly as the caller left them: anything that moves SP between the pacibsp and the matching retab
    // poisons the return address instead of restoring it, and the poisoned value lands in pc.
    TEST(LayerTree, ThePassThroughTrampolineKeepsAnArm64eReturnAuthenticating)
    {
        constexpr std::array<uint32_t, 5> balanced{PACIBSP, SUB_SP_32, MOV_X0_0x56, ADD_SP_32, RETAB};
        constexpr std::array<uint32_t, 4> unbalanced{PACIBSP, SUB_SP_32, MOV_X0_0x56, RETAB};

        EXPECT_EQ(call_method(balanced, false), pass_through_result) << "the method authenticates when it is not intercepted";
        EXPECT_EQ(call_method(balanced, true), pass_through_result)
            << "the same method authenticates through the trampoline, so the trap preserved SP and lr";

        // Without this the positive case would pass on an emulator that treats pacibsp and retab as
        // no-ops, and would say nothing about the trampoline.
        EXPECT_NE(call_method(unbalanced, false), pass_through_result)
            << "a retab whose SP does not match its pacibsp must fail, or this emulator is not checking PAC at all";
    }

    TEST(LayerTree, ReleasingAnEmulatorDropsItsTree)
    {
        const auto emu = macos_test::make_emulator();
        sogen::macos_layer_tree_of(*emu).touch(0x1234);
        EXPECT_EQ(sogen::macos_layer_tree_of(*emu).size(), 1u);

        sogen::macos_layer_tree_release(*emu);
        EXPECT_EQ(sogen::macos_layer_tree_of(*emu).size(), 0u);

        sogen::macos_layer_tree_release(*emu);
    }

    TEST(LayerTree, PresentDoesNothingWithoutAWindowBoundToALayerContext)
    {
        const auto emu = macos_test::make_emulator();
        EXPECT_EQ(sogen::macos_layer_tree_present(*emu), 0u);

        auto* window = emu->ui.server.create_window(emu->ui.server.main_connection(), 0, 0, 8, 8);
        ASSERT_NE(window, nullptr);
        EXPECT_EQ(sogen::macos_layer_tree_present(*emu), 0u) << "a window with no layer context has nothing to composite";

        window->layer_context = 0xC1;
        EXPECT_EQ(sogen::macos_layer_tree_present(*emu), 0u) << "a layer context with no root layer has nothing to composite";

        sogen::macos_layer_tree_release(*emu);
    }

    TEST(LayerTree, PresentCompositesTheRootLayerIntoTheWindowBackingStore)
    {
        const auto emu = macos_test::make_emulator();
        auto& tree = sogen::macos_layer_tree_of(*emu);

        auto* window = emu->ui.server.create_window(emu->ui.server.main_connection(), 0, 0, 4, 4);
        ASSERT_NE(window, nullptr);
        window->layer_context = 0xC1;

        auto& root = tree.touch(0x9000);
        root.bounds = {0, 0, 4, 4};
        root.position = {0, 0};
        root.anchor_point = {0, 0};
        root.background = {true, 1.0, 0.0, 0.0, 1.0};
        tree.set_context_root(0xC1, 0x9000);

        ASSERT_EQ(sogen::macos_layer_tree_present(*emu), 1u);

        const auto* presented = emu->ui.server.find_window(window->id);
        ASSERT_NE(presented, nullptr);
        ASSERT_NE(presented->backing_address, 0u);

        std::array<uint8_t, 4> pixel{};
        ASSERT_TRUE(emu->memory.try_read_memory(presented->backing_address, pixel.data(), pixel.size()));
        EXPECT_EQ(pixel[0], 0x00) << "blue";
        EXPECT_EQ(pixel[1], 0x00) << "green";
        EXPECT_EQ(pixel[2], 0xFF) << "red";
        EXPECT_EQ(pixel[3], 0xFF) << "alpha";
        EXPECT_EQ(emu->ui.present_count(), 1u);

        sogen::macos_layer_tree_release(*emu);
    }
}
