#include "../std_include.hpp"
#include "macos_layer_contents.hpp"

#include "../macos_emulator.hpp"
#include "../macos_platform.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>
#include <vector>

namespace sogen
{
    namespace
    {
        constexpr uint32_t MAX_RASTER_DIMENSION = 8192;

        // kCGBitmapByteOrder32Little | kCGImageAlphaPremultipliedFirst, which is what macos_layer_raster
        // documents: a pixel laid out B, G, R, A. Measured on 25G76 by filling a 1x1 context and reading
        // the bytes back -- opaque red is `00 00 ff ff` and opaque blue `ff 00 00 ff`. Grey cannot tell
        // the candidates apart, which is how the ...PremultipliedLast pairing that lays out A, B, G, R
        // went unnoticed: it answers `ff d9 d9 d9` for an opaque grey, and so does big-endian-first.
        //
        // CGContextDrawImage into such a context preserves row order -- measured with an image whose top
        // two memory rows are opaque, which land in the destination's top two rows -- so a raster built
        // this way honours macos_layer_raster's rows-top-down contract without any flip.
        constexpr uint32_t CG_BITMAP_INFO_BGRA8_PREMULTIPLIED = 0x2002;

        constexpr std::string_view LIBOBJC_IMAGE_PATH = "/usr/lib/libobjc.A.dylib";

        // The nullary selectors the chain sends. None has a colon, so the arity is two by construction
        // and only the return type is in question. Measured on 25G76 over every loaded class: -CGImage
        // exists on 12 of them and all 12 return a pointer; -image is what CATintedImage exposes its own
        // CGImage through (encoding ^{CGImage=}16@0:8) but is also worn by classes returning an ObjC
        // object, which is why the encoding is read.
        //
        // -tint is the other half of a template image. The CGImage a tinted image hands back is an
        // alpha-only mask -- measured on a live CATintedImage: 15x32, 8 bits per pixel, alphaInfo 0, no
        // colour space at all -- so it carries coverage and nothing else, and the colour it is meant to
        // be painted in lives behind -tint (encoding ^{CGColor=}16@0:8, ivar _tint at +0x10).
        constexpr std::array<std::string_view, 3> SELECTOR_NAMES{"CGImage", "image", "tint"};
        constexpr size_t ACCESSOR_COUNT = 2;
        constexpr size_t TINT_SELECTOR = 2;

        struct required_symbol
        {
            std::string_view image;
            std::string_view name;
            uint64_t macos_layer_contents_resolver::symbols::* field;
        };
    }

    bool macos_layer_contents_resolver::objc_symbols::complete() const
    {
        return this->sel_register_name != 0 && this->object_get_class != 0 && this->class_get_instance_method != 0 &&
               this->method_get_type_encoding != 0 && this->class_get_name != 0 && this->msg_send != 0;
    }

    bool macos_layer_contents_resolver::symbols::complete() const
    {
        return this->backing_store_copy_image != 0 && this->backing_store_get_type_id != 0 && this->image_get_width != 0 &&
               this->image_get_height != 0 && this->image_get_type_id != 0 && this->image_release != 0 &&
               this->colorspace_create_device_rgb != 0 && this->bitmap_context_create != 0 && this->context_draw_image != 0 &&
               this->context_release != 0 && this->context_set_fill_color != 0 && this->cf_get_type_id != 0;
    }

    uint64_t macos_layer_raster_allocate(macos_emulator& emu, const size_t bytes)
    {
        return emu.ui.arena.acquire(emu, bytes);
    }

