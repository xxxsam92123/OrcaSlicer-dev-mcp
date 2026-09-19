#include <catch2/catch_all.hpp>

#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "test_helpers.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

// The layer at this Z is the last one of the base, so its top surface is the ledge.
const double ledge_z = 5.0;

// The first layer, at initial_layer_print_height.
const double first_layer_z = 0.2;

// TestMesh::step scaled 3x in X/Y: a 60x60x5 base carrying a 54x54 column up to z=10, leaving a 3mm
// top ledge around a feature that keeps rising. That is the geometry both only_one_wall_top and the
// top surface expansion act on. The ledge has to stay wider than the wall band plus two top-infill
// lines, or the expansion discards it as a sliver and the tests below assert nothing.
TriangleMesh step_with_ledge()
{
    TriangleMesh m = Slic3r::Test::mesh(TestMesh::step);
    m.scale(Vec3f(3.f, 3.f, 1.f));
    return m;
}

// Every setting the assertions depend on, so none of them rests on a default.
DynamicPrintConfig base_config(const char *wall_generator)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "wall_generator",             wall_generator },
        { "layer_height",               0.2 },  // puts a layer boundary exactly on ledge_z
        { "initial_layer_print_height", 0.2 },
        { "wall_loops",                 3 },
        { "sparse_infill_density",      "15%" },
        { "top_shell_layers",           3 },
        { "bottom_shell_layers",        3 },
        { "top_surface_density",        "100%" },
        { "top_surface_expansion",      0.0 },
        { "only_one_wall_top",          false },
        { "only_one_wall_first_layer",  false },
        // Do not let the one-wall threshold discard the 3mm ledge before the feature sees it.
        { "min_width_top_surface",      0.0 },
    });
    return config;
}

double collection_length(const ExtrusionEntityCollection &coll)
{
    double len = 0.;
    for (const ExtrusionEntity *entity : coll.flatten().entities)
        if (! entity->is_collection())
            len += entity->length();
    return len;
}

// Extruded length per layer. Two slices are compared through this rather than through their G-code,
// because the G-code carries a config block that differs whenever any setting differs.
struct SliceLengths {
    std::vector<double> perimeters;
    std::vector<double> fills;
};

SliceLengths slice_lengths(const Print &print)
{
    SliceLengths out;
    for (const Layer *layer : print.objects().front()->layers()) {
        double perimeters = 0., fills = 0.;
        for (const LayerRegion *region : layer->regions()) {
            perimeters += collection_length(region->perimeters);
            fills      += collection_length(region->fills);
        }
        out.perimeters.push_back(perimeters);
        out.fills.push_back(fills);
    }
    return out;
}

double perimeter_length_at(const Print &print, double print_z)
{
    for (const Layer *layer : print.objects().front()->layers())
        if (std::abs(layer->print_z - print_z) < 1e-4) {
            double len = 0.;
            for (const LayerRegion *region : layer->regions())
                len += collection_length(region->perimeters);
            return len;
        }
    return 0.;
}

// Largest per-layer difference between two series; a negative result means they are not comparable.
double max_difference(const std::vector<double> &a, const std::vector<double> &b)
{
    if (a.size() != b.size() || a.empty())
        return -1.;
    double worst = 0.;
    for (size_t i = 0; i < a.size(); ++ i)
        worst = std::max(worst, std::abs(a[i] - b[i]));
    return worst;
}

} // namespace

