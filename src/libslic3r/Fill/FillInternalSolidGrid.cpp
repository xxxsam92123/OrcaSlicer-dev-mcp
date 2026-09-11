#include "FillInternalSolidGrid.hpp"

#include "../InternalSolidGrid.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace Slic3r {
namespace {

float cell_angle(float angle, unsigned short cell_index, int cells_x, float angle_step)
{
    cells_x = std::max(cells_x, 1);
    const int cell_x = cell_index % cells_x;
    const int cell_y = cell_index / cells_x;
    return angle + (((cell_x + cell_y) & 1) ? angle_step : -angle_step);
}

} // namespace

Polylines FillInternalSolidGrid::fill_surface(const Surface *surface, const FillParams &params)
{
    if (surface == nullptr || surface->surface_type != stInternalSolid ||
        params.pattern != ipInternalSolidGrid || params.extrusion_role != erSolidInfill)
        return FillRectilinear::fill_surface(surface, params);

    struct RestoreAngle {
        float &angle;
        float original_angle;
        ~RestoreAngle() { angle = original_angle; }
    } restore_angle { angle, angle };

    const InternalSolidGridSettings settings { params.internal_solid_grid_cells_x, params.internal_solid_grid_cells_y };
    if (surface->internal_solid_grid) {
        angle = cell_angle(restore_angle.original_angle, surface->internal_solid_grid_index,
                           settings.cells_x, params.internal_solid_grid_angle_step);
        return FillRectilinear::fill_surface(surface, params);
    }

    const Surfaces cells = split_internal_solid_grid_surface(*surface, settings);
    if (cells.size() == 1)
        return FillRectilinear::fill_surface(surface, params);

    Polylines result;
    result.reserve(cells.size());
    for (const Surface &cell : cells) {
        angle = cell_angle(restore_angle.original_angle, cell.internal_solid_grid_index,
                           settings.cells_x, params.internal_solid_grid_angle_step);
        Polylines cell_lines = FillRectilinear::fill_surface(&cell, params);
        result.insert(result.end(), std::make_move_iterator(cell_lines.begin()), std::make_move_iterator(cell_lines.end()));
    }
    return result;
}

} // namespace Slic3r
