#include "../std_include.hpp"
#include "macos_layer_compositor.hpp"

#include "macos_sdf_field.hpp"

#include <algorithm>
#include <set>
#include <cmath>

namespace sogen
{
    namespace
    {
        struct clip_entry
        {
            macos_layer_affine inverse{};
            macos_layer_rect rect{};
            double radius{};
            int32_t x0{};
            int32_t y0{};
            int32_t x1{};
            int32_t y1{};
        };

        struct device_box
        {
            int32_t x0{};
            int32_t y0{};
            int32_t x1{};
            int32_t y1{};

            bool empty() const
            {
                return this->x1 <= this->x0 || this->y1 <= this->y0;
            }
        };

        struct contents_plan
        {
            bool active{};
            macos_layer_rect destination{};
            double source_x{};
            double source_y{};
            double source_width{};
            double source_height{};
            bool upside_down{};
        };

        // One layer of a mask's subtree, flattened in draw order so the per-pixel coverage is a walk
        // over a list rather than a recursion into the tree.
        struct mask_shape
        {
            const macos_layer_node* node{};
            macos_layer_affine inverse{};
            macos_layer_rect bounds{};
            double radius{};
            contents_plan plan{};
            uint32_t opacity{};
        };

        struct mask_entry
        {
            size_t first{};
            size_t count{};
        };

        struct composite_state
        {
            const macos_layer_tree* tree{};
            const macos_layer_surface* surface{};
            const macos_layer_pixel_source* source{};
            macos_layer_composite_stats stats{};
            std::vector<clip_entry> clips{};
            std::vector<mask_shape> mask_shapes{};
            std::vector<mask_entry> masks{};
            std::vector<uint64_t> ancestors{};
            std::vector<float> sdf_coverage{};
            std::vector<macos_layer_path_edge> device_edges{};
            std::vector<uint32_t> shape_row{};
            std::set<uint64_t> sdf_rendered{};
            size_t backdrop_depth{};
            double root_y_direction{-1.0};
            std::vector<uint8_t> row{};
            uint64_t row_raster{};
            int64_t row_index{-1};
            bool row_valid{};
        };

        bool finite(const double value)
        {
            return std::isfinite(value);
        }

        bool finite_point(const macos_layer_point point)
        {
            return finite(point.x) && finite(point.y);
        }

        double clamp01(const double value)
        {
            if (!finite(value))
            {
                return 0.0;
            }

            return std::clamp(value, 0.0, 1.0);
        }

        bool inside_rect(const macos_layer_rect& rect, const macos_layer_point point)
        {
            return point.x >= rect.x && point.x < rect.x + rect.width && point.y >= rect.y && point.y < rect.y + rect.height;
        }

        double effective_radius(const macos_layer_rect& rect, const double radius)
        {
            if (!finite(radius) || radius <= 0.0)
            {
                return 0.0;
            }

            return std::min(radius, std::min(rect.width, rect.height) / 2.0);
        }

        bool inside_rounded(const macos_layer_rect& rect, const double radius, const macos_layer_point point)
        {
            if (!inside_rect(rect, point))
            {
                return false;
            }

            if (radius <= 0.0)
            {
                return true;
            }

            const auto qx = std::max({rect.x + radius - point.x, point.x - (rect.x + rect.width - radius), 0.0});
            const auto qy = std::max({rect.y + radius - point.y, point.y - (rect.y + rect.height - radius), 0.0});
            return qx * qx + qy * qy <= radius * radius;
        }

        uint32_t div255(const uint32_t value)
        {
            return (value + 127u) / 255u;
        }

        uint8_t saturate(const uint32_t value)
        {
            return static_cast<uint8_t>(std::min(value, 255u));
        }

        device_box box_of(const macos_layer_affine& to_device, const macos_layer_rect& rect, const macos_layer_surface& surface)
        {
            const macos_layer_point corners[4] = {
                {rect.x, rect.y},
                {rect.x + rect.width, rect.y},
                {rect.x, rect.y + rect.height},
                {rect.x + rect.width, rect.y + rect.height},
            };

            double min_x = 0.0;
            double min_y = 0.0;
            double max_x = 0.0;
            double max_y = 0.0;

            for (size_t i = 0; i < 4; ++i)
            {
                const auto mapped = to_device.apply(corners[i]);
                if (!finite_point(mapped))
                {
                    return {};
                }

                if (i == 0)
                {
                    min_x = max_x = mapped.x;
                    min_y = max_y = mapped.y;
                    continue;
                }

                min_x = std::min(min_x, mapped.x);
                max_x = std::max(max_x, mapped.x);
                min_y = std::min(min_y, mapped.y);
                max_y = std::max(max_y, mapped.y);
            }

            const auto clamp_axis = [](const double value, const int32_t limit) {
                if (value <= 0.0)
                {
                    return int32_t{0};
                }

                if (value >= static_cast<double>(limit))
                {
                    return limit;
                }

                return static_cast<int32_t>(value);
            };

            device_box box{};
            box.x0 = clamp_axis(std::floor(min_x), surface.width);
            box.y0 = clamp_axis(std::floor(min_y), surface.height);
            box.x1 = clamp_axis(std::ceil(max_x), surface.width);
            box.y1 = clamp_axis(std::ceil(max_y), surface.height);
            return box;
        }

        device_box intersect(const device_box& a, const device_box& b)
        {
            return {
                std::max(a.x0, b.x0),
                std::max(a.y0, b.y0),
                std::min(a.x1, b.x1),
                std::min(a.y1, b.y1),
            };
        }

        bool clips_allow(const std::vector<clip_entry>& clips, const macos_layer_point device_point)
        {
            for (const auto& clip : clips)
            {
                const auto local = clip.inverse.apply(device_point);
                if (!finite_point(local) || !inside_rounded(clip.rect, clip.radius, local))
                {
                    return false;
                }
            }

            return true;
        }

