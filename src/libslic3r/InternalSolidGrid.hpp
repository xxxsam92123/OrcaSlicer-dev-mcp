#pragma once

#include "Surface.hpp"

namespace Slic3r {

struct InternalSolidGridSettings
{
    int cells_x { 2 };
    int cells_y { 2 };
};

Surfaces split_internal_solid_grid_surface(const Surface &surface,
                                           const InternalSolidGridSettings &settings = InternalSolidGridSettings());

} // namespace Slic3r
