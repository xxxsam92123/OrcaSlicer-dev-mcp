#include <catch2/catch_all.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "test_utils.hpp"

#include <fstream>
#include <string>
#include <vector>

using namespace Slic3r;

// Bambu firmware uses the " FEATURE: " style reserved tags, everything else the Slic3r-compatible
// "TYPE:" style, so which list applies depends on the printer kind passed in.
TEST_CASE("Reserved keyword detection follows the printer kind it is given", "[GCodeProcessor]")
{
    struct Case
    {
        const char* name;
        std::string gcode;
        bool        reserved_on_bbl;
        bool        reserved_on_non_bbl;
    };

    const auto test_case = GENERATE(values<Case>({
        {"compatible feature tag", ";TYPE:Prime tower", false, true},
        {"compatible layer tag", ";LAYER_CHANGE", false, true},
        {"bbl feature tag", "; FEATURE: Outer wall", true, false},
        {"tag shared by both lists", ";_GP_FIRST_LINE_M73_PLACEHOLDER", true, true},
        {"bbl spells this one with a leading space", ";COLOR_CHANGE", false, true},
        {"ordinary comment", "; heat the bed", false, false},
        {"not a comment at all", "G1 X10 Y10 F3000", false, false},
        // A tag counts only as the whole comment's prefix, so neither a tag mentioned mid-comment
        // nor one trailing a real command is a reserved use.
        {"tag text later in the comment", "; the TYPE:Prime tower marker", false, false},
        {"tag trailing a command", "G1 X10 ;TYPE:Prime tower", false, false},
    }));

    DYNAMIC_SECTION(test_case.name)
    {
        std::vector<std::string> tags;
        REQUIRE(GCodeProcessor::contains_reserved_tags(test_case.gcode, 5, tags, true) == test_case.reserved_on_bbl);

        tags.clear();
        REQUIRE(GCodeProcessor::contains_reserved_tags(test_case.gcode, 5, tags, false) == test_case.reserved_on_non_bbl);
    }
}

TEST_CASE("Temperature preview follows the targeted nozzle across mapped tool changes", "[GCodeProcessor]")
{
    FullPrintConfig config;
    config.gcode_flavor.value = gcfMarlinFirmware;
    config.single_extruder_multi_material.value = true;
    config.filament_diameter.values = {1.75, 1.75};
    config.filament_map.values = {2, 1};
    config.physical_extruder_map.values = {1, 0};
    config.nozzle_diameter.values = {0.4, 0.4};
    config.nozzle_volume.values = {0.0, 0.0};

    ScopedTemporaryFile temp(".gcode");
    {
        std::ofstream output(temp.string());
        output << "M83\n"
                  "M104 S270 T1\n"
                  "T1 H-1\n"
                  "; FEATURE: Outer wall\n"
                  "G1 X10 Y10 Z0.2 F600\n"
                  "G1 X20 Y20 E1 F1200\n"
                  "M104 S140 T0\n"
                  "G1 X30 Y30 E1\n"
                  "M109 S230 T0\n"
                  "G1 X35 Y35 E1\n"
                  "T0 H-1\n"
                  "G1 X40 Y40 E1\n";
    }

    GCodeProcessor processor;
    processor.apply_config(config);
    processor.process_file(temp.string());

    std::vector<float> extrusion_temperatures;
    for (const GCodeProcessorResult::MoveVertex& move : processor.get_result().moves) {
        if (move.type == EMoveType::Extrude &&
            (move.position[Y] == 20.0f || move.position[Y] == 30.0f || move.position[Y] == 35.0f ||
            move.position[Y] == 40.0f))
            extrusion_temperatures.push_back(move.temperature);
    }

    REQUIRE(extrusion_temperatures.size() == 4);
    CHECK_THAT(extrusion_temperatures[0], Catch::Matchers::WithinAbs(270.0f, 0.01f));
    CHECK_THAT(extrusion_temperatures[1], Catch::Matchers::WithinAbs(270.0f, 0.01f));
    CHECK_THAT(extrusion_temperatures[2], Catch::Matchers::WithinAbs(270.0f, 0.01f));
    CHECK_THAT(extrusion_temperatures[3], Catch::Matchers::WithinAbs(230.0f, 0.01f));
}

namespace {

// Temperatures of the extrusion vertices at the given Y positions, in file order (MoveVertex::position
// is in mm).
std::vector<float> extrusion_temperatures_at(const Slic3r::GCodeProcessor& processor, const std::vector<float>& ys)
{
    std::vector<float> temperatures;
    for (const Slic3r::GCodeProcessorResult::MoveVertex& move : processor.get_result().moves) {
        if (move.type != Slic3r::EMoveType::Extrude)
            continue;
        for (float y : ys)
            if (move.position[Slic3r::Y] == y) {
                temperatures.push_back(move.temperature);
                break;
            }
    }
    return temperatures;
}

// Two filaments on one physical hotend: the temperature commands carry no T, and the A parameter is a
// plain "every hotend" flag.
void write_single_hotend_two_filament_gcode(const std::string& path)
{
    std::ofstream output(path);
    output << "M83\n"
              "M104 S240\n"
              "T0 H-1\n"
              "; FEATURE: Outer wall\n"
              "G1 X10 Y10 Z0.2 F600\n"
              "G1 X20 Y20 E1 F1200\n"
              "M104 S230\n"
              "T1 H-1\n"
              "G1 X30 Y30 E1 F1200\n";
}

} // namespace

