#include <catch2/catch_all.hpp>

#include <algorithm>
#include <cmath>
#include <set>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/ExternalBridgeGrid.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Surface.hpp"

#include "test_helpers.hpp"

using namespace Slic3r;

static ExPolygon make_square(double width, double height)
{
    Points points {
        Point::new_scale(0, 0),
        Point::new_scale(width, 0),
        Point::new_scale(width, height),
        Point::new_scale(0, height)
    };
    return ExPolygon(std::move(points));
}

static double total_area(const Surfaces &surfaces)
{
    double result = 0.;
    for (const Surface &surface : surfaces)
        result += surface.area();
    return result;
}

TEST_CASE("External bridge grid falls back to the complete surface", "[ExternalBridgeGrid]")
{
    Surface bridge(stBottomBridge, make_square(20, 20));

    ExternalBridgeGridSettings settings;
    REQUIRE(split_external_bridge_surface(bridge, settings).size() == 1);

    settings.enabled = true;
    settings.cells_x = 2;
    settings.cells_y = 2;
    settings.angle_step_deg = 0.;
    REQUIRE(split_external_bridge_surface(bridge, settings).size() == 1);

    settings.angle_step_deg = 15.;
    Surface small_bridge(stBottomBridge, make_square(5, 5));
    REQUIRE(split_external_bridge_surface(small_bridge, settings).size() == 1);
}

TEST_CASE("External bridge grid preserves surface area", "[ExternalBridgeGrid]")
{
    Surface bridge(stBottomBridge, make_square(64, 64));
    bridge.bridge_angle = 0.;

    ExternalBridgeGridSettings settings;
    settings.enabled = true;
    settings.cells_x = 4;
    settings.cells_y = 4;
    settings.angle_step_deg = 15.;
    const Surfaces split = split_external_bridge_surface(bridge, settings);

    REQUIRE(split.size() == 16);
    REQUIRE(total_area(split) == Catch::Approx(bridge.area()));
}

TEST_CASE("External bridge grid honors requested resolution", "[ExternalBridgeGrid]")
{
    for (const int cells : { 5, 8, 32 }) {
        Surface bridge(stBottomBridge, make_square(cells * 8., cells * 8.));
        ExternalBridgeGridSettings settings;
        settings.enabled = true;
        settings.cells_x = cells;
        settings.cells_y = cells;
        settings.angle_step_deg = 15.;

        const Surfaces split = split_external_bridge_surface(bridge, settings);
        REQUIRE(split.size() == size_t(cells * cells));
        REQUIRE(total_area(split) == Catch::Approx(bridge.area()));
    }
}

TEST_CASE("External bridge grid configuration allows 32 cells per axis", "[ExternalBridgeGrid]")
{
    REQUIRE(print_config_def.get("external_bridge_grid_cells_x")->min == 1);
    REQUIRE(print_config_def.get("external_bridge_grid_cells_x")->max == 32);
    REQUIRE(print_config_def.get("external_bridge_grid_cells_y")->min == 1);
    REQUIRE(print_config_def.get("external_bridge_grid_cells_y")->max == 32);
}

TEST_CASE("External bridge grid caps the bounded grid", "[ExternalBridgeGrid]")
{
    Surface bridge(stBottomBridge, make_square(256, 256));
    bridge.bridge_angle = 0.;

    ExternalBridgeGridSettings settings;
    settings.enabled = true;
    settings.cells_x = 64;
    settings.cells_y = 64;
    settings.angle_step_deg = 15.;
    const Surfaces split = split_external_bridge_surface(bridge, settings);

    REQUIRE(split.size() == 1024);
}

TEST_CASE("External bridge grid alternates bridge angles", "[ExternalBridgeGrid]")
{
    Surface bridge(stBottomBridge, make_square(32, 32));
    bridge.bridge_angle = Geometry::deg2rad(90.);

    ExternalBridgeGridSettings settings;
    settings.enabled = true;
    settings.cells_x = 2;
    settings.cells_y = 2;
    settings.angle_step_deg = 15.;
    const Surfaces split = split_external_bridge_surface(bridge, settings);

    REQUIRE(split.size() == 4);
    size_t negative_offset = 0;
    size_t positive_offset = 0;
    for (const Surface &surface : split) {
        if (surface.bridge_angle == Catch::Approx(Geometry::deg2rad(75.)))
            ++negative_offset;
        if (surface.bridge_angle == Catch::Approx(Geometry::deg2rad(105.)))
            ++positive_offset;
    }
    REQUIRE(negative_offset == 2);
    REQUIRE(positive_offset == 2);
}

