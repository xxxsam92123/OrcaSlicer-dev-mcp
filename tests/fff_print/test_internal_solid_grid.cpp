#include <catch2/catch_all.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <tuple>
#include <utility>

#include "libslic3r/Fill/FillBase.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/InternalSolidGrid.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Surface.hpp"

#include "test_helpers.hpp"

using namespace Slic3r;

static ExPolygon make_square(double width, double height)
{
    return ExPolygon(Points {
        Point::new_scale(0., 0.),
        Point::new_scale(width, 0.),
        Point::new_scale(width, height),
        Point::new_scale(0., height)
    });
}

static double total_area(const Surfaces &surfaces)
{
    double result = 0.;
    for (const Surface &surface : surfaces)
        result += surface.area();
    return result;
}

TEST_CASE("Internal solid grid is scoped to internal solid infill", "[InternalSolidGrid]")
{
    REQUIRE(print_config_def.get("internal_solid_infill_pattern")->has_enum_value("internal_solid_grid"));
    REQUIRE_FALSE(print_config_def.get("top_surface_pattern")->has_enum_value("internal_solid_grid"));
    REQUIRE_FALSE(print_config_def.get("bottom_surface_pattern")->has_enum_value("internal_solid_grid"));
    REQUIRE_FALSE(print_config_def.get("sparse_infill_pattern")->has_enum_value("internal_solid_grid"));

    std::unique_ptr<Fill> fill(Fill::new_from_type("internal_solid_grid"));
    REQUIRE(fill != nullptr);
}

TEST_CASE("Internal solid grid preserves area at the requested resolution", "[InternalSolidGrid]")
{
    Surface solid(stInternalSolid, make_square(64., 32.));
    InternalSolidGridSettings settings;
    settings.cells_x = 4;
    settings.cells_y = 2;
    const Surfaces split = split_internal_solid_grid_surface(solid, settings);
    REQUIRE(split.size() == 8);
    REQUIRE_THAT(total_area(split), Catch::Matchers::WithinAbs(solid.area(), 1e-6));
    REQUIRE(split.front().internal_solid_grid);
    REQUIRE(split_internal_solid_grid_surface(split.front()).size() == 1);

    Surface small(stInternalSolid, make_square(7., 40.));
    REQUIRE(split_internal_solid_grid_surface(small).size() == 1);
}

TEST_CASE("Internal solid grid exposes bounded defaults", "[InternalSolidGrid]")
{
    const DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    REQUIRE(config.opt_int("internal_solid_grid_cells_x") == 2);
    REQUIRE(config.opt_int("internal_solid_grid_cells_y") == 2);
    REQUIRE_THAT(config.opt_float("internal_solid_grid_angle_step"), Catch::Matchers::WithinAbs(15., 1e-6));
    REQUIRE(config.opt_bool("internal_solid_grid_walls"));

    REQUIRE(print_config_def.get("internal_solid_grid_cells_x")->min == 1);
    REQUIRE(print_config_def.get("internal_solid_grid_cells_x")->max == 32);
    REQUIRE(print_config_def.get("internal_solid_grid_cells_y")->min == 1);
    REQUIRE(print_config_def.get("internal_solid_grid_cells_y")->max == 32);
}

TEST_CASE("Internal solid grid falls back atomically for unsafe requests", "[InternalSolidGrid]")
{
    Surface solid(stInternalSolid, make_square(40., 40.));
    InternalSolidGridSettings one_by_one;
    one_by_one.cells_x = 1;
    one_by_one.cells_y = 1;
    REQUIRE(split_internal_solid_grid_surface(solid, one_by_one).size() == 1);

    InternalSolidGridSettings too_many_cells;
    too_many_cells.cells_x = 8;
    too_many_cells.cells_y = 8;
    REQUIRE(split_internal_solid_grid_surface(solid, too_many_cells).size() == 1);

    // The first 16 mm cell contains 40 disconnected teeth. They are connected
    // through the part of the contour above that cell, so clipping that cell
    // produces more than MAX_GRID_FRAGMENTS ExPolygons.
    constexpr double tooth_pitch = 0.4;
    constexpr double tooth_width = 0.2;
    Points comb { Point::new_scale(0., 0.), Point::new_scale(0., 256.),
                  Point::new_scale(256., 256.), Point::new_scale(256., 16.),
                  Point::new_scale(16., 16.) };
    for (int tooth = 39; tooth >= 0; --tooth) {
        const double right = (tooth + 1) * tooth_pitch;
        const double left = tooth * tooth_pitch + tooth_width;
        comb.emplace_back(Point::new_scale(right, 0.));
        comb.emplace_back(Point::new_scale(left, 0.));
        comb.emplace_back(Point::new_scale(left, 16.));
        if (tooth > 0)
            comb.emplace_back(Point::new_scale(tooth * tooth_pitch, 16.));
    }
    comb.emplace_back(Point::new_scale(0., 16.));
    comb.emplace_back(Point::new_scale(0., 0.));
    Surface fragmented(stInternalSolid, ExPolygon(std::move(comb)));
    InternalSolidGridSettings fragmented_settings;
    fragmented_settings.cells_x = 16;
    fragmented_settings.cells_y = 16;
    REQUIRE(split_internal_solid_grid_surface(fragmented, fragmented_settings).size() == 1);

    InternalSolidGridSettings maximum;
    maximum.cells_x = 32;
    maximum.cells_y = 32;
    Surface large(stInternalSolid, make_square(256., 256.));
    REQUIRE(split_internal_solid_grid_surface(large, maximum).size() == 1024);
}