    bool macos_layer_contents_resolver::bind(macos_emulator& emu, const macos_cache_symbols& cache_symbols)
    {
        constexpr std::string_view quartz_core = MACOS_QUARTZ_CORE_IMAGE_PATH;
        constexpr std::string_view core_graphics = MACOS_CORE_GRAPHICS_IMAGE_PATH;
        constexpr std::string_view core_foundation = MACOS_CORE_FOUNDATION_IMAGE_PATH;

        const std::array<required_symbol, 12> required{{
            {quartz_core, "_CABackingStoreCopyCGImage", &symbols::backing_store_copy_image},
            {quartz_core, "_CABackingStoreGetTypeID", &symbols::backing_store_get_type_id},
            {core_graphics, "_CGImageGetWidth", &symbols::image_get_width},
            {core_graphics, "_CGImageGetHeight", &symbols::image_get_height},
            {core_graphics, "_CGImageGetTypeID", &symbols::image_get_type_id},
            {core_graphics, "_CGImageRelease", &symbols::image_release},
            {core_graphics, "_CGColorSpaceCreateDeviceRGB", &symbols::colorspace_create_device_rgb},
            {core_graphics, "_CGBitmapContextCreate", &symbols::bitmap_context_create},
            {core_graphics, "_CGContextDrawImage", &symbols::context_draw_image},
            {core_graphics, "_CGContextSetRGBFillColor", &symbols::context_set_fill_color},
            {core_graphics, "_CGContextRelease", &symbols::context_release},
            {core_foundation, "_CFGetTypeID", &symbols::cf_get_type_id},
        }};

        symbols resolved{};
        for (const auto& entry : required)
        {
            const auto address = cache_symbols.find_export(entry.image, entry.name);
            if (!address)
            {
                emu.log.warn("layer contents: this system does not export %.*s; layer contents stay unrasterised\n",
                             static_cast<int>(entry.name.size()), entry.name.data());
                return false;
            }

            resolved.*entry.field = *address;
        }

        this->symbols_ = resolved;

        objc_symbols objc{};
        const std::array<std::pair<std::string_view, uint64_t objc_symbols::*>, 6> objc_required{{
            {"_sel_registerName", &objc_symbols::sel_register_name},
            {"_object_getClass", &objc_symbols::object_get_class},
            {"_class_getInstanceMethod", &objc_symbols::class_get_instance_method},
            {"_method_getTypeEncoding", &objc_symbols::method_get_type_encoding},
            {"_class_getName", &objc_symbols::class_get_name},
            {"_objc_msgSend", &objc_symbols::msg_send},
        }};

        for (const auto& [name, field] : objc_required)
        {
            const auto address = cache_symbols.find_export(LIBOBJC_IMAGE_PATH, name);
            if (!address)
            {
                emu.log.warn("layer contents: libobjc does not export %.*s; a contents object that is not a CGImage or a "
                             "CABackingStore stays unrasterised\n",
                             static_cast<int>(name.size()), name.data());
                objc = {};
                break;
            }

            objc.*field = *address;
        }

        this->objc_ = objc;
        return true;
    }

    void macos_layer_contents_resolver::bind_objc(const objc_symbols& resolved)
    {
        this->objc_ = resolved;
    }

    void macos_layer_contents_resolver::bind(const symbols& resolved)
    {
        this->symbols_ = resolved;
    }

    void macos_layer_contents_resolver::reset()
    {
        this->tree_ = nullptr;
        this->colorspace_ = 0;
        this->image_type_id_ = 0;
        this->backing_store_type_id_ = 0;
        this->type_ids_known_ = false;
        this->accessor_names_ = 0;
        this->accessors_.clear();
        this->accessors_registered_ = 0;
        this->accessors_known_ = false;
        this->in_flight_.reset();
        this->release_ = {};
        this->queue_.clear();
        this->cache_.clear();
        this->refused_.clear();
        this->attached_ = false;
        this->resolved_ = 0;
        this->failed_ = 0;
    }

    bool macos_layer_contents_resolver::call(macos_emulator& emu, const uint64_t function, const std::array<uint64_t, 8> args,
                                             void (macos_layer_contents_resolver::*next)(macos_emulator&, uint64_t))
    {
        auto* self = this;
        return emu.ui.calls.begin(
            emu, macos_guest_call_request{
                     .function = function,
                     .args = args,
                     .on_return = [self, next](macos_emulator& inner, const uint64_t result) { (self->*next)(inner, result); },
                 });
    }

    void macos_layer_contents_resolver::attach(macos_layer_tree& tree, const uint64_t object, const macos_layer_raster& raster)
    {
        std::vector<uint64_t> layers{};
        for (const auto& node : tree.nodes())
        {
            if (node.second.contents.object == object && node.second.contents.kind != macos_layer_contents_kind::raster)
            {
                layers.push_back(node.first);
            }
        }

        for (const auto layer : layers)
        {
            tree.attach_contents_raster(layer, raster);
        }

        this->attached_ = this->attached_ || !layers.empty();
    }

    void macos_layer_contents_resolver::forget(macos_emulator& emu, macos_layer_tree& tree, const uint64_t object)
    {
        if (object == 0)
        {
            return;
        }

        for (const auto& node : tree.nodes())
        {
            if (node.second.contents.object == object)
            {
                tree.discard_contents_raster(node.first);
            }
        }

        const auto cached = this->cache_.find(object);
        if (cached == this->cache_.end())
        {
            return;
        }

        emu.ui.arena.recycle(cached->second.pixels);
        this->cache_.erase(cached);
    }

