#include "ExternalBridgeGrid.hpp"

#include "BoundingBox.hpp"
#include "ClipperUtils.hpp"
#include "Geometry.hpp"
#include "libslic3r.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace Slic3r {

namespace {

constexpr int    MAX_GRID_AXIS      = 32;
constexpr int    MAX_GRID_CELLS     = 1024;
constexpr size_t MAX_GRID_FRAGMENTS = 32;
constexpr double MIN_CELL_MM        = 8.;
constexpr double MIN_SURFACE_AREA_MM2 = 100.;

struct GridCandidate
{
    int cells_x;
    int cells_y;
};

Surfaces full_surface(const Surface &surface)
{
    return Surfaces { surface };
}

std::vector<GridCandidate> make_grid_candidates(const BoundingBox &bbox,
                                                const ExternalBridgeGridSettings &settings)
{
    const int requested_x = std::clamp(settings.cells_x, 1, MAX_GRID_AXIS);
    const int requested_y = std::clamp(settings.cells_y, 1, MAX_GRID_AXIS);
    const double width_mm = unscaled<double>(bbox.size().x());
    const double height_mm = unscaled<double>(bbox.size().y());
    const int max_x = std::min(requested_x, int(std::floor(width_mm / MIN_CELL_MM + 1e-9)));
    const int max_y = std::min(requested_y, int(std::floor(height_mm / MIN_CELL_MM + 1e-9)));

    std::vector<GridCandidate> candidates;
    for (int cells_y = 1; cells_y <= max_y; ++cells_y)
        for (int cells_x = 1; cells_x <= max_x; ++cells_x)
            if (cells_x * cells_y <= MAX_GRID_CELLS)
                candidates.push_back({ cells_x, cells_y });

    std::sort(candidates.begin(), candidates.end(), [width_mm, height_mm](const GridCandidate &lhs, const GridCandidate &rhs) {
        const int lhs_count = lhs.cells_x * lhs.cells_y;
        const int rhs_count = rhs.cells_x * rhs.cells_y;
        if (lhs_count != rhs_count)
            return lhs_count > rhs_count;

        const double lhs_cell_aspect = width_mm * lhs.cells_y / (height_mm * lhs.cells_x);
        const double rhs_cell_aspect = width_mm * rhs.cells_y / (height_mm * rhs.cells_x);
        const double lhs_aspect_error = std::abs(std::log(lhs_cell_aspect));
        const double rhs_aspect_error = std::abs(std::log(rhs_cell_aspect));
        if (lhs_aspect_error != rhs_aspect_error)
            return lhs_aspect_error < rhs_aspect_error;

        return lhs.cells_x > rhs.cells_x;
    });
    return candidates;
}

std::optional<Surfaces> split_for_grid(const Surface &surface, const BoundingBox &bbox,
                                       int cells_x, int cells_y, double angle_step)
{
    const coord_t bbox_width = bbox.size().x();
    const coord_t bbox_height = bbox.size().y();

    Surfaces result;
    size_t fragment_count = 0;
    for (int y = 0; y < cells_y; ++y) {
        for (int x = 0; x < cells_x; ++x) {
            const coord_t x0 = bbox.min.x() + coord_t(std::floor(coordf_t(bbox_width) * x / cells_x));
            const coord_t x1 = bbox.min.x() + coord_t(std::floor(coordf_t(bbox_width) * (x + 1) / cells_x));
            const coord_t y0 = bbox.min.y() + coord_t(std::floor(coordf_t(bbox_height) * y / cells_y));
            const coord_t y1 = bbox.min.y() + coord_t(std::floor(coordf_t(bbox_height) * (y + 1) / cells_y));
            if (x1 <= x0 || y1 <= y0 ||
                unscaled<double>(x1 - x0) < MIN_CELL_MM || unscaled<double>(y1 - y0) < MIN_CELL_MM)
                return std::nullopt;

            const Polygon cell { Points {
                Point(x0, y0), Point(x1, y0), Point(x1, y1), Point(x0, y1)
            } };
            ExPolygons clipped = intersection_ex(surface.expolygon, Polygons { cell });
            // A normal cell with one clipped polygon is not a geometric
            // fragment. Only additional pieces created by clipping consume
            // the fragment safety budget, so large regular grids are allowed.
            if (clipped.size() > 1)
                fragment_count += clipped.size() - 1;
            if (fragment_count > MAX_GRID_FRAGMENTS)
                return std::nullopt;

            const double cell_angle = surface.bridge_angle + (((x + y) & 1) ? angle_step : -angle_step);
            for (ExPolygon &expolygon : clipped)
                if (!expolygon.empty()) {
                    Surface split_surface(surface, std::move(expolygon));
                    split_surface.bridge_angle = cell_angle;
                    split_surface.external_bridge_grid = true;
                    result.emplace_back(std::move(split_surface));
                }
        }
    }

    if (result.size() <= 1)
        return std::nullopt;

    double result_area = 0.;
    for (const Surface &split_surface : result)
        result_area += split_surface.area();
    const double area_tolerance = std::max(1., std::abs(surface.area()) * 1e-9);
    if (std::abs(result_area - surface.area()) > area_tolerance)
        return std::nullopt;

    return result;
}

} // namespace

Surfaces split_external_bridge_surface(const Surface &surface,
                                       const ExternalBridgeGridSettings &settings)
{
    if (!settings.enabled || surface.empty() || surface.surface_type != stBottomBridge ||
        !std::isfinite(settings.angle_step_deg) || settings.angle_step_deg == 0. ||
        surface.area() * SCALING_FACTOR * SCALING_FACTOR < MIN_SURFACE_AREA_MM2)
        return full_surface(surface);

    const BoundingBox bbox = get_extents(surface.expolygon);
    if (!bbox.defined || bbox.size().x() <= 0 || bbox.size().y() <= 0)
        return full_surface(surface);

    const double angle_step = Geometry::deg2rad(settings.angle_step_deg);
    for (const GridCandidate &candidate : make_grid_candidates(bbox, settings)) {
        if (candidate.cells_x == 1 && candidate.cells_y == 1)
            continue;
        if (std::optional<Surfaces> result = split_for_grid(
                surface, bbox, candidate.cells_x, candidate.cells_y, angle_step))
            return std::move(*result);
    }
    return full_surface(surface);
}

} // namespace Slic3r