        struct premultiplied
        {
            uint8_t b{};
            uint8_t g{};
            uint8_t r{};
            uint8_t a{};
        };

        premultiplied premultiply(const macos_layer_color& color, const double opacity)
        {
            const auto alpha = clamp01(color.a) * clamp01(opacity);
            return {
                macos_layer_channel_to_byte(clamp01(color.b) * alpha),
                macos_layer_channel_to_byte(clamp01(color.g) * alpha),
                macos_layer_channel_to_byte(clamp01(color.r) * alpha),
                macos_layer_channel_to_byte(alpha),
            };
        }

        bool raster_is_sane(const macos_layer_raster& raster)
        {
            if (!raster.valid())
            {
                return false;
            }

            constexpr uint32_t max_dimension = 1u << 16;
            if (raster.width > max_dimension || raster.height > max_dimension)
            {
                return false;
            }

            if (raster.stride < raster.width * 4u)
            {
                return false;
            }

            const auto span = static_cast<uint64_t>(raster.stride) * raster.height;
            return span != 0 && raster.pixels <= UINT64_MAX - span;
        }

        const uint8_t* sample_row(composite_state& state, const macos_layer_raster& raster, const int64_t row)
        {
            if (state.row_valid && state.row_raster == raster.pixels && state.row_index == row)
            {
                return state.row.data();
            }

            const auto bytes = static_cast<size_t>(raster.width) * 4u;
            state.row.assign(bytes, 0);
            state.row_valid = false;
            state.row_raster = raster.pixels;
            state.row_index = row;

            const auto address = raster.pixels + static_cast<uint64_t>(row) * raster.stride;
            if (state.source->read == nullptr || !state.source->read(state.source->context, address, state.row.data(), bytes))
            {
                ++state.stats.contents_unreadable;
                return nullptr;
            }

            state.row_valid = true;
            return state.row.data();
        }

        // Where the contents raster lands inside the layer's bounds. Local space is y-up, and the
        // raster's first row is its visual top, so "top" gravities pin the raster to the maximum y --
        // measured by rendering a 2x2 image through -[CALayer renderInContext:] under every gravity.
        macos_layer_rect gravity_rect(const macos_layer_gravity gravity, const macos_layer_rect& bounds, const double natural_width,
                                      const double natural_height, const bool upside_down)
        {
            const auto place = [&](const double width, const double height, const double ux, const double uy) {
                return macos_layer_rect{
                    bounds.x + (bounds.width - width) * ux,
                    bounds.y + (bounds.height - height) * (upside_down ? 1.0 - uy : uy),
                    width,
                    height,
                };
            };

            switch (gravity)
            {
            case macos_layer_gravity::resize:
                return bounds;

            case macos_layer_gravity::resize_aspect:
            case macos_layer_gravity::resize_aspect_fill: {
                if (natural_width <= 0.0 || natural_height <= 0.0)
                {
                    return bounds;
                }

                const auto sx = bounds.width / natural_width;
                const auto sy = bounds.height / natural_height;
                const auto scale = gravity == macos_layer_gravity::resize_aspect ? std::min(sx, sy) : std::max(sx, sy);
                return place(natural_width * scale, natural_height * scale, 0.5, 0.5);
            }

            case macos_layer_gravity::center:
                return place(natural_width, natural_height, 0.5, 0.5);
            case macos_layer_gravity::top:
                return place(natural_width, natural_height, 0.5, 1.0);
            case macos_layer_gravity::bottom:
                return place(natural_width, natural_height, 0.5, 0.0);
            case macos_layer_gravity::left:
                return place(natural_width, natural_height, 0.0, 0.5);
            case macos_layer_gravity::right:
                return place(natural_width, natural_height, 1.0, 0.5);
            case macos_layer_gravity::top_left:
                return place(natural_width, natural_height, 0.0, 1.0);
            case macos_layer_gravity::top_right:
                return place(natural_width, natural_height, 1.0, 1.0);
            case macos_layer_gravity::bottom_left:
                return place(natural_width, natural_height, 0.0, 0.0);
            case macos_layer_gravity::bottom_right:
                return place(natural_width, natural_height, 1.0, 0.0);
            }

            return bounds;
        }

        contents_plan plan_contents(const macos_layer_node& node, const macos_layer_rect& bounds, const bool upside_down)
        {
            const auto& raster = node.contents.raster;
            if (node.contents.kind != macos_layer_contents_kind::raster || !raster_is_sane(raster))
            {
                return {};
            }

            const auto unit = node.contents_rect.standardized();
            const auto source_width = unit.width * raster.width;
            const auto source_height = unit.height * raster.height;
            if (source_width <= 0.0 || source_height <= 0.0)
            {
                return {};
            }

            const auto scale = node.contents_scale > 0.0 && finite(node.contents_scale) ? node.contents_scale : 1.0;
            const auto destination =
                gravity_rect(node.gravity, bounds, source_width / scale, source_height / scale, upside_down).standardized();
            if (destination.empty())
            {
                return {};
            }

            return {
                true, destination, unit.x * raster.width, unit.y * raster.height, source_width, source_height, upside_down,
            };
        }

        bool sample_contents(composite_state& state, const macos_layer_node& node, const contents_plan& plan, const macos_layer_point local,
                             premultiplied& out)
        {
            if (!inside_rect(plan.destination, local))
            {
                return false;
            }

            const auto& raster = node.contents.raster;
            const auto u = (local.x - plan.destination.x) / plan.destination.width;

            // The raster's first row is its visual top. Measured on 25G76: CoreAnimation keeps contents
            // upright on screen, so under a chain that mirrors y -- geometryFlipped, or a negative scale
            // -- row 0 moves to the minimum local y instead of the maximum.
            const auto from_top = plan.destination.y + plan.destination.height - local.y;
            const auto from_bottom = local.y - plan.destination.y;
            const auto v = (plan.upside_down ? from_bottom : from_top) / plan.destination.height;

            const auto sx = static_cast<int64_t>(std::floor(plan.source_x + u * plan.source_width));
            const auto sy = static_cast<int64_t>(std::floor(plan.source_y + v * plan.source_height));
            if (sx < 0 || sy < 0 || sx >= static_cast<int64_t>(raster.width) || sy >= static_cast<int64_t>(raster.height))
            {
                return false;
            }

            const auto* row = sample_row(state, raster, sy);
            if (row == nullptr)
            {
                return false;
            }

            const auto* pixel = row + static_cast<size_t>(sx) * 4u;
            out = {pixel[0], pixel[1], pixel[2], pixel[3]};
            return true;
        }