    void macos_layer_contents_resolver::settle(macos_emulator& emu)
    {
        this->tree_ = nullptr;

        if (!std::exchange(this->attached_, false))
        {
            return;
        }

        macos_layer_tree_present(emu);
    }

    bool macos_layer_contents_resolver::resolve_one(macos_emulator& emu, macos_layer_tree& tree)
    {
        if (!this->bound())
        {
            return false;
        }

        this->tree_ = &tree;

        // A chain still in flight when a commit begins never came back: the guest call it made did not
        // return through the trap, which is what a contents object gone stale under the guest's own heap
        // corruption looks like from here. One such object must not wedge every later frame, and the
        // frames it left behind are why this is checked before the guest-call stack looks busy.
        if (this->in_flight_)
        {
            this->abandon(emu, "the guest never returned from the call that would have rasterised it");
            return emu.ui.calls.active();
        }

        if (emu.ui.calls.active())
        {
            return false;
        }

        if (!this->queue_.empty())
        {
            this->start_next(emu);
            return emu.ui.calls.active();
        }

        std::vector<uint64_t> already_rasterised{};

        for (const auto& node : tree.nodes())
        {
            const auto object = node.second.contents.object;
            if (object == 0 || node.second.contents.kind == macos_layer_contents_kind::raster)
            {
                continue;
            }

            if (this->cache_.contains(object))
            {
                if (std::ranges::find(already_rasterised, object) == already_rasterised.end())
                {
                    already_rasterised.push_back(object);
                }

                continue;
            }

            if (this->refused_.contains(object) || std::ranges::find(this->queue_, object) != this->queue_.end())
            {
                continue;
            }

            this->queue_.push_back(object);
        }

        for (const auto object : already_rasterised)
        {
            this->attach(tree, object, this->cache_.at(object));
        }

        if (this->queue_.empty())
        {
            this->settle(emu);
            return false;
        }

        if (this->type_ids_known_)
        {
            this->start_next(emu);
        }
        else
        {
            this->start_type_ids(emu);
        }

        return emu.ui.calls.active();
    }

    void macos_layer_contents_resolver::start_type_ids(macos_emulator& emu)
    {
        if (!this->call(emu, this->symbols_.image_get_type_id, {}, &macos_layer_contents_resolver::on_image_type_id))
        {
            this->queue_.clear();
            this->settle(emu);
        }
    }

    void macos_layer_contents_resolver::on_image_type_id(macos_emulator& emu, const uint64_t type_id)
    {
        this->image_type_id_ = type_id;

        if (!this->call(emu, this->symbols_.backing_store_get_type_id, {}, &macos_layer_contents_resolver::on_backing_store_type_id))
        {
            this->queue_.clear();
            this->settle(emu);
        }
    }

    void macos_layer_contents_resolver::on_backing_store_type_id(macos_emulator& emu, const uint64_t type_id)
    {
        this->backing_store_type_id_ = type_id;
        this->type_ids_known_ = true;
        this->start_selectors(emu);
    }

    void macos_layer_contents_resolver::start_selectors(macos_emulator& emu)
    {
        const auto give_up = [&] {
            this->accessors_known_ = true;
            this->start_next(emu);
        };

        if (this->accessors_known_ || !this->objc_.complete())
        {
            give_up();
            return;
        }

        if (this->accessor_names_ == 0)
        {
            const auto page = macos_layer_raster_allocate(emu, MACOS_PAGE_SIZE);
            size_t offset = 0;
            for (const auto name : SELECTOR_NAMES)
            {
                if (page == 0 || !emu.memory.try_write_memory(page + offset, name.data(), name.size() + 1))
                {
                    give_up();
                    return;
                }

                offset += name.size() + 1;
            }

            this->accessor_names_ = page;
            this->accessors_.assign(SELECTOR_NAMES.size(), 0);
        }

        if (this->accessors_registered_ >= SELECTOR_NAMES.size())
        {
            give_up();
            return;
        }

        size_t offset = 0;
        for (size_t index = 0; index < this->accessors_registered_; ++index)
        {
            offset += SELECTOR_NAMES.at(index).size() + 1;
        }

        if (!this->call(emu, this->objc_.sel_register_name, {this->accessor_names_ + offset}, &macos_layer_contents_resolver::on_selector))
        {
            give_up();
        }
    }

