#pragma once

#include <imgui.h>

#include <string>

namespace me
{

// What the driving HUD shows of the driven car, this frame.
struct Gt7HudInput
{
    float speedKmh = 0.0f;
    float rpm = 0.0f;
    // The rev limit: the rev strip lights over the upper half of the range up to it.
    float maxRpm = 7000.0f;
    int gear = 0; // negative reverse, 0 neutral
    bool manualGearbox = false;
    // The pedals as the car takes them (0 to 1) and the steering (-1 full left to 1 full right).
    float throttle = 0.0f;
    float brake = 0.0f;
    float steering = 0.0f;
    bool handBrake = false;
    // The assists: fitted and switched on (lamp on; grey when the car has none or it is switched off) and
    // working this moment (lamp red, and the cut shown red on its pedal's bar).
    bool absFitted = false;
    bool absActive = false;
    bool tcsFitted = false;
    bool tcsActive = false;
    bool counterSteerAssist = false;
    // The boost gauge is drawn for a car with turbos only; bar (x100 kPa) over the air outside.
    bool turbo = false;
    float boostBar = 0.0f;
    float fuelShare = 1.0f;
    double odometerKm = 0.0;
    // The tyres' compound initials ("SS", "SM"), front and rear; empty draws none.
    std::string frontTyre;
    std::string rearTyre;
    // Seconds, for what blinks.
    double time = 0.0;
};

// Draws Gran Turismo 7's driving HUD along the bottom of the rectangle: the surface water bracket,
// the tyres round the car, the fuel gauge and odometer, the ABS / hand brake / driving aid lamps,
// the brake bar, the rev strip over the speed and gear, the throttle bar, the light / counter-steer /
// TCS / warning lamps and the boost gauge. Text, icons and digits are SVG fonts and icons built into
// the editor (engine/editor/ui/gt7_hud, from tools/gt7_hud/make_gt7_hud_svgs.py), drawn as geometry so
// they stay sharp at any size; the bars, arcs and ticks are ImGui's anti-aliased shapes. The layout
// scales with the rectangle's height (or its width, when narrower than 16:9).
//
// Returns false when the built-in SVGs could not be read (nothing is drawn then).
bool DrawGt7Hud(ImDrawList& drawList, ImVec2 origin, ImVec2 size, const Gt7HudInput& input);
}