TEST_CASE("Internal solid grid metadata survives surface geometry replacement", "[InternalSolidGrid]")
{
    Surface source(stInternalSolid, make_square(40., 40.));
    source.internal_solid_grid = true;
    source.internal_solid_grid_index = 3;

    Surface replacement(source, ExPolygon(make_square(20., 20.)));

    REQUIRE(replacement.internal_solid_grid);
    REQUIRE(replacement.internal_solid_grid_index == 3);
}

TEST_CASE("Internal solid grid applies the configured alternating angle step", "[InternalSolidGrid]")
{
    std::unique_ptr<Fill> fill(Fill::new_from_type(ipInternalSolidGrid));
    fill->spacing = 1.;
    fill->angle = 0.f;

    FillParams params;
    params.pattern = ipInternalSolidGrid;
    params.density = 1.f;
    params.dont_adjust = true;
    params.extrusion_role = erSolidInfill;
    params.internal_solid_grid_cells_x = 2;
    params.internal_solid_grid_cells_y = 2;
    params.internal_solid_grid_angle_step = Geometry::deg2rad(15.f);
    Surface solid(stInternalSolid, make_square(40., 40.));

    const Surfaces cells = split_internal_solid_grid_surface(solid, {
        params.internal_solid_grid_cells_x,
        params.internal_solid_grid_cells_y
    });
    REQUIRE(cells.size() == 4);

    // FillRectilinear also emits short contour-connection segments. Inspect
    // only the longest segment in each cell so those segments cannot mask the
    // configured signed angle.
    for (const Surface &cell : cells) {
        const int cell_x = cell.internal_solid_grid_index % params.internal_solid_grid_cells_x;
        const int cell_y = cell.internal_solid_grid_index / params.internal_solid_grid_cells_x;
        const double expected_angle = ((cell_x + cell_y) & 1) ? 15. : -15.;
        double measured_angle = 0.;
        double measured_length = 0.;
        for (const Polyline &line : fill->fill_surface(&cell, params))
            for (size_t i = 1; i < line.points.size(); ++i) {
                const double dx = unscaled<double>(line.points[i].x() - line.points[i - 1].x());
                const double dy = unscaled<double>(line.points[i].y() - line.points[i - 1].y());
                const double length = std::hypot(dx, dy);
                if (length <= measured_length)
                    continue;
                measured_length = length;
                measured_angle = Geometry::rad2deg(std::atan2(dy, dx));
                while (measured_angle >= 90.) measured_angle -= 180.;
                while (measured_angle < -90.) measured_angle += 180.;
            }
        REQUIRE(measured_length > 10.);
        REQUIRE_THAT(measured_angle, Catch::Matchers::WithinAbs(expected_angle, 1.));
    }
}