    void macos_layer_contents_resolver::on_selector(macos_emulator& emu, const uint64_t selector)
    {
        if (this->accessors_registered_ < this->accessors_.size())
        {
            this->accessors_.at(this->accessors_registered_) = selector;
        }

        ++this->accessors_registered_;
        this->start_selectors(emu);
    }

    void macos_layer_contents_resolver::start_next(macos_emulator& emu)
    {
        while (!this->queue_.empty())
        {
            const auto object = this->queue_.front();
            this->queue_.erase(this->queue_.begin());

            if (object == 0 || this->cache_.contains(object) || this->refused_.contains(object))
            {
                continue;
            }

            if (this->start_chain(emu, object))
            {
                return;
            }

            this->refuse(emu, object, "the guest-call chain could not be started");
        }

        this->settle(emu);
    }

    bool macos_layer_contents_resolver::start_chain(macos_emulator& emu, const uint64_t object)
    {
        this->in_flight_ = pending{.object = object};

        if (this->call(emu, this->symbols_.cf_get_type_id, {object}, &macos_layer_contents_resolver::on_object_type_id))
        {
            return true;
        }

        this->in_flight_.reset();
        return false;
    }

    void macos_layer_contents_resolver::on_object_type_id(macos_emulator& emu, const uint64_t type_id)
    {
        if (!this->in_flight_)
        {
            return;
        }

        if (type_id == this->image_type_id_)
        {
            this->in_flight_->image = this->in_flight_->object;
            this->in_flight_->owns_image = false;
            this->measure_image(emu);
            return;
        }

        if (type_id != this->backing_store_type_id_)
        {
            // CFGetTypeID answers the base CFType id for an ObjC instance that is not a CoreFoundation
            // type at all, which is what CATintedImage and NSViewBackingLayerContents are (measured,
            // 25G76). Handing one to CABackingStoreCopyCGImage dereferences it as a backing store and
            // faults, so the only safe question left is whether its class exposes a CGImage.
            this->in_flight_->object_type_id = type_id;
            this->ask_objc_for_image(emu);
            return;
        }

        this->in_flight_->owns_image = true;
        if (!this->call(emu, this->symbols_.backing_store_copy_image, {this->in_flight_->object},
                        &macos_layer_contents_resolver::on_copied_image))
        {
            this->abandon(emu, "CABackingStoreCopyCGImage could not be called");
        }
    }

    void macos_layer_contents_resolver::on_copied_image(macos_emulator& emu, const uint64_t image)
    {
        if (!this->in_flight_)
        {
            return;
        }

        if (image == 0)
        {
            const auto object = this->in_flight_->object;
            this->in_flight_->owns_image = false;
            this->abandon(emu, "CABackingStoreCopyCGImage answered nothing -- " + this->describe_empty_backing_store(emu, object));
            return;
        }

        this->in_flight_->image = image;
        if (!this->call(emu, this->symbols_.cf_get_type_id, {image}, &macos_layer_contents_resolver::on_copied_image_type_id))
        {
            this->abandon(emu, "the copied image's CFTypeID could not be read");
        }
    }

    void macos_layer_contents_resolver::on_copied_image_type_id(macos_emulator& emu, const uint64_t type_id)
    {
        if (!this->in_flight_)
        {
            return;
        }

        if (type_id != this->image_type_id_)
        {
            this->abandon(emu, "CABackingStoreCopyCGImage answered a CFTypeID " + std::to_string(type_id) + " object, not a CGImage");
            return;
        }

        this->measure_image(emu);
    }

    void macos_layer_contents_resolver::ask_objc_for_image(macos_emulator& emu)
    {
        if (this->accessors_.empty())
        {
            this->refuse_objc(emu);
            return;
        }

        if (!this->call(emu, this->objc_.object_get_class, {this->in_flight_->object}, &macos_layer_contents_resolver::on_objc_class))
        {
            this->refuse_objc(emu);
        }
    }

    void macos_layer_contents_resolver::on_objc_class(macos_emulator& emu, const uint64_t objc_class)
    {
        if (!this->in_flight_)
        {
            return;
        }

        if (objc_class == 0)
        {
            this->refuse_objc(emu);
            return;
        }

        this->in_flight_->objc_class = objc_class;
        this->in_flight_->accessor = 0;
        this->try_next_accessor(emu);
    }