        premultiplied scale_by_opacity(const premultiplied& color, const uint32_t opacity)
        {
            if (opacity >= 255u)
            {
                return color;
            }

            return {
                static_cast<uint8_t>(div255(color.b * opacity)),
                static_cast<uint8_t>(div255(color.g * opacity)),
                static_cast<uint8_t>(div255(color.r * opacity)),
                static_cast<uint8_t>(div255(color.a * opacity)),
            };
        }

        // The alpha the mask subtree composites to at one device point. Measured on 25G76 through
        // -[CALayer renderInContext:]: only the mask's alpha is read -- an opaque black mask masks in
        // exactly as an opaque white one does -- its sublayers contribute their own alpha, its layer
        // opacity multiplies, and the shapes accumulate source-over in draw order.
        uint32_t mask_coverage(composite_state& state, const macos_layer_point device_point)
        {
            uint32_t coverage = 255u;

            for (const auto& mask : state.masks)
            {
                uint32_t accumulated = 0;

                for (size_t index = mask.first; index < mask.first + mask.count; ++index)
                {
                    const auto& shape = state.mask_shapes[index];
                    const auto local = shape.inverse.apply(device_point);
                    if (!finite_point(local) || !inside_rounded(shape.bounds, shape.radius, local))
                    {
                        continue;
                    }

                    uint32_t alpha = macos_layer_channel_to_byte(clamp01(shape.node->background.a));
                    if (shape.plan.active)
                    {
                        premultiplied sampled{};
                        if (sample_contents(state, *shape.node, shape.plan, local, sampled))
                        {
                            alpha = alpha + div255(sampled.a * (255u - alpha));
                        }
                    }

                    alpha = div255(alpha * shape.opacity);
                    accumulated = alpha + div255(accumulated * (255u - alpha));
                }

                coverage = div255(coverage * accumulated);
                if (coverage == 0)
                {
                    return 0;
                }
            }

            return coverage;
        }

        // CGPath's closed forms are a unit shape mapped by an affine, so containment is tested in unit
        // space: the rect is [0,1]^2, the ellipse is the circle inscribed in it, and a rounded rect's
        // corner radii arrive already divided by the shape's width and height.
        bool inside_unit_shape(const macos_layer_shape& shape, const macos_layer_point unit)
        {
            if (shape.kind == macos_layer_shape_kind::ellipse)
            {
                const auto dx = unit.x - 0.5;
                const auto dy = unit.y - 0.5;
                return dx * dx + dy * dy <= 0.25;
            }

            if (unit.x < 0.0 || unit.x > 1.0 || unit.y < 0.0 || unit.y > 1.0)
            {
                return false;
            }

            if (shape.kind != macos_layer_shape_kind::rounded_rect)
            {
                return true;
            }

            const auto rx = std::clamp(shape.corner_x, 0.0, 0.5);
            const auto ry = std::clamp(shape.corner_y, 0.0, 0.5);
            if (rx <= 0.0 || ry <= 0.0)
            {
                return true;
            }

            const auto qx = std::max({rx - unit.x, unit.x - (1.0 - rx), 0.0});
            const auto qy = std::max({ry - unit.y, unit.y - (1.0 - ry), 0.0});
            const auto nx = qx / rx;
            const auto ny = qy / ry;
            return nx * nx + ny * ny <= 1.0;
        }

        // CoreAnimation antialiases a shape's edge -- measured on 25G76, an ellipse filling a 20x20
        // CAShapeLayer produced edge alphas of 0x05, 0x55, 0xa5, 0xe0 rather than a hard cut -- so the
        // fill is weighted by the fraction of a supersampled pixel the path covers.
        uint32_t shape_coverage(const macos_layer_shape& shape, const macos_layer_affine& to_unit, const macos_layer_point device)
        {
            constexpr int samples = 4;
            uint32_t inside = 0;

            for (int sy = 0; sy < samples; ++sy)
            {
                for (int sx = 0; sx < samples; ++sx)
                {
                    const macos_layer_point at{
                        device.x + (static_cast<double>(sx) + 0.5) / samples - 0.5,
                        device.y + (static_cast<double>(sy) + 0.5) / samples - 0.5,
                    };

                    const auto unit = to_unit.apply(at);
                    if (finite_point(unit) && inside_unit_shape(shape, unit))
                    {
                        ++inside;
                    }
                }
            }

            return inside * 255u / (samples * samples);
        }

        // A general path is filled by the same 4x4 supersampling the closed forms use, but the samples
        // are taken a sub-scanline at a time: every edge is crossed once per sub-scanline, the crossings
        // are sorted, and the winding is swept along the row. Testing each of the sixteen samples
        // against every edge instead would cost the edge count on every one of them.
        struct scanline_crossing
        {
            double x{};
            int32_t direction{};
        };

