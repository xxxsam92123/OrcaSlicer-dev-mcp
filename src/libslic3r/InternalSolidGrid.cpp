#include "InternalSolidGrid.hpp"
#include "BoundingBox.hpp"
#include "ClipperUtils.hpp"
#include "libslic3r.h"
#include <algorithm>
#include <cmath>
#include <utility>

namespace Slic3r {
namespace {
constexpr int    MAX_GRID_AXIS        = 32;
constexpr int    MAX_GRID_CELLS       = 1024;
constexpr size_t MAX_GRID_FRAGMENTS   = 32;
constexpr double MIN_CELL_MM          = 8.;
constexpr double MIN_SURFACE_AREA_MM2 = 100.;
Surfaces full_surface(const Surface &surface) { return Surfaces { surface }; }
}

Surfaces split_internal_solid_grid_surface(const Surface &surface, const InternalSolidGridSettings &settings)
{
    if (surface.empty() || surface.surface_type != stInternalSolid || surface.internal_solid_grid ||
        surface.area() * SCALING_FACTOR * SCALING_FACTOR < MIN_SURFACE_AREA_MM2)
        return full_surface(surface);

    const int cells_x = std::clamp(settings.cells_x, 1, MAX_GRID_AXIS);
    const int cells_y = std::clamp(settings.cells_y, 1, MAX_GRID_AXIS);
    if (cells_x <= 1 || cells_y <= 1 || cells_x > MAX_GRID_CELLS / cells_y)
        return full_surface(surface);

    const BoundingBox bbox = get_extents(surface.expolygon);
    if (!bbox.defined || bbox.size().x() <= 0 || bbox.size().y() <= 0 ||
        unscaled<double>(bbox.size().x()) / cells_x < MIN_CELL_MM ||
        unscaled<double>(bbox.size().y()) / cells_y < MIN_CELL_MM)
        return full_surface(surface);

    Surfaces result;
    size_t fragment_count = 0;
    const coord_t width = bbox.size().x(), height = bbox.size().y();
    for (int y = 0; y < cells_y; ++y)
        for (int x = 0; x < cells_x; ++x) {
            const coord_t x0 = bbox.min.x() + coord_t(std::floor(coordf_t(width) * x / cells_x));
            const coord_t x1 = bbox.min.x() + coord_t(std::floor(coordf_t(width) * (x + 1) / cells_x));
            const coord_t y0 = bbox.min.y() + coord_t(std::floor(coordf_t(height) * y / cells_y));
            const coord_t y1 = bbox.min.y() + coord_t(std::floor(coordf_t(height) * (y + 1) / cells_y));
            if (x1 <= x0 || y1 <= y0 || unscaled<double>(x1 - x0) < MIN_CELL_MM ||
                unscaled<double>(y1 - y0) < MIN_CELL_MM)
                return full_surface(surface);
            const Polygon cell { Points { Point(x0, y0), Point(x1, y0), Point(x1, y1), Point(x0, y1) } };
            ExPolygons clipped = intersection_ex(surface.expolygon, Polygons { cell });
            if (clipped.size() > 1)
                fragment_count += clipped.size() - 1;
            if (fragment_count > MAX_GRID_FRAGMENTS)
                return full_surface(surface);
            for (ExPolygon &expoly : clipped)
                if (!expoly.empty()) {
                    result.emplace_back(surface, std::move(expoly));
                    result.back().internal_solid_grid = true;
                    result.back().internal_solid_grid_index = static_cast<unsigned short>(y * cells_x + x);
                }
        }
    if (result.size() <= 1)
        return full_surface(surface);
    double result_area = 0.;
    for (const Surface &cell : result) result_area += cell.area();
    const double tolerance = std::max(1., std::abs(surface.area()) * 1e-9);
    return std::abs(result_area - surface.area()) <= tolerance ? result : full_surface(surface);
}
} // namespace Slic3r