    void macos_layer_contents_resolver::try_next_accessor(macos_emulator& emu)
    {
        while (this->in_flight_->accessor < ACCESSOR_COUNT && this->accessors_.at(this->in_flight_->accessor) == 0)
        {
            ++this->in_flight_->accessor;
        }

        if (this->in_flight_->accessor >= ACCESSOR_COUNT)
        {
            this->refuse_objc(emu);
            return;
        }

        const auto selector = this->accessors_.at(this->in_flight_->accessor);
        ++this->in_flight_->accessor;

        if (!this->call(emu, this->objc_.class_get_instance_method, {this->in_flight_->objc_class, selector},
                        &macos_layer_contents_resolver::on_objc_method))
        {
            this->refuse_objc(emu);
        }
    }

    void macos_layer_contents_resolver::on_objc_method(macos_emulator& emu, const uint64_t method)
    {
        if (!this->in_flight_)
        {
            return;
        }

        if (method == 0)
        {
            this->try_next_accessor(emu);
            return;
        }

        if (!this->call(emu, this->objc_.method_get_type_encoding, {method}, &macos_layer_contents_resolver::on_objc_encoding))
        {
            this->refuse_objc(emu);
        }
    }

    void macos_layer_contents_resolver::on_objc_encoding(macos_emulator& emu, const uint64_t encoding)
    {
        if (!this->in_flight_)
        {
            return;
        }

        // Only the first character matters, and reading one byte cannot straddle a page. An accessor
        // that hands back an integer, a float or a struct would put something that is not an address in
        // x0, and CFGetTypeID dereferences whatever it is given.
        char returns = 0;
        if (encoding == 0 || !emu.memory.try_read_memory(encoding, &returns, sizeof(returns)) || (returns != '^' && returns != '@'))
        {
            this->try_next_accessor(emu);
            return;
        }

        const auto selector = this->accessors_.at(this->in_flight_->accessor - 1);
        if (!this->call(emu, this->objc_.msg_send, {this->in_flight_->object, selector}, &macos_layer_contents_resolver::on_objc_image))
        {
            this->refuse_objc(emu);
        }
    }

    void macos_layer_contents_resolver::on_objc_image(macos_emulator& emu, const uint64_t image)
    {
        if (!this->in_flight_)
        {
            return;
        }

        if (image == 0)
        {
            this->try_next_accessor(emu);
            return;
        }

        this->in_flight_->image = image;
        if (!this->call(emu, this->symbols_.cf_get_type_id, {image}, &macos_layer_contents_resolver::on_objc_image_type_id))
        {
            this->refuse_objc(emu);
        }
    }

    void macos_layer_contents_resolver::on_objc_image_type_id(macos_emulator& emu, const uint64_t type_id)
    {
        if (!this->in_flight_)
        {
            return;
        }

        if (type_id != this->image_type_id_)
        {
            this->in_flight_->image = 0;
            this->try_next_accessor(emu);
            return;
        }

        // A plain accessor lends its CGImage rather than handing ownership over, and the layer holds the
        // object that owns it for as long as the raster is worth having.
        this->in_flight_->owns_image = false;
        this->ask_objc_for_tint(emu);
    }

    // The image an accessor hands back may be a mask: measured on a live CATintedImage, 8 bits per
    // pixel with no colour space, so it carries coverage and no colour at all. CGContextDrawImage paints
    // such an image in the context's current fill colour -- black by default, which is why an untinted
    // template comes out dark -- so the colour behind -tint has to be fetched and set before the draw.
    // The class is what a reader needs in order to know whether a refusal is a gap worth closing or an
    // object that legitimately holds no pixels, and the guest's own runtime is the only thing that knows
    // it -- CFGetTypeID has already answered the base CFType id for everything on this path.
    void macos_layer_contents_resolver::on_objc_class_name(macos_emulator& emu, const uint64_t name)
    {
        if (!this->in_flight_)
        {
            return;
        }

        std::string described{};
        for (size_t index = 0; name != 0 && index < 128; ++index)
        {
            char letter = 0;
            if (!emu.memory.try_read_memory(name + index, &letter, sizeof(letter)) || letter == 0)
            {
                break;
            }

            described.push_back(letter);
        }

        this->abandon(emu,
                      described.empty() ? this->in_flight_->fallback_reason : this->in_flight_->fallback_reason + " (a " + described + ")");
    }