        void collect_crossings(const std::vector<macos_layer_path_edge>& edges, const double y, std::vector<scanline_crossing>& out)
        {
            out.clear();

            for (const auto& edge : edges)
            {
                const auto y0 = edge.from.y;
                const auto y1 = edge.to.y;
                if (y0 == y1 || !finite(y0) || !finite(y1))
                {
                    continue;
                }

                // Half-open in y, so a vertex shared by two edges is crossed exactly once.
                const auto top = std::min(y0, y1);
                const auto bottom = std::max(y0, y1);
                if (y < top || y >= bottom)
                {
                    continue;
                }

                const auto t = (y - y0) / (y1 - y0);
                const auto x = edge.from.x + t * (edge.to.x - edge.from.x);
                if (!finite(x))
                {
                    continue;
                }

                out.push_back({x, y1 > y0 ? 1 : -1});
            }

            std::ranges::sort(out, [](const scanline_crossing& left, const scanline_crossing& right) { return left.x < right.x; });
        }

        bool shape_is_drawable(const macos_layer_shape& shape)
        {
            if (shape.kind == macos_layer_shape_kind::none || shape.kind == macos_layer_shape_kind::unmodelled || !shape.fill.present ||
                shape.fill.a <= 0.0)
            {
                return false;
            }

            return shape.kind != macos_layer_shape_kind::path || !shape.edges.empty();
        }

        void paint_path_shape(composite_state& state, const macos_layer_shape& shape, const macos_layer_affine& to_device,
                              const double opacity)
        {
            const auto path_to_device = shape.transform.then(to_device);

            state.device_edges.clear();
            state.device_edges.reserve(shape.edges.size());

            auto min_x = std::numeric_limits<double>::max();
            auto min_y = min_x;
            auto max_x = std::numeric_limits<double>::lowest();
            auto max_y = max_x;

            for (const auto& edge : shape.edges)
            {
                const auto from = path_to_device.apply(edge.from);
                const auto to = path_to_device.apply(edge.to);
                if (!finite_point(from) || !finite_point(to))
                {
                    return;
                }

                state.device_edges.push_back({from, to});
                min_x = std::min({min_x, from.x, to.x});
                min_y = std::min({min_y, from.y, to.y});
                max_x = std::max({max_x, from.x, to.x});
                max_y = std::max({max_y, from.y, to.y});
            }

            // Clamped as doubles before the cast: a finite point may still be far outside any int32.
            const auto clamp_to = [](const double value, const int32_t limit) {
                return static_cast<int32_t>(std::clamp(value, 0.0, static_cast<double>(limit)));
            };

            device_box box{
                clamp_to(std::floor(min_x), state.surface->width),
                clamp_to(std::floor(min_y), state.surface->height),
                clamp_to(std::ceil(max_x) + 1.0, state.surface->width),
                clamp_to(std::ceil(max_y) + 1.0, state.surface->height),
            };

            for (const auto& clip : state.clips)
            {
                box = intersect(box, device_box{clip.x0, clip.y0, clip.x1, clip.y1});
            }

            if (box.empty())
            {
                return;
            }

            constexpr int32_t samples = 4;
            const auto fill = premultiply(shape.fill, opacity);
            const auto width = static_cast<size_t>(box.x1 - box.x0);

            std::vector<scanline_crossing> crossings{};
            bool wrote = false;

            for (int32_t y = box.y0; y < box.y1; ++y)
            {
                state.shape_row.assign(width, 0u);

                for (int32_t sub = 0; sub < samples; ++sub)
                {
                    const auto sample_y = static_cast<double>(y) + (static_cast<double>(sub) + 0.5) / samples;
                    collect_crossings(state.device_edges, sample_y, crossings);
                    if (crossings.empty())
                    {
                        continue;
                    }

                    size_t next = 0;
                    int32_t winding = 0;

                    for (int32_t x = box.x0; x < box.x1; ++x)
                    {
                        for (int32_t sx = 0; sx < samples; ++sx)
                        {
                            const auto sample_x = static_cast<double>(x) + (static_cast<double>(sx) + 0.5) / samples;
                            while (next < crossings.size() && crossings[next].x <= sample_x)
                            {
                                winding += crossings[next].direction;
                                ++next;
                            }

                            if (shape.even_odd ? (next % 2u) != 0 : winding != 0)
                            {
                                ++state.shape_row[static_cast<size_t>(x - box.x0)];
                            }
                        }
                    }
                }

                for (int32_t x = box.x0; x < box.x1; ++x)
                {
                    const auto hits = state.shape_row[static_cast<size_t>(x - box.x0)];
                    if (hits == 0)
                    {
                        continue;
                    }

                    const macos_layer_point device{static_cast<double>(x) + 0.5, static_cast<double>(y) + 0.5};
                    if (!clips_allow(state.clips, device))
                    {
                        continue;
                    }

                    auto coverage = hits * 255u / static_cast<uint32_t>(samples * samples);
                    if (!state.masks.empty())
                    {
                        coverage = div255(coverage * mask_coverage(state, device));
                    }

                    const auto painted = scale_by_opacity(fill, coverage);
                    if (painted.a == 0)
                    {
                        continue;
                    }

                    auto* pixel = state.surface->pixels + static_cast<size_t>(y) * static_cast<size_t>(state.surface->stride) +
                                  static_cast<size_t>(x) * 4u;
                    macos_layer_blend_pixel(pixel, painted.b, painted.g, painted.r, painted.a);
                    ++state.stats.pixels_written;
                    wrote = true;
                }
            }

            if (wrote)
            {
                ++state.stats.shapes_filled;
            }
        }