TEST_CASE("Internal solid grid reaches final solid infill paths", "[InternalSolidGrid]")
{
    Print print;
    Test::init_and_process_print({ Test::cube(20) }, print,
                                  {{ "sparse_infill_density", "15%" },
                                   { "internal_solid_infill_pattern", "internal_solid_grid" },
                                   { "internal_solid_grid_cells_x", "2" },
                                   { "internal_solid_grid_cells_y", "2" },
                                   { "internal_solid_grid_angle_step", "15" },
                                   { "top_surface_pattern", "rectilinear" },
                                  { "bottom_surface_pattern", "rectilinear" },
                                  { "layer_height", 0.2 }});

    bool found_orthogonal_solid_fill = false;
    bool found_sparse_fill = false;
    bool found_top_fill = false;
    bool found_bottom_fill = false;
    for (const Layer *layer : print.objects().front()->layers()) {
        struct Directions {
            bool horizontal = false;
            bool vertical = false;
        };
        Directions solid, sparse, top, bottom;
        for (const LayerRegion *region : layer->regions()) {
            size_t solid_fill_collections = 0;
            size_t first_wall_collection = size_t(-1);
            size_t first_solid_collection = size_t(-1);
            for (size_t collection_index = 0; collection_index < region->fills.entities.size(); ++collection_index) {
                const ExtrusionEntity *entity = region->fills.entities[collection_index];
                const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(entity);
                REQUIRE(collection != nullptr);
                bool contains_solid_infill = false;
                bool contains_grid_wall = false;
                for (const ExtrusionEntity *child : collection->entities)
                    contains_solid_infill |= child->role() == erSolidInfill;
                for (const ExtrusionEntity *child : collection->entities)
                    contains_grid_wall |= child->role() == erPerimeter;
                solid_fill_collections += contains_solid_infill;
                if (contains_grid_wall)
                    first_wall_collection = std::min(first_wall_collection, collection_index);
                if (contains_solid_infill)
                    first_solid_collection = std::min(first_solid_collection, collection_index);
            }
            REQUIRE(solid_fill_collections <= 1);
            if (first_wall_collection != size_t(-1))
                REQUIRE(first_wall_collection < first_solid_collection);

            for (const ExtrusionEntity *entity : region->fills.flatten().entities) {
                const auto account = [&solid, &sparse, &top, &bottom](const ExtrusionPath &path) {
                    Directions *directions = nullptr;
                    switch (path.role()) {
                    case erSolidInfill:    directions = &solid; break;
                    case erInternalInfill: directions = &sparse; break;
                    case erTopSolidInfill: directions = &top; break;
                    case erBottomSurface:  directions = &bottom; break;
                    default: return;
                    }
                    if (directions == nullptr)
                        return;
                    for (size_t i = 1; i < path.polyline.points.size(); ++i) {
                        const double dx = unscaled<double>(path.polyline.points[i].x() - path.polyline.points[i - 1].x());
                        const double dy = unscaled<double>(path.polyline.points[i].y() - path.polyline.points[i - 1].y());
                        if (std::abs(dx) > std::abs(dy) * 4.) directions->horizontal = true;
                        if (std::abs(dy) > std::abs(dx) * 4.) directions->vertical = true;
                    }
                };
                if (const auto *path = dynamic_cast<const ExtrusionPath *>(entity))
                    account(*path);
                else if (const auto *multi = dynamic_cast<const ExtrusionMultiPath *>(entity))
                    for (const ExtrusionPath &path : multi->paths)
                        account(path);
                else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(entity))
                    for (const ExtrusionPath &path : loop->paths)
                        account(path);
            }
        }
        found_orthogonal_solid_fill |= solid.horizontal && solid.vertical;
        found_sparse_fill |= sparse.horizontal || sparse.vertical;
        found_top_fill |= top.horizontal || top.vertical;
        found_bottom_fill |= bottom.horizontal || bottom.vertical;
    }

    REQUIRE(found_orthogonal_solid_fill);
    REQUIRE(found_sparse_fill);
    REQUIRE(found_top_fill);
    REQUIRE(found_bottom_fill);
}

TEST_CASE("Internal solid grid walls are continuous and honor fallback", "[InternalSolidGrid]")
{
    const auto inspect = [](int cells_x) {
        Print print;
        Test::init_and_process_print({ Test::cube(30) }, print,
                                      {{ "wall_loops", "0" },
                                       { "sparse_infill_density", "15%" },
                                       { "internal_solid_infill_pattern", "internal_solid_grid" },
                                       { "internal_solid_grid_cells_x", std::to_string(cells_x) },
                                       { "internal_solid_grid_cells_y", "2" },
                                       { "top_surface_pattern", "rectilinear" },
                                       { "bottom_surface_pattern", "rectilinear" },
                                       { "layer_height", 0.2 }});
        size_t wall_collections = 0;
        size_t continuous_wall_paths = 0;

        bool ordered = true;
        for (const Layer *layer : print.objects().front()->layers())
            for (const LayerRegion *region : layer->regions()) {
                size_t first_wall = size_t(-1);
                size_t first_solid = size_t(-1);
                for (size_t i = 0; i < region->fills.entities.size(); ++i) {
                    const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(region->fills.entities[i]);
                    REQUIRE(collection != nullptr);
                    bool has_wall = false;
                    bool has_solid = false;
                    for (const ExtrusionEntity *entity : collection->entities) {
                        has_wall |= entity->role() == erPerimeter;
                        has_solid |= entity->role() == erSolidInfill;
                    }
                    if (has_wall) {
                        ++wall_collections;
                        first_wall = std::min(first_wall, i);
                        REQUIRE(collection->no_sort);
                        REQUIRE(collection->internal_solid_infill_wall);
                        for (const ExtrusionEntity *entity : collection->entities) {
                            REQUIRE(entity->role() == erPerimeter);
                            const auto *path = dynamic_cast<const ExtrusionPath *>(entity);
                            if (path != nullptr) {
                                if (path->polyline.points.size() >= 3)
                                    ++continuous_wall_paths;
                            }
                        }
                    }
                    if (has_solid)
                        first_solid = std::min(first_solid, i);
                }
                if (first_wall != size_t(-1))
                    ordered &= first_solid != size_t(-1) && first_wall < first_solid;
            }
        return std::make_tuple(wall_collections, continuous_wall_paths, ordered);
    };

    const auto enabled = inspect(3);
    REQUIRE(std::get<0>(enabled) > 0);
    REQUIRE(std::get<1>(enabled) > 0);
    REQUIRE(std::get<2>(enabled));
    REQUIRE(std::get<0>(inspect(1)) == 0);
}