    void macos_layer_contents_resolver::refuse_objc(macos_emulator& emu)
    {
        this->in_flight_->image = 0;
        this->in_flight_->fallback_reason = "its CFTypeID " + std::to_string(this->in_flight_->object_type_id) +
                                            " is neither CGImage nor CABackingStore, and its class hands out no CGImage";

        if (this->in_flight_->objc_class != 0 && this->objc_.class_get_name != 0 &&
            this->call(emu, this->objc_.class_get_name, {this->in_flight_->objc_class}, &macos_layer_contents_resolver::on_objc_class_name))
        {
            return;
        }

        this->abandon(emu, this->in_flight_->fallback_reason);
    }

    void macos_layer_contents_resolver::ask_objc_for_tint(macos_emulator& emu)
    {
        if (this->accessors_.size() <= TINT_SELECTOR || this->accessors_.at(TINT_SELECTOR) == 0 || this->in_flight_->objc_class == 0 ||
            !this->call(emu, this->objc_.class_get_instance_method, {this->in_flight_->objc_class, this->accessors_.at(TINT_SELECTOR)},
                        &macos_layer_contents_resolver::on_tint_method))
        {
            this->measure_image(emu);
        }
    }

    void macos_layer_contents_resolver::on_tint_method(macos_emulator& emu, const uint64_t method)
    {
        if (!this->in_flight_)
        {
            return;
        }

        if (method == 0 ||
            !this->call(emu, this->objc_.method_get_type_encoding, {method}, &macos_layer_contents_resolver::on_tint_encoding))
        {
            this->measure_image(emu);
        }
    }

    void macos_layer_contents_resolver::on_tint_encoding(macos_emulator& emu, const uint64_t encoding)
    {
        if (!this->in_flight_)
        {
            return;
        }

        char returns = 0;
        if (encoding == 0 || !emu.memory.try_read_memory(encoding, &returns, sizeof(returns)) || returns != '^' ||
            !this->call(emu, this->objc_.msg_send, {this->in_flight_->object, this->accessors_.at(TINT_SELECTOR)},
                        &macos_layer_contents_resolver::on_tint))
        {
            this->measure_image(emu);
        }
    }

    void macos_layer_contents_resolver::on_tint(macos_emulator& emu, const uint64_t tint)
    {
        if (!this->in_flight_)
        {
            return;
        }

        // Read out of the CGColor here rather than handed back to CoreGraphics as an object. The colour
        // is only borrowed from the tinted image, the components sit at offsets the tree already reads,
        // and it keeps the draw on CGContextSetRGBFillColor -- a leaf setter that takes four doubles.
        this->in_flight_->tint = tint != 0 ? macos_layer_read_color(emu, tint) : macos_layer_color{};
        this->measure_image(emu);
    }

    // CABackingStoreCopyCGImage has no error channel, so the reason it answered nothing is only visible
    // in the store itself. Offsets measured on 25G76 out of the export's own disassembly: the size at
    // +0x80/+0x88, a flag halfword at +0x1f4 whose bit 8 is an unconditional "answer nothing", and at
    // +0x198 a slot holding the CA::Render::Shmem at +0x10 or the CA::CG::Drawable at +0x18 -- one of
    // those two is where the pixels are, and a store with neither has none.
    std::string macos_layer_contents_resolver::describe_empty_backing_store(macos_emulator& emu, const uint64_t store) const
    {
        std::array<uint8_t, 0x1f6> header{};
        if (!emu.memory.try_read_memory(store, header.data(), header.size()))
        {
            return "the backing store holds no readable pixels";
        }

        const auto field = [&](const size_t offset) {
            uint64_t value = 0;
            std::memcpy(&value, header.data() + offset, sizeof(value));
            return value;
        };

        uint16_t flags = 0;
        std::memcpy(&flags, header.data() + 0x1f4, sizeof(flags));

        if ((flags & 0x100u) != 0)
        {
            return "the store is marked as having nothing to hand out";
        }

        uint64_t shmem = 0;
        uint64_t drawable = 0;
        if (const auto slot = field(0x198); slot != 0)
        {
            std::array<uint8_t, 0x20> slot_bytes{};
            if (emu.memory.try_read_memory(slot, slot_bytes.data(), slot_bytes.size()))
            {
                std::memcpy(&shmem, slot_bytes.data() + 0x10, sizeof(shmem));
                std::memcpy(&drawable, slot_bytes.data() + 0x18, sizeof(drawable));
            }
        }

        if (shmem == 0 && drawable == 0)
        {
            return "the store is " + std::to_string(field(0x80)) + "x" + std::to_string(field(0x88)) +
                   " with neither a shared-memory buffer nor a drawable, so CoreAnimation never gave it any storage";
        }

        return "the store is " + std::to_string(field(0x80)) + "x" + std::to_string(field(0x88)) + " over a " +
               (shmem != 0 ? "shared-memory buffer" : "drawable") + " that yielded no image";
    }