        void paint_shape(composite_state& state, const macos_layer_node& node, const macos_layer_affine& to_device, const double opacity)
        {
            const auto& shape = node.shape;
            if (!shape_is_drawable(shape))
            {
                return;
            }

            if (shape.kind == macos_layer_shape_kind::path)
            {
                paint_path_shape(state, shape, to_device, opacity);
                return;
            }

            const auto shape_to_device = shape.transform.then(to_device);
            const auto to_unit = shape_to_device.inverse();
            if (!to_unit)
            {
                ++state.stats.singular_transforms;
                return;
            }

            auto box = box_of(shape_to_device, macos_layer_rect{0.0, 0.0, 1.0, 1.0}, *state.surface);
            for (const auto& clip : state.clips)
            {
                box = intersect(box, device_box{clip.x0, clip.y0, clip.x1, clip.y1});
            }

            if (box.empty())
            {
                return;
            }

            const auto fill = premultiply(shape.fill, opacity);
            bool wrote = false;

            for (int32_t y = box.y0; y < box.y1; ++y)
            {
                for (int32_t x = box.x0; x < box.x1; ++x)
                {
                    const macos_layer_point device{static_cast<double>(x) + 0.5, static_cast<double>(y) + 0.5};
                    if (!clips_allow(state.clips, device))
                    {
                        continue;
                    }

                    auto coverage = shape_coverage(shape, *to_unit, device);
                    if (coverage == 0)
                    {
                        continue;
                    }

                    if (!state.masks.empty())
                    {
                        coverage = div255(coverage * mask_coverage(state, device));
                        if (coverage == 0)
                        {
                            continue;
                        }
                    }

                    const auto painted = scale_by_opacity(fill, coverage);
                    if (painted.a == 0)
                    {
                        continue;
                    }

                    auto* pixel = state.surface->pixels + static_cast<size_t>(y) * static_cast<size_t>(state.surface->stride) +
                                  static_cast<size_t>(x) * 4u;
                    macos_layer_blend_pixel(pixel, painted.b, painted.g, painted.r, painted.a);
                    ++state.stats.pixels_written;
                    wrote = true;
                }
            }

            if (wrote)
            {
                ++state.stats.shapes_filled;
            }
        }

        void paint(composite_state& state, const macos_layer_node& node, const macos_layer_affine& to_device,
                   const macos_layer_rect& bounds, const double radius, const double opacity)
        {
            const auto inverse = to_device.inverse();
            if (!inverse)
            {
                ++state.stats.singular_transforms;
                return;
            }

            auto box = box_of(to_device, bounds, *state.surface);
            for (const auto& clip : state.clips)
            {
                box = intersect(box, device_box{clip.x0, clip.y0, clip.x1, clip.y1});
            }

            if (box.empty())
            {
                return;
            }

            const auto background = premultiply(node.background, opacity);
            const auto border = premultiply(node.border, opacity);
            const auto plan = plan_contents(node, bounds, to_device.d * state.root_y_direction < 0.0);
            const auto opacity_byte = static_cast<uint32_t>(macos_layer_channel_to_byte(clamp01(opacity)));

            const auto stroke = finite(node.border_width) ? std::max(node.border_width, 0.0) : 0.0;
            const macos_layer_rect inner{
                bounds.x + stroke,
                bounds.y + stroke,
                bounds.width - 2.0 * stroke,
                bounds.height - 2.0 * stroke,
            };
            const auto inner_radius = std::max(radius - stroke, 0.0);
            const bool has_border = border.a != 0 && stroke > 0.0;

            bool wrote = false;
            bool blitted = false;

            for (int32_t y = box.y0; y < box.y1; ++y)
            {
                for (int32_t x = box.x0; x < box.x1; ++x)
                {
                    const macos_layer_point device{static_cast<double>(x) + 0.5, static_cast<double>(y) + 0.5};
                    const auto local = inverse->apply(device);
                    if (!finite_point(local) || !inside_rect(bounds, local))
                    {
                        continue;
                    }

                    if (!clips_allow(state.clips, device))
                    {
                        continue;
                    }

                    const auto coverage = state.masks.empty() ? 255u : mask_coverage(state, device);
                    if (coverage == 0)
                    {
                        continue;
                    }

                    const bool rounded = radius <= 0.0 || inside_rounded(bounds, radius, local);
                    auto* pixel = state.surface->pixels + static_cast<size_t>(y) * static_cast<size_t>(state.surface->stride) +
                                  static_cast<size_t>(x) * 4u;

                    if (rounded && background.a != 0)
                    {
                        const auto masked = scale_by_opacity(background, coverage);
                        if (masked.a != 0)
                        {
                            macos_layer_blend_pixel(pixel, masked.b, masked.g, masked.r, masked.a);
                            wrote = true;
                        }
                    }

                    // CoreAnimation clips a layer's contents to its corner radius only when
                    // masksToBounds is set; the background and border always follow the radius.
                    if (plan.active && (rounded || !node.masks_to_bounds))
                    {
                        premultiplied sampled{};
                        if (sample_contents(state, node, plan, local, sampled))
                        {
                            const auto scaled = scale_by_opacity(scale_by_opacity(sampled, opacity_byte), coverage);
                            blitted = true;
                            if (scaled.a != 0)
                            {
                                macos_layer_blend_pixel(pixel, scaled.b, scaled.g, scaled.r, scaled.a);
                                wrote = true;
                            }
                        }
                    }

                    if (has_border && rounded && !inside_rounded(inner, inner_radius, local))
                    {
                        const auto masked = scale_by_opacity(border, coverage);
                        if (masked.a != 0)
                        {
                            macos_layer_blend_pixel(pixel, masked.b, masked.g, masked.r, masked.a);
                            wrote = true;
                        }
                    }

                    if (wrote)
                    {
                        ++state.stats.pixels_written;
                        wrote = false;
                    }
                }
            }

            if (blitted)
            {
                ++state.stats.contents_blits;
            }
        }

