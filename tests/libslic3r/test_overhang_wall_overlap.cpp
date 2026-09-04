#include <catch2/catch_all.hpp>

#include "libslic3r/PerimeterGenerator.hpp"

using namespace Slic3r;

TEST_CASE("overhang wall overlap derives a bounded center spacing", "[OverhangWallOverlap]")
{
    const coord_t layer_height = scaled<coord_t>(0.20);
    const coord_t nominal_spacing = scaled<coord_t>(0.45);
    const coord_t overhang_width = scaled<coord_t>(0.50);

    CHECK(overhang_wall_spacing(nominal_spacing, overhang_width, layer_height, 0.) == nominal_spacing);
    CHECK(overhang_wall_spacing(nominal_spacing, overhang_width, layer_height, 20.) == scaled<coord_t>(0.35));
    CHECK(overhang_wall_spacing(nominal_spacing, overhang_width, layer_height, 100.) == layer_height);
    CHECK(overhang_wall_spacing(nominal_spacing, overhang_width, layer_height, 150.) == layer_height);
}