// The expansion only retypes area as top solid infill, so it can do nothing where there is no top
// fill to begin with: zero top shell layers retypes the top surfaces as internal, and a top surface
// density of 0% leaves the top layer with walls only. The last section is the control - the same
// expansion on the same model does change the slice once a top fill exists - without which the two
// equality checks above it would hold for an unrelated reason.
TEST_CASE("Top surface expansion only acts where there is a top fill", "[Perimeters]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    CAPTURE(wall_generator);

    auto lengths_for = [wall_generator](int top_shell_layers, const char *top_surface_density, double expansion) {
        DynamicPrintConfig config = base_config(wall_generator);
        config.set_deserialize_strict({
            { "top_shell_layers",      top_shell_layers },
            { "top_surface_density",   top_surface_density },
            { "top_surface_expansion", expansion },
        });
        Print print;
        init_and_process_print({ step_with_ledge() }, print, config);
        REQUIRE_FALSE(print.objects().empty());
        return slice_lengths(print);
    };

    SECTION("no top shell layers") {
        const SliceLengths off = lengths_for(0, "100%", 0.0);
        const SliceLengths on  = lengths_for(0, "100%", 2.0);
        REQUIRE(off.perimeters.size() == on.perimeters.size());
        CHECK_THAT(max_difference(off.perimeters, on.perimeters), Catch::Matchers::WithinAbs(0., 1.0));
        CHECK_THAT(max_difference(off.fills,      on.fills),      Catch::Matchers::WithinAbs(0., 1.0));
    }

    SECTION("zero top surface density") {
        const SliceLengths off = lengths_for(3, "0%", 0.0);
        const SliceLengths on  = lengths_for(3, "0%", 2.0);
        REQUIRE(off.perimeters.size() == on.perimeters.size());
        CHECK_THAT(max_difference(off.perimeters, on.perimeters), Catch::Matchers::WithinAbs(0., 1.0));
        CHECK_THAT(max_difference(off.fills,      on.fills),      Catch::Matchers::WithinAbs(0., 1.0));
    }

    SECTION("with a top fill the same expansion does change the slice") {
        const SliceLengths off = lengths_for(3, "100%", 0.0);
        const SliceLengths on  = lengths_for(3, "100%", 2.0);
        REQUIRE(off.fills.size() == on.fills.size());
        CHECK(max_difference(off.fills, on.fills) > scale_(0.5));
    }
}

// With no top shell the top surfaces are retyped as internal, so the top surface density has nothing
// left to control: there is no top fill, and only_one_wall_top - the one route from the density to the
// perimeters - is itself switched off for want of a top surface to act on.
TEST_CASE("Top surface density does not affect a slice without a top shell", "[Perimeters]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    CAPTURE(wall_generator);

    auto lengths_for = [wall_generator](const char *top_surface_density) {
        DynamicPrintConfig config = base_config(wall_generator);
        config.set_deserialize_strict({
            { "top_shell_layers",    0 },
            { "only_one_wall_top",   true },
            { "top_surface_density", top_surface_density },
        });
        Print print;
        init_and_process_print({ step_with_ledge() }, print, config);
        REQUIRE_FALSE(print.objects().empty());
        return slice_lengths(print);
    };

    const SliceLengths solid = lengths_for("100%");
    const SliceLengths none  = lengths_for("0%");
    REQUIRE(solid.perimeters.size() == none.perimeters.size());
    CHECK_THAT(max_difference(solid.perimeters, none.perimeters), Catch::Matchers::WithinAbs(0., 1.0));
    CHECK_THAT(max_difference(solid.fills,      none.fills),      Catch::Matchers::WithinAbs(0., 1.0));
}

// On the ledge layer the inner walls are given up to the top fill, so that layer loses wall length.
// The handover needs a top fill that reaches the freed space: at a top surface density of 0% there is
// no top fill at all, and without top_surface_expansion the fill never grows over the walls. Either
// way the feature still runs, through the original generation, which keeps the inner walls up to the
// top boundary - putting that layer back between the plain and the one-wall slice.
TEST_CASE("Only one wall on top surfaces drops inner walls only where a top fill replaces them", "[Perimeters]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    CAPTURE(wall_generator);

    auto ledge_perimeters_for = [wall_generator](bool only_one_wall_top, const char *top_surface_density, double expansion) {
        DynamicPrintConfig config = base_config(wall_generator);
        config.set_deserialize_strict({
            { "only_one_wall_top",     only_one_wall_top },
            { "top_surface_density",   top_surface_density },
            { "top_surface_expansion", expansion },
        });
        Print print;
        init_and_process_print({ step_with_ledge() }, print, config);
        REQUIRE_FALSE(print.objects().empty());
        return perimeter_length_at(print, ledge_z);
    };

    const double plain              = ledge_perimeters_for(false, "100%", 2.0);
    const double one_wall           = ledge_perimeters_for(true,  "100%", 2.0);
    const double one_wall_no_fill   = ledge_perimeters_for(true,  "0%",   2.0);
    const double one_wall_no_expand = ledge_perimeters_for(true,  "100%", 0.0);

    REQUIRE(plain > 0.);
    CHECK(one_wall < plain);
    // Both fall back to the original generation, which cuts the walls back to the top boundary but not past it.
    CHECK(one_wall_no_fill > one_wall);
    CHECK(one_wall_no_fill < plain);
    CHECK(one_wall_no_expand > one_wall);
    CHECK(one_wall_no_expand < plain);
}