        // Flattens a mask's own subtree. Returns false when the mask cannot be modelled -- an unresolved
        // contents raster would read as no coverage at all, which erases the masked layer instead of
        // shaping it, so the mask is dropped and the layer composites unmasked as it did before.
        bool flatten_mask(composite_state& state, const uint64_t layer, const macos_layer_affine& parent_to_device, const double opacity,
                          const size_t depth)
        {
            if (depth >= MACOS_LAYER_MAX_DEPTH || state.mask_shapes.size() >= MACOS_LAYER_MAX_MASK_SHAPES)
            {
                return false;
            }

            const auto* node = state.tree->find(layer);
            if (node == nullptr)
            {
                return false;
            }

            if (node->contents.kind == macos_layer_contents_kind::unresolved)
            {
                return false;
            }

            if (node->hidden)
            {
                return true;
            }

            const auto own_opacity = opacity * clamp01(node->opacity);
            const auto to_device = node->to_superlayer().then(parent_to_device);
            const auto bounds = node->bounds.standardized();

            if (!bounds.empty())
            {
                const auto inverse = to_device.inverse();
                if (!inverse)
                {
                    return false;
                }

                state.mask_shapes.push_back(mask_shape{
                    node,
                    *inverse,
                    bounds,
                    effective_radius(bounds, node->corner_radius),
                    plan_contents(*node, bounds, to_device.d * state.root_y_direction < 0.0),
                    static_cast<uint32_t>(macos_layer_channel_to_byte(clamp01(own_opacity))),
                });
            }

            const auto child_to_device = node->sublayer_mapping().then(to_device);
            for (const auto child : node->children)
            {
                if (!flatten_mask(state, child, child_to_device, own_opacity, depth + 1))
                {
                    return false;
                }
            }

            return true;
        }

        bool push_mask(composite_state& state, const uint64_t mask, const macos_layer_affine& base)
        {
            if (state.masks.size() >= MACOS_LAYER_MAX_MASKS)
            {
                ++state.stats.masks_skipped;
                return false;
            }

            const auto first = state.mask_shapes.size();
            if (!flatten_mask(state, mask, base, 1.0, 0))
            {
                state.mask_shapes.resize(first);
                ++state.stats.masks_skipped;
                return false;
            }

            state.masks.push_back(mask_entry{first, state.mask_shapes.size() - first});
            ++state.stats.masks_applied;
            return true;
        }

        // Sampled from a capture of the real Calculator on this host, not from a synthetic probe. They
        // are used as opaque fills because the target is a flat measured colour, so no alpha has to be
        // fitted: painting the colour reproduces it exactly whatever it lands on.
        // macos_sdf_measured_glass_fill is deliberately not used -- it was calibrated on the SDF work's
        // own capsule over a gradient backdrop and composites to (33,33,33) here, which is neither the
        // body nor the keys.
        //
        // The capture these came from was of an **inactive** window, which is what the guest currently
        // renders as too: re-measured, the same window active reads (35,33,29) body, (72,71,68) number
        // keys and (115,114,111) function keys, with an orange operator column. Moving these to the
        // active palette is only correct once the guest's own layers stop carrying inactive colours --
        // its traffic lights are handed rgb(35,35,35) today. docs/macos-window-server.md.
        constexpr macos_layer_color MACOS_LAYER_MEASURED_CHROME_GLASS{true, 43.0 / 255.0, 39.0 / 255.0, 37.0 / 255.0, 1.0};

        // The function keys measure (60,59,57), an 8-level lift on this. Which keys are tinted is not
        // recoverable from the layer tree: the right-hand column is cleanly its own CASDFLayer, but the
        // top row's other three keys share a field with nine untinted ones, and the tint itself lives
        // only in the shape of SwiftUI's Swift-side SDFStyle tree. All capsules therefore take the
        // number-key colour, which is right for twelve of the twenty and 8 levels dark on the rest.
        constexpr macos_layer_color MACOS_LAYER_MEASURED_CAPSULE_GLASS{true, 52.0 / 255.0, 51.0 / 255.0, 49.0 / 255.0, 1.0};

        // The element's chain up to the container, exactly the mapping draw() accumulates but stopping at
        // the CASDFLayer instead of the surface. A hidden subtree is not handed over at all, and a
        // non-element descendant contributes only its transform.
        void collect_sdf_elements(const macos_layer_tree& tree, const uint64_t layer, const macos_layer_affine& parent_to_container,
                                  std::vector<macos_sdf_element>& out, const size_t depth)
        {
            if (depth >= MACOS_LAYER_MAX_DEPTH || out.size() >= MACOS_LAYER_MAX_NODES_PER_COMPOSITE)
            {
                return;
            }

            const auto* node = tree.find(layer);
            if (node == nullptr || node->hidden)
            {
                return;
            }

            const auto to_container = node->to_superlayer().then(parent_to_container);

            if (node->role == macos_layer_role::sdf_element && node->sdf.operation)
            {
                out.push_back(macos_sdf_element{
                    node->bounds.standardized(),
                    node->corner_radius,
                    to_container,
                    node->sdf.mode,
                    *node->sdf.operation,
                    node->sdf.contents_zero_value_distance,
                    node->sdf.contents_one_value_distance,
                    node->sdf.gradient_ovalization,
                });
            }

            const auto child_base = node->sublayer_mapping().then(to_container);
            for (const auto child : node->children)
            {
                collect_sdf_elements(tree, child, child_base, out, depth + 1);
            }
        }

 // Section 9 of. The coverage
        // is rasterised rather than composited by macos_sdf_render so the field goes through the same
        // clip and mask machinery every other layer does.
        void render_sdf_container(composite_state& state, const macos_layer_node& node, const macos_layer_affine& to_device,
                                  const double opacity)
        {
            macos_sdf_field field{};
            field.smoothness = node.sdf.smoothness;
            field.gaussian_radius = node.sdf.gaussian_radius;
            field.effect_offset = node.sdf.effect_offset;
            field.merge_elements = node.sdf.merge_elements;

            for (const auto child : node.children)
            {
                collect_sdf_elements(*state.tree, child, node.sublayer_mapping(), field.elements, 0);
            }

            if (field.elements.empty())
            {
                return;
            }

            macos_layer_color paint{};
            if (node.sdf.effect_kind == macos_sdf_effect_kind::fill)
            {
                paint = node.sdf.effect_color;
            }
            else if (node.sdf.effect_kind == macos_sdf_effect_kind::output && state.backdrop_depth > 0)
            {
                paint = MACOS_LAYER_MEASURED_CHROME_GLASS;
                ++state.stats.sdf_fields_approximated;
            }
            else if (node.sdf.effect_kind == macos_sdf_effect_kind::gradient)
            {
                paint = MACOS_LAYER_MEASURED_CAPSULE_GLASS;
                ++state.stats.sdf_fields_approximated;
            }

            if (!paint.present || paint.a <= 0.0)
            {
                ++state.stats.sdf_fields_unmodelled;
                return;
            }

            const auto width = state.surface->width;
            const auto height = state.surface->height;
            state.sdf_coverage.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0.0f);