    void macos_layer_contents_resolver::measure_image(macos_emulator& emu)
    {
        if (!this->call(emu, this->symbols_.image_get_width, {this->in_flight_->image}, &macos_layer_contents_resolver::on_width))
        {
            this->abandon(emu, "CGImageGetWidth could not be called");
        }
    }

    void macos_layer_contents_resolver::on_width(macos_emulator& emu, const uint64_t width)
    {
        if (!this->in_flight_)
        {
            return;
        }

        if (width == 0 || width > MAX_RASTER_DIMENSION)
        {
            this->abandon(emu, "CGImageGetWidth answered " + std::to_string(width) + ", outside what sogen rasterises");
            return;
        }

        this->in_flight_->width = static_cast<uint32_t>(width);
        if (!this->call(emu, this->symbols_.image_get_height, {this->in_flight_->image}, &macos_layer_contents_resolver::on_height))
        {
            this->abandon(emu, "CGImageGetHeight could not be called");
        }
    }

    void macos_layer_contents_resolver::on_height(macos_emulator& emu, const uint64_t height)
    {
        if (!this->in_flight_)
        {
            return;
        }

        if (height == 0 || height > MAX_RASTER_DIMENSION)
        {
            this->abandon(emu, "CGImageGetHeight answered " + std::to_string(height) + " for a " + std::to_string(this->in_flight_->width) +
                                   "-wide image, outside what sogen rasterises");
            return;
        }

        this->in_flight_->height = static_cast<uint32_t>(height);

        const auto stride = this->in_flight_->width * 4u;
        const auto bytes = static_cast<size_t>(stride) * this->in_flight_->height;

        this->in_flight_->pixels = macos_layer_raster_allocate(emu, bytes);
        if (this->in_flight_->pixels == 0)
        {
            this->abandon(emu, "no room in the GUI arena for the raster");
            return;
        }

        this->in_flight_->stride = stride;

        if (this->colorspace_ != 0)
        {
            this->on_colorspace(emu, this->colorspace_);
            return;
        }

        if (!this->call(emu, this->symbols_.colorspace_create_device_rgb, {}, &macos_layer_contents_resolver::on_colorspace))
        {
            this->abandon(emu, "CGColorSpaceCreateDeviceRGB could not be called");
        }
    }

    void macos_layer_contents_resolver::on_colorspace(macos_emulator& emu, const uint64_t colorspace)
    {
        if (!this->in_flight_)
        {
            return;
        }

        if (colorspace == 0)
        {
            this->abandon(emu, "CGColorSpaceCreateDeviceRGB returned nothing");
            return;
        }

        this->colorspace_ = colorspace;

        const std::array<uint64_t, 8> args{
            this->in_flight_->pixels,           this->in_flight_->width, this->in_flight_->height, 8, this->in_flight_->stride, colorspace,
            CG_BITMAP_INFO_BGRA8_PREMULTIPLIED,
        };

        if (!this->call(emu, this->symbols_.bitmap_context_create, args, &macos_layer_contents_resolver::on_context))
        {
            this->abandon(emu, "CGBitmapContextCreate could not be called");
        }
    }

    void macos_layer_contents_resolver::on_context(macos_emulator& emu, const uint64_t context)
    {
        if (!this->in_flight_)
        {
            return;
        }

        if (context == 0)
        {
            this->abandon(emu, "CGBitmapContextCreate returned nothing");
            return;
        }

        this->in_flight_->context = context;

        if (this->in_flight_->tint.present)
        {
            auto* self = this;
            const auto& tint = this->in_flight_->tint;
            const auto started = emu.ui.calls.begin(
                emu, macos_guest_call_request{
                         .function = this->symbols_.context_set_fill_color,
                         .args = {context},
                         .double_args = {tint.r, tint.g, tint.b, tint.a},
                         .on_return = [self](macos_emulator& inner, const uint64_t result) { self->on_fill_colour_set(inner, result); },
                     });

            if (started)
            {
                return;
            }
        }

        this->draw_into_context(emu);
    }

    void macos_layer_contents_resolver::on_fill_colour_set(macos_emulator& emu, uint64_t)
    {
        if (!this->in_flight_)
        {
            return;
        }

        this->draw_into_context(emu);
    }