// The bottom counterpart: the first layer is thinned to a single wall only where a bottom shell fills the
// space behind it. With no bottom shell layers the bottom surfaces are retyped as internal, so that wall
// would ring sparse infill on the bed - the option is switched off instead, and the GUI hides it in that
// state so a profile that left it enabled cannot act behind a hidden checkbox.
TEST_CASE("Only one wall on the first layer needs a bottom shell", "[Perimeters]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    CAPTURE(wall_generator);

    auto first_layer_perimeters_for = [wall_generator](bool only_one_wall_first_layer, int bottom_shell_layers) {
        DynamicPrintConfig config = base_config(wall_generator);
        config.set_deserialize_strict({
            { "only_one_wall_first_layer", only_one_wall_first_layer },
            { "bottom_shell_layers",       bottom_shell_layers },
        });
        Print print;
        init_and_process_print({ step_with_ledge() }, print, config);
        REQUIRE_FALSE(print.objects().empty());
        return perimeter_length_at(print, first_layer_z);
    };

    const double plain             = first_layer_perimeters_for(false, 3);
    const double one_wall          = first_layer_perimeters_for(true,  3);
    // Both at zero bottom shell layers, so everything else that setting changes cancels out between them.
    const double plain_no_shell    = first_layer_perimeters_for(false, 0);
    const double one_wall_no_shell = first_layer_perimeters_for(true,  0);

    REQUIRE(plain > 0.);
    CHECK(one_wall < plain);
    // No bottom shell: the option is inert, down to the same walls an unchecked box gives.
    CHECK_THAT(one_wall_no_shell, Catch::Matchers::WithinAbs(plain_no_shell, 1.0));
}