            const macos_sdf_coverage_raster raster{state.sdf_coverage.data(), width, height, width};
            const auto sdf = macos_sdf_rasterize_coverage(field, to_device, raster);
            if (sdf.elements_used == 0)
            {
                ++state.stats.sdf_fields_unmodelled;
                return;
            }

            ++state.stats.sdf_fields_drawn;
            state.stats.sdf_elements_used += sdf.elements_used;
            state.stats.sdf_elements_refused += sdf.elements_refused;

            const auto fill = premultiply(paint, opacity);

            for (int32_t y = 0; y < height; ++y)
            {
                for (int32_t x = 0; x < width; ++x)
                {
                    const auto value = state.sdf_coverage[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)];
                    if (!(value > 0.0f))
                    {
                        continue;
                    }

                    const macos_layer_point device{static_cast<double>(x) + 0.5, static_cast<double>(y) + 0.5};
                    if (!clips_allow(state.clips, device))
                    {
                        continue;
                    }

                    auto coverage = static_cast<uint32_t>(macos_layer_channel_to_byte(clamp01(value)));
                    if (!state.masks.empty())
                    {
                        coverage = div255(coverage * mask_coverage(state, device));
                    }

                    const auto painted = scale_by_opacity(fill, coverage);
                    if (painted.a == 0)
                    {
                        continue;
                    }

                    auto* pixel = state.surface->pixels + static_cast<size_t>(y) * static_cast<size_t>(state.surface->stride) +
                                  static_cast<size_t>(x) * 4u;
                    macos_layer_blend_pixel(pixel, painted.b, painted.g, painted.r, painted.a);
                    ++state.stats.pixels_written;
                }
            }
        }

        // SwiftUI hangs the glyph cells and the glass bezel off one SDFLayer, with the bezel as a later
        // sibling, so drawing the field where the CASDFLayer sits puts it over the glyphs. In the real
        // compositor the portals project the content back on top; sogen does not model portals, so the
        // contract's "treat the whole SwiftUI.SDFLayer subtree as one fill" is taken literally and the
        // fill is drawn on the way into the group, before anything the group contains.
        void render_sdf_group(composite_state& state, const uint64_t layer, const macos_layer_affine& to_device, const double opacity,
                              const size_t depth, const bool under_backdrop)
        {
            if (depth >= MACOS_LAYER_MAX_DEPTH)
            {
                return;
            }

            const auto* node = state.tree->find(layer);
            if (node == nullptr || node->hidden)
            {
                return;
            }

            const auto backdrop = under_backdrop || node->role == macos_layer_role::backdrop;

            if (node->role == macos_layer_role::sdf_container && state.sdf_rendered.insert(layer).second)
            {
                const auto saved = state.backdrop_depth;
                state.backdrop_depth = backdrop ? 1 : 0;
                render_sdf_container(state, *node, to_device, opacity);
                state.backdrop_depth = saved;
                return;
            }

            const auto child_base = node->sublayer_mapping().then(to_device);
            for (const auto child : node->children)
            {
                const auto* entry = state.tree->find(child);
                if (entry == nullptr)
                {
                    continue;
                }

                render_sdf_group(state, child, entry->to_superlayer().then(child_base), opacity * clamp01(entry->opacity), depth + 1,
                                 backdrop);
            }
        }

        void draw(composite_state& state, uint64_t layer, const macos_layer_affine& parent_to_device, const double opacity,
                  const size_t depth)
        {
            if (depth >= MACOS_LAYER_MAX_DEPTH)
            {
                ++state.stats.depth_limited;
                return;
            }

            if (state.stats.layers_visited >= MACOS_LAYER_MAX_NODES_PER_COMPOSITE)
            {
                ++state.stats.layer_limited;
                return;
            }

            const auto* node = state.tree->find(layer);
            if (node == nullptr)
            {
                return;
            }

            if (std::ranges::find(state.ancestors, layer) != state.ancestors.end())
            {
                ++state.stats.cycles_cut;
                return;
            }

            ++state.stats.layers_visited;

            if (node->hidden)
            {
                return;
            }

            const auto own_opacity = opacity * clamp01(node->opacity);
            if (own_opacity <= 0.0)
            {
                return;
            }

            const auto to_device = node->to_superlayer().then(parent_to_device);
            const auto bounds = node->bounds.standardized();
            const auto radius = effective_radius(bounds, node->corner_radius);
            const auto child_to_device = node->sublayer_mapping().then(to_device);

            if (node->contents.kind == macos_layer_contents_kind::unresolved)
            {
                ++state.stats.contents_unresolved;
            }

            // A CAPortalLayer's projection is measured (spec section 11) and still not applied: A/B'd
            // twice on Calculator, once with matchesPosition folded in, honouring it cost seven of the
            // sixteen contents blits and truncated the keypad. Hiding a source without a projection
            // that lands where CoreAnimation puts it only deletes content, so the portal is counted.
            if (node->portal_source != 0)
            {
                ++state.stats.portals_unmodelled;
            }

            // Measured: a mask shapes the layer's own drawing as well as its subtree's, and the masked
            // layer's sublayerTransform moves the mask, so it is placed exactly where a sublayer is.
            const auto mask_shapes_before = state.mask_shapes.size();
            const auto masked = node->mask != 0 && push_mask(state, node->mask, child_to_device);

            if (!bounds.empty())
            {
                ++state.stats.layers_drawn;
                paint(state, *node, to_device, bounds, radius, own_opacity);
            }

            // A CAShapeLayer's path is drawn in the layer's own space and is not clipped to its bounds,
            // so this is not folded into paint(), which iterates the bounds box.
            paint_shape(state, *node, to_device, own_opacity);

            if (node->shape.kind == macos_layer_shape_kind::unmodelled)
            {
                ++state.stats.shapes_unmodelled;
            }

            const auto clips_before = state.clips.size();
            if (node->masks_to_bounds && !bounds.empty())
            {
                if (state.clips.size() >= MACOS_LAYER_MAX_CLIPS)
                {
                    ++state.stats.clip_limited;
                }
                else if (const auto inverse = to_device.inverse())
                {
                    const auto box = box_of(to_device, bounds, *state.surface);
                    state.clips.push_back(clip_entry{*inverse, bounds, radius, box.x0, box.y0, box.x1, box.y1});
                }
                else
                {
                    ++state.stats.singular_transforms;
                }
            }

            if (node->role == macos_layer_role::sdf_group)
            {
                render_sdf_group(state, layer, to_device, own_opacity, 0, state.backdrop_depth > 0);
            }
            else if (node->role == macos_layer_role::sdf_container && state.sdf_rendered.insert(layer).second)
            {
                render_sdf_container(state, *node, to_device, own_opacity);
            }

            state.ancestors.push_back(layer);
            const auto backdrop = node->role == macos_layer_role::backdrop;
            state.backdrop_depth += backdrop ? 1 : 0;

            for (const auto child : node->children)
            {
                draw(state, child, child_to_device, own_opacity, depth + 1);
            }

            state.backdrop_depth -= backdrop ? 1 : 0;

            state.ancestors.pop_back();
            state.clips.resize(clips_before);

            if (masked)
            {
                state.masks.pop_back();
            }

            state.mask_shapes.resize(mask_shapes_before);
        }
    }

    bool macos_layer_surface::valid() const
    {
        if (this->pixels == nullptr || this->width <= 0 || this->height <= 0)
        {
            return false;
        }

        return this->stride >= this->width * 4;
    }

    uint8_t macos_layer_channel_to_byte(const double value)
    {
        if (!std::isfinite(value) || value <= 0.0)
        {
            return 0;
        }

        if (value >= 1.0)
        {
            return 255;
        }

        return static_cast<uint8_t>(std::lround(value * 255.0));
    }

    void macos_layer_blend_pixel(uint8_t* destination, const uint8_t b, const uint8_t g, const uint8_t r, const uint8_t a)
    {
        if (a == 0)
        {
            return;
        }

        if (a == 255)
        {
            destination[0] = b;
            destination[1] = g;
            destination[2] = r;
            destination[3] = 255;
            return;
        }

        // A source whose colour channels exceed its own alpha is not validly premultiplied, and the sum
        // then leaves the 8-bit range. Saturating is what CoreGraphics does with an over-bright pixel;
        // letting it wrap turns one bad channel into a colour from the far side of the wheel.
        const uint32_t inverse = 255u - a;
        destination[0] = saturate(b + div255(destination[0] * inverse));
        destination[1] = saturate(g + div255(destination[1] * inverse));
        destination[2] = saturate(r + div255(destination[2] * inverse));
        destination[3] = saturate(a + div255(destination[3] * inverse));
    }

    std::optional<macos_layer_affine> macos_layer_transform_to_root(const macos_layer_tree& tree, const uint64_t root, const uint64_t layer)
    {
        if (tree.find(root) == nullptr || tree.find(layer) == nullptr)
        {
            return std::nullopt;
        }

        macos_layer_affine mapping{};
        auto current = layer;

        for (size_t step = 0; step < MACOS_LAYER_MAX_DEPTH; ++step)
        {
            if (current == root)
            {
                return mapping;
            }

            const auto* node = tree.find(current);
            if (node == nullptr)
            {
                return std::nullopt;
            }

            const auto* parent = tree.find(node->parent);
            if (parent == nullptr)
            {
                return std::nullopt;
            }

            mapping = mapping.then(node->to_superlayer()).then(parent->sublayer_mapping());
            current = node->parent;
        }

        return std::nullopt;
    }

    macos_layer_composite_stats macos_layer_composite(const macos_layer_tree& tree, const uint64_t root, const macos_layer_surface& surface,
                                                      const macos_layer_composite_options& options, const macos_layer_pixel_source& source)
    {
        macos_layer_composite_stats stats{};
        if (!surface.valid())
        {
            return stats;
        }

        if (options.clear)
        {
            for (int32_t y = 0; y < surface.height; ++y)
            {
                auto* row = surface.pixels + static_cast<size_t>(y) * static_cast<size_t>(surface.stride);
                std::fill_n(row, static_cast<size_t>(surface.width) * 4u, uint8_t{0});
            }
        }

        const auto scale = std::isfinite(options.scale) && options.scale > 0.0 ? options.scale : 1.0;
        const auto origin_x = std::isfinite(options.origin_x) ? options.origin_x : 0.0;
        const auto origin_y = std::isfinite(options.origin_y) ? options.origin_y : 0.0;

        macos_layer_affine base{};
        base.a = scale;
        base.d = options.flip_y ? -scale : scale;
        base.tx = -origin_x * scale;
        base.ty = options.flip_y ? static_cast<double>(surface.height) + origin_y * scale : -origin_y * scale;

        composite_state state{};
        state.tree = &tree;
        state.surface = &surface;
        state.source = &source;
        state.root_y_direction = base.d;

        draw(state, root, base, 1.0, 0);
        return state.stats;
    }
}