// The A parameter is a "every hotend" flag only in its valueless form. Other dialects use it as a
// valued parameter (Snapmaker U1 ships `M104 S0 T0 A0`), where it must not swallow the T target and
// must not fan the temperature out to every hotend.
TEST_CASE("Temperature preview does not let a valued A parameter override the T target", "[GCodeProcessor]")
{
    FullPrintConfig config;
    config.gcode_flavor.value = gcfMarlinFirmware;
    config.single_extruder_multi_material.value = true;
    config.filament_diameter.values = {1.75, 1.75};
    config.filament_map.values = {2, 1};
    config.physical_extruder_map.values = {1, 0};
    config.nozzle_diameter.values = {0.4, 0.4};
    config.nozzle_volume.values = {0.0, 0.0};

    ScopedTemporaryFile temp(".gcode");
    {
        std::ofstream output(temp.string());
        output << "M83\n"
                  "M104 S270 T1\n"
                  "T1 H-1\n"
                  "; FEATURE: Outer wall\n"
                  "G1 X10 Y10 Z0.2 F600\n"
                  "G1 X20 Y20 E1 F1200\n"
                  "M104 S0 T0 A0\n"
                  "G1 X30 Y30 E1 F1200\n"
                  "T0 H-1\n"
                  "M104 S240 A\n"
                  "T1 H-1\n"
                  "G1 X40 Y40 E1 F1200\n";
    }

    GCodeProcessor processor;
    processor.apply_config(config);
    processor.process_file(temp.string());

    const std::vector<float> temperatures = extrusion_temperatures_at(processor, {20.0f, 30.0f, 40.0f});
    REQUIRE(temperatures.size() == 3);
    CHECK_THAT(temperatures[0], Catch::Matchers::WithinAbs(270.0f, 0.01f));
    // `M104 S0 T0 A0` addresses hotend 0 only: the active hotend (1) keeps its own temperature.
    CHECK_THAT(temperatures[1], Catch::Matchers::WithinAbs(270.0f, 0.01f));
    // The valueless `M104 S240 A` does address every hotend. It is issued while hotend 0 is the active
    // one, so hotend 1 reading 240 afterwards is only possible if the command really wrote every slot:
    // a predicate that missed the valueless form would leave hotend 1 at its previous 270.
    CHECK_THAT(temperatures[2], Catch::Matchers::WithinAbs(240.0f, 0.01f));
}

// The same resolution on a printer that does not map filaments to physical hotends: the commands carry
// no T, so every filament's path has to show the temperature that was set while it was printing.
TEST_CASE("Temperature preview follows the active hotend without a physical extruder map", "[GCodeProcessor]")
{
    FullPrintConfig config;
    config.gcode_flavor.value = gcfMarlinFirmware;
    config.single_extruder_multi_material.value = true;
    config.filament_diameter.values = {1.75, 1.75};
    config.filament_map.values = {1, 1};
    config.physical_extruder_map.values = {0, 0};
    config.nozzle_diameter.values = {0.4, 0.4};
    config.nozzle_volume.values = {0.0, 0.0};

    ScopedTemporaryFile temp(".gcode");
    write_single_hotend_two_filament_gcode(temp.string());

    GCodeProcessor processor;
    processor.apply_config(config);
    processor.process_file(temp.string());

    const std::vector<float> temperatures = extrusion_temperatures_at(processor, {20.0f, 30.0f});
    REQUIRE(temperatures.size() == 2);
    CHECK_THAT(temperatures[0], Catch::Matchers::WithinAbs(240.0f, 0.01f));
    CHECK_THAT(temperatures[1], Catch::Matchers::WithinAbs(230.0f, 0.01f));
}

TEST_CASE("Reserved keyword detection reports every offending line", "[GCodeProcessor]")
{
    const std::string gcode = ";TYPE:Prime tower\nG1 X10\n;LAYER_CHANGE\n";

    std::vector<std::string> tags;
    REQUIRE(GCodeProcessor::contains_reserved_tags(gcode, 5, tags, false));
    REQUIRE(tags.size() == 2);
    // Reported in the order they appear, which is what makes the max_count cut-off meaningful.
    CHECK(tags[0] == "TYPE:Prime tower");
    CHECK(tags[1] == "LAYER_CHANGE");

    SECTION("the reported count is capped at max_count")
    {
        tags.clear();
        REQUIRE(GCodeProcessor::contains_reserved_tags(gcode, 1, tags, false));
        CHECK(tags.size() == 1);
        CHECK(tags[0] == "TYPE:Prime tower");
    }

    SECTION("a max_count of zero still reports the first tag")
    {
        tags.clear();
        REQUIRE(GCodeProcessor::contains_reserved_tags(gcode, 0, tags, false));
        CHECK(tags.size() == 1);
    }

    SECTION("g-code with nothing reserved in it reports nothing")
    {
        tags.clear();
        CHECK_FALSE(GCodeProcessor::contains_reserved_tags("G28\n; home all axes\n", 5, tags, false));
        CHECK(tags.empty());
    }
}