TEST_CASE("Overhang wall overlap changes mixed-surface wall geometry", "[OverhangWallOverlap]")
{
    struct OverhangMeasure {
        size_t count = 0;
        double length = 0.;
        int64_t coordinate_sum = 0;
        int64_t perimeter_coordinate_sum = 0;
        int64_t bridge_coordinate_sum = 0;
    };

    const auto mixed_overhang_step = [] {
        TriangleMesh base = make_cube(20., 20., 1.);
        TriangleMesh top  = make_cube(30., 20., 2.);
        top.translate(-5.f, 0.f, 1.f);
        base.merge(top);
        return base;
    };
    const auto measure = [&mixed_overhang_step](const char *wall_generator, const char *overlap) {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({
            { "wall_generator", wall_generator },
            { "wall_loops", 3 },
            { "layer_height", 0.2 },
            { "initial_layer_print_height", 0.2 },
            { "detect_overhang_wall", "1" },
            { "enable_overhang_speed", "1" },
            { "enable_support", false },
            { "overhang_wall_overlap", overlap },
        });
        Print print;
        init_and_process_print({ mixed_overhang_step() }, print, config);

        OverhangMeasure out;
        const auto account = [&out](const ExtrusionPath &path) {
            for (const Point3 &point : path.polyline.points)
                if (path.role() == erOverhangPerimeter)
                    out.coordinate_sum += point.x() + point.y();
                else if (path.role() == erPerimeter || path.role() == erExternalPerimeter)
                    out.perimeter_coordinate_sum += point.x() + point.y();
                else if (path.role() == erBridgeInfill || path.role() == erInternalBridgeInfill)
                    out.bridge_coordinate_sum += point.x() + point.y();
            if (path.role() == erOverhangPerimeter) {
                ++out.count;
                out.length += path.length();
            }
        };
        for (const Layer *layer : print.objects().front()->layers())
            for (const LayerRegion *region : layer->regions())
                for (const ExtrusionEntity *entity : region->perimeters.flatten().entities)
                    if (const auto *path = dynamic_cast<const ExtrusionPath *>(entity))
                        account(*path);
                    else if (const auto *multi = dynamic_cast<const ExtrusionMultiPath *>(entity))
                        for (const ExtrusionPath &path : multi->paths)
                            account(path);
                    else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(entity))
                        for (const ExtrusionPath &path : loop->paths)
                            account(path);
        return out;
    };

    const OverhangMeasure classic_zero = measure("classic", "0%");
    const OverhangMeasure classic_zero_repeat = measure("classic", "0%");
    const OverhangMeasure classic_full = measure("classic", "100%");

    REQUIRE(classic_zero.count > 0);
    REQUIRE(classic_full.count > 0);
    CHECK(classic_zero.coordinate_sum == classic_zero_repeat.coordinate_sum);
    CHECK(classic_full.coordinate_sum != classic_zero.coordinate_sum);
    CHECK(std::abs(classic_full.perimeter_coordinate_sum - classic_zero.perimeter_coordinate_sum) <= scaled<int64_t>(0.001));
    CHECK(classic_full.bridge_coordinate_sum == classic_zero.bridge_coordinate_sum);


    const OverhangMeasure arachne_zero = measure("arachne", "0%");
    const OverhangMeasure arachne_zero_repeat = measure("arachne", "0%");
    const OverhangMeasure arachne_full = measure("arachne", "100%");

    REQUIRE(arachne_zero.count > 0);
    REQUIRE(arachne_full.count > 0);
    CHECK(arachne_zero.coordinate_sum == arachne_zero_repeat.coordinate_sum);
    CHECK(arachne_full.coordinate_sum != arachne_zero.coordinate_sum);
    CHECK(std::abs(arachne_full.perimeter_coordinate_sum - arachne_zero.perimeter_coordinate_sum) <= scaled<int64_t>(0.001));
    CHECK(arachne_full.bridge_coordinate_sum == arachne_zero.bridge_coordinate_sum);

}