TEST_CASE("External bridge grid emits bridge walls only for split cells", "[ExternalBridgeGrid]")
{
    struct BridgeGridEntities {
        std::set<int> surface_angles;
        std::set<int> path_angles;
        size_t        cell_count = 0;
        size_t        bridge_wall_loops = 0;
        size_t        first_wall_entity = size_t(-1);
        size_t        first_bridge_fill_entity = size_t(-1);
        size_t        wall_entities_before_bridge = 0;
        BoundingBox   bridge_bbox;
    };

    const auto collect_entities = [](bool enabled, double angle_step) {
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "external_bridge_grid_enable", enabled ? "1" : "0" },
        { "external_bridge_grid_cells_x", "2" },
        { "external_bridge_grid_cells_y", "2" },
        { "external_bridge_grid_angle_step", std::to_string(angle_step) },
        { "wall_loops", "0" }
    });

    Print print;
    TriangleMesh model_mesh = Test::mesh(Test::TestMesh::bridge_with_hole);
    model_mesh.scale(Vec3f(1.f, 2.f, 1.f));
    Test::init_and_process_print({ std::move(model_mesh) }, print, config);

    BridgeGridEntities result;
    for (const Layer *layer : print.objects().front()->layers()) {
        for (const LayerRegion *region : layer->regions()) {
            for (const Surface &surface : region->fill_surfaces.surfaces)
                if (surface.surface_type == stBottomBridge && surface.bridge_angle >= 0.) {
                    result.surface_angles.insert(int(std::lround(Geometry::rad2deg(surface.bridge_angle))) % 180);
                    result.cell_count += surface.external_bridge_grid;
                    result.bridge_bbox.merge(get_extents(surface.expolygon));
                }
            for (size_t collection_index = 0; collection_index < region->fills.entities.size(); ++collection_index) {
                const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(region->fills.entities[collection_index]);
                REQUIRE(collection != nullptr);
                bool has_wall = false;
                bool has_bridge_fill = false;
                for (const ExtrusionEntity *entity : collection->entities) {
                    has_wall |= entity->role() == erOverhangPerimeter;
                    has_bridge_fill |= entity->role() == erBridgeInfill;
                }
                if (has_wall)
                    result.first_wall_entity = std::min(result.first_wall_entity, collection_index);
                if (has_bridge_fill)
                    result.first_bridge_fill_entity = std::min(result.first_bridge_fill_entity, collection_index);
            }
            for (const ExtrusionEntity *entity : region->fills.flatten().entities) {
                const auto account = [&result](const ExtrusionPath &path) {
                    if (path.role() != erBridgeInfill)
                        return;
                    const Points3 &points = path.polyline.points;
                    for (size_t i = 1; i < points.size(); ++i) {
                        const double dx = double(points[i].x() - points[i - 1].x());
                        const double dy = double(points[i].y() - points[i - 1].y());
                        if (std::hypot(dx, dy) > 0.)
                            result.path_angles.insert(int(std::lround(Geometry::rad2deg(std::atan2(dy, dx)))) % 180);
                    }
                };
                if (const auto *path = dynamic_cast<const ExtrusionPath *>(entity)) {
                    if (path->role() == erOverhangPerimeter)
                        ++result.bridge_wall_loops;
                    else
                        account(*path);
                }
                else if (const auto *multi = dynamic_cast<const ExtrusionMultiPath *>(entity))
                    for (const ExtrusionPath &path : multi->paths)
                        account(path);
                else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(entity))
                    for (const ExtrusionPath &path : loop->paths)
                        account(path);
            }
        }
    }

    REQUIRE(result.bridge_bbox.defined);
    REQUIRE(unscaled<double>(result.bridge_bbox.size().x()) >= 32.);
    REQUIRE(unscaled<double>(result.bridge_bbox.size().y()) >= 32.);
    return result;
    };

    const auto disabled = collect_entities(false, 15.);
    REQUIRE(disabled.surface_angles.size() == 1);
    REQUIRE(disabled.cell_count == 0);
    REQUIRE(disabled.bridge_wall_loops == 0);

    const auto fallback = collect_entities(true, 0.);
    REQUIRE(fallback.cell_count == 0);
    REQUIRE(fallback.bridge_wall_loops == 0);

    const auto enabled = collect_entities(true, 15.);
    REQUIRE(enabled.cell_count == 4);
    REQUIRE(enabled.surface_angles.size() >= 2);
    REQUIRE(enabled.path_angles.size() >= 2);
    REQUIRE(enabled.first_wall_entity != size_t(-1));
    REQUIRE(enabled.first_bridge_fill_entity != size_t(-1));
    REQUIRE(enabled.first_wall_entity < enabled.first_bridge_fill_entity);
    // A 2x2 grid has one vertical and one horizontal internal grid line. The
    // center hole may split each line into multiple segments, so at least two
    // overhang-perimeter wall paths must be emitted for the split cells.
    REQUIRE(enabled.bridge_wall_loops >= 2);
}