    void macos_layer_contents_resolver::draw_into_context(macos_emulator& emu)
    {
        const auto context = this->in_flight_->context;

        // CGContextDrawImage takes its CGRect by value, so the four doubles travel in d0..d3 and the
        // integer bank carries only the context and the image.
        auto* self = this;
        const auto started = emu.ui.calls.begin(
            emu, macos_guest_call_request{
                     .function = this->symbols_.context_draw_image,
                     .args = {context, this->in_flight_->image},
                     .double_args = {0.0, 0.0, static_cast<double>(this->in_flight_->width), static_cast<double>(this->in_flight_->height)},
                     .on_return = [self](macos_emulator& inner, uint64_t) { self->finish(inner); },
                 });

        if (!started)
        {
            this->abandon(emu, "CGContextDrawImage could not be called");
        }
    }

    void macos_layer_contents_resolver::finish(macos_emulator& emu)
    {
        if (!this->in_flight_)
        {
            return;
        }

        const auto done = *this->in_flight_;
        this->in_flight_.reset();

        const macos_layer_raster raster{
            .pixels = done.pixels,
            .width = done.width,
            .height = done.height,
            .stride = done.stride,
        };

        this->cache_[done.object] = raster;
        ++this->resolved_;

        if (this->tree_ != nullptr)
        {
            this->attach(*this->tree_, done.object, raster);
        }

        if (done.tint.present)
        {
            emu.log.info("layer contents: rasterised object 0x%" PRIx64 " as %ux%u, tinted (%.3f %.3f %.3f %.3f)\n", done.object,
                         done.width, done.height, done.tint.r, done.tint.g, done.tint.b, done.tint.a);
        }
        else
        {
            emu.log.info("layer contents: rasterised object 0x%" PRIx64 " as %ux%u, untinted\n", done.object, done.width, done.height);
        }

        this->release_ = release_task{.context = done.context, .image = done.owns_image ? done.image : 0};

        if (!this->call(emu, this->symbols_.context_release, {done.context}, &macos_layer_contents_resolver::on_context_released))
        {
            this->release_ = {};
            this->start_next(emu);
        }
    }

    void macos_layer_contents_resolver::on_context_released(macos_emulator& emu, uint64_t)
    {
        const auto image = this->release_.image;
        this->release_ = {};

        if (image != 0 && this->call(emu, this->symbols_.image_release, {image}, &macos_layer_contents_resolver::on_image_released))
        {
            return;
        }

        this->start_next(emu);
    }

    void macos_layer_contents_resolver::on_image_released(macos_emulator& emu, uint64_t)
    {
        this->start_next(emu);
    }

    void macos_layer_contents_resolver::abandon(macos_emulator& emu, const std::string& reason)
    {
        if (!this->in_flight_)
        {
            return;
        }

        const auto abandoned = *this->in_flight_;

        // The abandoned CGBitmapContext was built over exactly these pixels and this path never releases
        // it, so the guest can still draw through them. Retiring keeps the block mapped and out of every
        // later hand-out; recycling would put a guest-drawn background under someone else's raster.
        if (abandoned.pixels != 0)
        {
            emu.ui.arena.retire(abandoned.pixels);
        }

        this->in_flight_.reset();
        ++this->failed_;
        this->refuse(emu, abandoned.object, reason);

        // A CGImage this chain copied out of a backing store is the chain's to release even when the
        // rest of it failed, and the release has to be the thing that resumes the queue: starting the
        // next object first would point pc at it and lose the release.
        if (abandoned.owns_image && abandoned.image != 0 &&
            this->call(emu, this->symbols_.image_release, {abandoned.image}, &macos_layer_contents_resolver::on_image_released))
        {
            return;
        }

        // Frames still on the guest-call stack here belong to a call that never returned. Starting the
        // next object on top of them would point pc away from a chain that may yet unwind.
        if (!emu.ui.calls.active())
        {
            this->start_next(emu);
        }
    }

    void macos_layer_contents_resolver::refuse(macos_emulator& emu, const uint64_t object, const std::string& reason)
    {
        // Refused once and never retried: a contents object that cannot be rasterised on one commit
        // cannot be rasterised on the next either, and retrying would spend a guest-call chain per
        // frame forever.
        if (this->refused_.insert(object).second)
        {
            emu.log.warn("layer contents: object 0x%" PRIx64 " is not rasterised -- %s\n", object, reason.c_str());
        }
    }
}