TEST_CASE("External bridge overlap changes only the bridge-to-overhang-wall seam", "[ExternalBridgeOverlap][Regression]")
{
    struct Measurement {
        size_t wall_count = 0;
        size_t perimeter_boundary_count = 0;
        double wall_length = 0.;
        int64_t wall_coordinate_sum = 0;
        double bridge_wall_seam_area = 0.;
    };

    const auto mixed_overhang_bridge = [] {
        TriangleMesh base = make_cube(20., 20., 1.);
        TriangleMesh top = make_cube(30., 20., 2.);
        top.translate(-5.f, 0.f, 1.f);
        base.merge(top);
        return base;
    };
    const auto measure = [&mixed_overhang_bridge](const char *wall_generator, const char *overlap) {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({
            { "wall_generator", wall_generator },
            { "wall_loops", 3 },
            { "detect_overhang_wall", true },
            { "enable_overhang_speed", true },
            { "enable_support", false },
            { "external_bridge_infill_wall_overlap", overlap },
            { "overhang_wall_overlap", "0%" },
        });

        Print print;
        Test::init_and_process_print({ mixed_overhang_bridge() }, print, config);

        Polylines wall_boundaries;
        Measurement result;
        const auto account = [&](const ExtrusionPath &path) {
            if (path.role() != erOverhangPerimeter)
                return;
            ++result.wall_count;
            result.wall_length += path.length();
            for (size_t i = 1; i < path.polyline.points.size(); ++i) {
                const Point a = path.polyline.points[i - 1].to_point();
                const Point b = path.polyline.points[i].to_point();
                wall_boundaries.emplace_back(Polyline{ Points{ a, b } });
            }
            for (const Point3 &point : path.polyline.points)
                result.wall_coordinate_sum += point.x() + point.y();
        };
        const auto collect_walls = [&](const ExtrusionEntityCollection &collection) {
            for (const ExtrusionEntity *entity : collection.flatten().entities)
                if (const auto *path = dynamic_cast<const ExtrusionPath *>(entity))
                    account(*path);
                else if (const auto *multi = dynamic_cast<const ExtrusionMultiPath *>(entity))
                    for (const ExtrusionPath &path : multi->paths)
                        account(path);
                else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(entity))
                    for (const ExtrusionPath &path : loop->paths)
                        account(path);
        };
        for (const Layer *layer : print.objects().front()->layers())
            for (const LayerRegion *region : layer->regions()) {
                result.perimeter_boundary_count += region->external_bridge_wall_boundary.size();
                collect_walls(region->perimeters);
                collect_walls(region->fills);
            }

        if (result.wall_count == 0 || wall_boundaries.empty())
            return result;
        const Polygons seam_region = offset(wall_boundaries, scaled<float>(0.2));
        for (const Layer *layer : print.objects().front()->layers())
            for (const LayerRegion *region : layer->regions())
                for (const Surface &surface : region->fill_surfaces.surfaces)
                    if (surface.surface_type == stBottomBridge)
                        result.bridge_wall_seam_area += area(intersection_ex(ExPolygons{ surface.expolygon }, seam_region));
        return result;
    };

    for (const char *wall_generator : { "classic", "arachne" }) {
        CAPTURE(wall_generator);
        const Measurement no_overlap = measure(wall_generator, "0%");
        const Measurement full_overlap = measure(wall_generator, "100%");
        REQUIRE(no_overlap.wall_count > 0);
        REQUIRE(no_overlap.perimeter_boundary_count > 0);
        REQUIRE(no_overlap.wall_count == full_overlap.wall_count);
        CHECK_THAT(no_overlap.wall_length, Catch::Matchers::WithinAbs(full_overlap.wall_length, scaled<double>(0.001)));
        CHECK(no_overlap.wall_coordinate_sum == full_overlap.wall_coordinate_sum);
        CHECK(full_overlap.bridge_wall_seam_area > no_overlap.bridge_wall_seam_area);
    }
}

namespace {

// The rib spans z=[0,5] and the slab z=[5,6], so this is the slab's first layer - the only one whose
// support comes from the rib rather than from the slab below it.
const double slab_first_layer_z = 5.2;

// Rib widths either side of what the wall generators can print. At a 0.4mm nozzle the classic generator
// builds nothing thinner than nozzle/3 = 0.133mm and Arachne drops anything below min_feature_size, 25%
// of the nozzle = 0.1mm. 0.08mm is under both thresholds, 0.3mm over both.
const double unprintable_rib = 0.08;
const double printable_rib   = 0.3;

// A 4x5mm anchor tower carrying a 20x5mm slab at z=[5,6], with a rib `rib_width` wide running the whole
// length of the slab beneath its y=0 edge; a `rib_width` of 0 leaves the rib out. Nothing else is under
// that edge, so whether the wall along it is an overhang rests entirely on the rib. Overhang detection
// grows the lower slices by half the nozzle diameter before it asks, which carries either rib past the
// 0.21mm from the slab edge to that wall - the unprintable one only fails to reach it once it is filtered
// out for being unprintable.
Print &slab_over_rib(Print &print, Model &model, double rib_width, const DynamicPrintConfig &config)
{
    ModelObject *object = model.add_object();
    object->name = "slab_over_rib.stl";
    object->add_volume(make_cube(4., 5., 6.), ModelVolumeType::MODEL_PART, false);
    if (rib_width > 0.) {
        TriangleMesh rib = make_cube(20., rib_width, 5.);
        rib.translate(4.f, 0.f, 0.f);
        object->add_volume(std::move(rib), ModelVolumeType::MODEL_PART, false);
    }
    TriangleMesh slab = make_cube(20., 5., 1.);
    slab.translate(4.f, 0.f, 5.f);
    object->add_volume(std::move(slab), ModelVolumeType::MODEL_PART, false);
    object->add_instance();
    object->ensure_on_bed();

    print.auto_assign_extruders(object);
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    return print;
}

// Every setting the assertions below depend on, so none of them rests on a default. The wall line widths
// are pinned because the rib widths above are chosen against the distance from the slab edge to its outer
// wall, and min_feature_size because it is one of the two thresholds under test.
DynamicPrintConfig printable_rib_config(const char *wall_generator, bool detect_thin_wall)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "wall_generator",                wall_generator },
        { "layer_height",                  0.2 },  // puts a layer boundary exactly on the top of the rib
        { "initial_layer_print_height",    0.2 },
        { "nozzle_diameter",               "0.4" },
        { "outer_wall_line_width",         0.42 },
        { "inner_wall_line_width",         0.45 },
        { "wall_loops",                    2 },
        { "detect_overhang_wall",          true },
        { "detect_thin_wall",              detect_thin_wall },
        { "min_feature_size",              "25%" },
        { "raft_layers",                   0 },
        // Anything that adds, drops or reorders walls would move length between the roles being counted.
        { "extra_perimeters_on_overhangs", false },
        { "overhang_reverse",              false },
        { "only_one_wall_top",             false },
        { "only_one_wall_first_layer",     false },
        { "unsupported_wall_last",         false },
        { "sparse_infill_density",         "15%" },
    });
    return config;
}

