#pragma once

#include "../std_include.hpp"
#include "macos_objc_intercept.hpp"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace sogen
{
    class macos_emulator;
    class macos_layer_tree;

    // Declared, not included: macos_sdf_field.hpp reaches the compositor, which reaches this header.
    // Every one of these fixes its underlying type, so an opaque declaration is enough to store them.
    enum class macos_sdf_mode : uint8_t;
    enum class macos_sdf_operation : uint8_t;
    enum class macos_sdf_effect_kind : uint8_t;

    // Between the guest-call trap page (0x2F0000000) and the window backing-store arena (0x300000000).
    // The trampolines are guest code, so they cannot live in either.
    constexpr uint64_t MACOS_LAYER_TRAMPOLINE_BASE = 0x2F8000000ULL;
    constexpr size_t MACOS_LAYER_TRAMPOLINE_SLOT = 32;

    struct macos_layer_point
    {
        double x{};
        double y{};
    };

    struct macos_layer_rect
    {
        double x{};
        double y{};
        double width{};
        double height{};

        // CALayer stores a standardized rect: setting bounds (0,0,-40,-20) reads back (-40,-20,40,20).
        macos_layer_rect standardized() const;
        bool empty() const;
    };

    // CGAffineTransform component order: x' = a*x + c*y + tx, y' = b*x + d*y + ty.
    struct macos_layer_affine
    {
        double a{1.0};
        double b{0.0};
        double c{0.0};
        double d{1.0};
        double tx{0.0};
        double ty{0.0};

        bool is_identity() const;
        macos_layer_point apply(macos_layer_point point) const;

        // The transform that applies this one first and then `outer`.
        macos_layer_affine then(const macos_layer_affine& outer) const;
        std::optional<macos_layer_affine> inverse() const;

        static macos_layer_affine translation(double tx, double ty);
        static macos_layer_affine scaling(double sx, double sy);
    };

    struct macos_layer_color
    {
        bool present{};
        double r{};
        double g{};
        double b{};
        double a{};
    };

    enum class macos_layer_gravity : uint8_t
    {
        center,
        top,
        bottom,
        left,
        right,
        top_left,
        top_right,
        bottom_left,
        bottom_right,
        resize,
        resize_aspect,
        resize_aspect_fill,
    };

    std::optional<macos_layer_gravity> macos_layer_gravity_from_name(std::string_view name);

    enum class macos_layer_contents_kind : uint8_t
    {
        none,
        raster,
        unresolved,
    };

    // BGRA8 premultiplied, rows top-down, in guest memory sogen owns.
    struct macos_layer_raster
    {
        uint64_t pixels{};
        uint32_t width{};
        uint32_t height{};
        uint32_t stride{};

        bool valid() const;
    };

    struct macos_layer_contents
    {
        macos_layer_contents_kind kind{macos_layer_contents_kind::none};
        uint64_t object{};
        macos_layer_raster raster{};
    };

    // CGPath keeps its three closed forms as a unit shape mapped through an affine, measured on 25G76:
    // the kind tag is at +0x10 and the transform's six doubles follow, with a rounded rect's corner
    // radii stored after them as fractions of the shape's width and height.
    enum class macos_layer_shape_kind : uint8_t
    {
        none,
        rect,
        rounded_rect,
        ellipse,
        path,
        unmodelled,
    };

    // One flattened segment of a general path, in the path's own coordinate space. Curves are
    // subdivided where the path is decoded, because the compositor cannot reach guest memory and a
    // segment list is all a scanline fill needs.
    struct macos_layer_path_edge
    {
        macos_layer_point from{};
        macos_layer_point to{};
    };

    // A path longer than this is refused rather than flattened. Calculator's glyph outlines run to a
    // few hundred segments; the cap is what keeps a corrupt count out of the compositor's inner loop.
    constexpr size_t MACOS_LAYER_MAX_PATH_EDGES = 4096;

    struct macos_layer_shape
    {
        macos_layer_shape_kind kind{};
        macos_layer_affine transform{};
        double corner_x{};
        double corner_y{};
        macos_layer_color fill{};
        macos_layer_color stroke{};
        double line_width{1.0};
        bool even_odd{};

        // Only for macos_layer_shape_kind::path. Every subpath is closed, which is what CoreGraphics
        // fills whether the guest closed it or not.
        std::vector<macos_layer_path_edge> edges{};
    };

    // Which CoreAnimation class a layer is, resolved once from its isa. The compositor cannot read
    // guest memory, so the role has to be decided while the tree still can.
    enum class macos_layer_role : uint8_t
    {
        plain,
        sdf_group,
        sdf_container,
        sdf_element,
        backdrop,
    };

    // The CASDFLayer / CASDFElementLayer attributes macos_sdf_field needs, kept in the layer they were
    // set on. The rasteriser itself lives in macos_sdf_field.hpp and never sees the tree.
    struct macos_layer_sdf
    {
        uint64_t effect{};
        std::string effect_class{};
        macos_sdf_effect_kind effect_kind{};
        macos_layer_color effect_color{};

        double smoothness{};
        double gaussian_radius{};
        double effect_offset{};
        bool merge_elements{};

        macos_sdf_mode mode{};
        std::optional<macos_sdf_operation> operation{};
        double contents_zero_value_distance{};
        double contents_one_value_distance{1.0};
        double gradient_ovalization{};
    };

    struct macos_layer_node
    {
        uint64_t id{};
        uint64_t parent{};
        std::vector<uint64_t> children{};

        macos_layer_rect bounds{};
        macos_layer_point position{};
        macos_layer_point anchor_point{0.5, 0.5};
        macos_layer_affine transform{};
        macos_layer_affine sublayer_transform{};
        macos_layer_rect contents_rect{0.0, 0.0, 1.0, 1.0};

        double corner_radius{};
        double border_width{};
        double contents_scale{1.0};
        double z_position{};
        double opacity{1.0};

        bool hidden{};
        bool masks_to_bounds{};
        bool geometry_flipped{};

        macos_layer_color background{};
        macos_layer_color border{};
        uint64_t mask{};
        uint64_t compositing_filter{};
        double shadow_opacity{};
        bool allows_group_opacity{};

        macos_layer_role role{macos_layer_role::plain};
        bool role_resolved{};
        macos_layer_sdf sdf{};

        // CAPortalLayer. Measured on 25G76 by capturing a real window off the WindowServer: a portal
        // draws its source layer's subtree at 1:1, centred on the portal's own bounds and not clipped
        // to them, and hidesSourceLayer takes the source out of its own place.
        uint64_t portal_source{};
        bool hides_source_layer{};
        bool portal_matches_position{};
        macos_layer_shape shape{};
        macos_layer_gravity gravity{macos_layer_gravity::resize};
        macos_layer_contents contents{};

        // bounds.origin + anchor_point * bounds.size. Measured never to be mirrored by
        // geometry_flipped, which only mirrors the point being mapped.
        macos_layer_point anchor_in_bounds() const;

        // The layer's own space to its superlayer's space.
        macos_layer_affine to_superlayer() const;

        // The layer's sublayerTransform, applied about the anchor point in the layer's own space.
        macos_layer_affine sublayer_mapping() const;
    };

    class macos_layer_tree
    {
      public:
        macos_layer_node& touch(uint64_t layer);
        const macos_layer_node* find(uint64_t layer) const;
        macos_layer_node* find(uint64_t layer);

        void add_sublayer(uint64_t parent, uint64_t child);
        void insert_sublayer(uint64_t parent, uint64_t child, size_t index);
        void insert_sublayer_relative(uint64_t parent, uint64_t child, uint64_t sibling, bool above);
        void replace_sublayers(uint64_t parent, const std::vector<uint64_t>& children);
        void remove_from_superlayer(uint64_t child);

        // A mask is not a sublayer. Measured on 25G76: assigning a layer that is currently a sublayer as
        // a mask takes it out of its superlayer's sublayers, and clearing the mask leaves it with no
        // superlayer at all. A layer sogen keeps in both roles would be drawn twice, once as chrome and
        // once as the silhouette that was meant to cut it out.
        void set_mask(uint64_t layer, uint64_t mask);

        // CALayer derives bounds.size and position from a frame; measured formula in
 //. Returns false when
        // the layer's transform has off-diagonal terms, where CoreAnimation documents the result as
        // undefined -- the caller reports that by name.
        bool set_frame(uint64_t layer, macos_layer_rect frame);

        void attach_contents_raster(uint64_t layer, const macos_layer_raster& raster);

        // A raster is a copy taken out of the guest's own contents object, and a CABackingStore is a
        // buffer CoreAnimation draws into again rather than an immutable image. Once the guest has
        // redrawn, the copy is last frame's picture, so the layer goes back to unresolved and the
        // resolver takes a fresh one.
        void discard_contents_raster(uint64_t layer);

        void set_context_root(uint64_t context, uint64_t layer);
        uint64_t root_for_context(uint64_t context) const;

        const std::map<uint64_t, uint64_t>& context_roots() const
        {
            return this->context_roots_;
        }

        const std::map<uint64_t, macos_layer_node>& nodes() const
        {
            return this->nodes_;
        }

        size_t size() const
        {
            return this->nodes_.size();
        }

        size_t flush_count() const
        {
            return this->flush_count_;
        }

        void note_flush()
        {
            ++this->flush_count_;
        }

        void clear();

      private:
        void detach(macos_layer_node& child);

        std::map<uint64_t, macos_layer_node> nodes_{};
        std::map<uint64_t, uint64_t> context_roots_{};
        size_t flush_count_{};
    };

    // The CALayer / CAContext / CATransaction methods the tree is built from. Registering them is the
    // emulator's job; macos_layer_tree_bind does it and then installs the pass-through trampolines that
    // let the real implementations still run.
    std::vector<macos_objc_method> macos_layer_tree_methods();

    struct macos_layer_tree_binding
    {
        std::string name{};
        uint64_t imp{};
        uint64_t trampoline{};
        bool observed{};
        std::string refusal{};
    };

    std::vector<macos_layer_tree_binding> macos_layer_tree_bind(macos_emulator& emu, const dyld_shared_cache_reader& cache,
                                                                const macos_cache_symbols& symbols, macos_native_dispatch& dispatch);

    // The same installation for any other ObjC method list: each implementation is resolved out of the
    // cache, its first instruction is put back and copied into a pass-through trampoline, and the trap
    // goes over it, so a handler can observe or substitute and still let the real code run. Callers must
    // run after macos_layer_tree_bind, which owns the trampoline page and clears it per emulator.
    std::vector<macos_layer_tree_binding> macos_objc_bind_with_pass_through(macos_emulator& emu, const dyld_shared_cache_reader& cache,
                                                                            const macos_cache_symbols& symbols,
                                                                            macos_native_dispatch& dispatch,
                                                                            const std::vector<macos_objc_method>& methods);

    // Turns one already-resolved method implementation into an observation point: the first instruction
    // is copied into a trampoline that jumps back to imp + 4, and the trap is written over it. The entry
    // must not be trapped yet, because the instruction it displaces is read out of guest memory.
    bool macos_layer_tree_install(macos_emulator& emu, macos_native_dispatch& dispatch, uint64_t imp, std::string name,
                                  macos_native_handler handler);

    // The same thing for an export macos_native_dispatch::bind has already replaced: the pristine
    // instruction comes back out of the cache file, the trampoline is built from it, and the trap goes
    // back on. A routine installed this way observes its arguments and then lets the real
    // implementation run, which is what an opaque object the guest's own code dereferences needs.
    bool macos_layer_tree_reinstall_export(macos_emulator& emu, const dyld_shared_cache_reader& cache, macos_native_dispatch& dispatch,
                                           uint64_t entry, std::string name, macos_native_handler handler);

    // Points pc at the pass-through trampoline for this call's entry. False -- silently -- when the
    // entry has none, which is the answer a handler needs in order to fall back to substituting the
    // routine outright.
    bool macos_layer_tree_continue_into_original(const macos_native_call& call);

    // macos_native_handler is a plain function pointer with no context argument, so the handlers need a
    // way from an emulator to its tree. The natural home is a member of macos_ui_state; until it lives
    // there this registry keeps one tree per emulator and macos_layer_tree_release drops it.
    macos_layer_tree& macos_layer_tree_of(macos_emulator& emu);
    void macos_layer_tree_release(macos_emulator& emu);

    // Reads a CGColor out of guest memory at the measured offsets (+0x38 component count, +0x48
    // components). Absent, with a named warn, for a colour whose component count is not one sogen
    // decodes or whose memory cannot be read.
    macos_layer_color macos_layer_read_color(macos_emulator& emu, uint64_t color);

    // Decodes a CGPath's closed forms out of guest memory. A path CoreGraphics keeps as an element
    // list answers `unmodelled`: the shape is real but sogen has no rasteriser for it.
    macos_layer_shape macos_layer_read_shape_path(macos_emulator& emu, uint64_t path);

    // The ObjC class name of a live guest object, from its isa. Absent for a tagged pointer, an
    // unreadable object, or a Swift class the runtime has not realized and whose descriptor is gone.
    std::optional<std::string> macos_layer_read_class_name(macos_emulator& emu, uint64_t object);

    // Composites every window whose layer context has a root layer into that window's backing store and
    // presents it. Returns the number of windows presented.
    size_t macos_layer_tree_present(macos_emulator& emu);

    // Starts one contents-rasterisation chain if a layer needs one. This must NOT be called from a
    // method interception that continues into the original implementation: both want to write pc, and
    // the trampoline would overwrite the call the chain just set up, leaving a frame on the guest-call
    // stack for a call that never happened. Only a handler that returns straight to its caller -- an
    // intercepted export -- may start one.
    bool macos_layer_tree_resolve_contents(macos_emulator& emu);
}
