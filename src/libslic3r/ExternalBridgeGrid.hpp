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

} // namespace Slic3r

#endif // slic3r_ExternalBridgeGrid_hpp_