// Length of every overhang perimeter path on the layer at `print_z`, loops and open extrusions alike.
double overhang_length_at(const Print &print, double print_z)
{
    double len = 0.;
    const auto add_entity = [&len](const ExtrusionEntity *entity, auto &&self) -> void {
        const auto add_paths = [&len](const ExtrusionPaths &paths) {
            for (const ExtrusionPath &path : paths)
                if (path.role() == erOverhangPerimeter)
                    len += path.length();
        };
        if (const auto *coll = dynamic_cast<const ExtrusionEntityCollection*>(entity)) {
            for (const ExtrusionEntity *child : coll->entities)
                self(child, self);
        } else if (const auto *loop = dynamic_cast<const ExtrusionLoop*>(entity)) {
            add_paths(loop->paths);
        } else if (const auto *multi = dynamic_cast<const ExtrusionMultiPath*>(entity)) {
            add_paths(multi->paths);
        } else if (const auto *path = dynamic_cast<const ExtrusionPath*>(entity)) {
            if (path->role() == erOverhangPerimeter)
                len += path->length();
        }
    };

    for (const Layer *layer : print.objects().front()->layers()) {
        if (std::abs(layer->print_z - print_z) > EPSILON)
            continue;
        for (const LayerRegion *region : layer->regions())
            add_entity(&region->perimeters, add_entity);
    }
    return len;
}

} // namespace

// A sliver the wall generator prints nothing for holds nothing up, so it cannot be what decides that the
// wall above it is not an overhang. The rib under the slab is the only thing that edge of the slab could
// rest on: below the threshold of the active generator the slab has to come out exactly as it does with
// no rib at all, and the last check is the control - a rib the generator does print anchors that wall,
// without which the first check would hold for want of any sensitivity to the rib.
TEST_CASE("A lower layer sliver too thin to print does not support the wall above it", "[Perimeters]")
{
    const char *wall_generator   = GENERATE("classic", "arachne");
    const bool  detect_thin_wall = GENERATE(true, false);
    CAPTURE(wall_generator, detect_thin_wall);

    auto overhang_for = [wall_generator, detect_thin_wall](double rib_width) {
        Print print;
        Model model;
        slab_over_rib(print, model, rib_width, printable_rib_config(wall_generator, detect_thin_wall));
        print.process();
        REQUIRE_FALSE(print.objects().empty());
        return overhang_length_at(print, slab_first_layer_z);
    };

    const double no_rib      = overhang_for(0.);
    const double unprintable = overhang_for(unprintable_rib);
    const double printable   = overhang_for(printable_rib);

    // Only where the slab meets the tower is it held up from below, so both of its 20mm walls overhang.
    REQUIRE(no_rib > scale_(30.));
    CHECK_THAT(unprintable, Catch::Matchers::WithinAbs(no_rib, scale_(1.)));
    // A rib that does get printed takes the 20mm outer wall running along it out of the overhangs.
    CHECK(printable < no_rib - scale_(15.));
}
