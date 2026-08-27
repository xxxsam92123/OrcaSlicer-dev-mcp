#ifndef slic3r_ExternalBridgeGrid_hpp_
#define slic3r_ExternalBridgeGrid_hpp_

#include "Surface.hpp"

namespace Slic3r {

struct ExternalBridgeGridSettings
{
    bool enabled { false };
    int cells_x { 1 };
    int cells_y { 1 };
    double angle_step_deg { 0.0 };
};

// Split an external bridge before fill generation. Invalid or unsafe requests
// return the original surface as one complete region.
Surfaces split_external_bridge_surface(
    const Surface &surface, const ExternalBridgeGridSettings &settings);

// Return the shared cell boundaries of a successfully split bridge surface.
// The returned paths are open center lines and contain no outer boundary.
Polylines external_bridge_grid_walls(const Surfaces &cells);

} // namespace Slic3r

#endif // slic3r_ExternalBridgeGrid_hpp_
