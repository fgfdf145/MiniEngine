#pragma once

#include <glm/vec2.hpp>

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace me::tyre
{

// One tyre: a compound in one size, as a car's data defines it, independent of any car. Every physical
// parameter of Assetto Corsa's tyres.ini has a field (in the game's units, named with the unit where it
// is not SI); a field the data leaves out is empty. Keys the table below does not know are kept in
// extraValues / extraCurves under their own names (a thermal section's with "THERMAL_" in front).
// docs/design/2026-10-08-tyre-library-design.md.
struct TyreSpec
{
    std::string name;
    std::string shortName;
    // Where it came from, in words ("Assetto Corsa ks_mazda_rx7_tuned, compound 2 Semislicks, front").
    std::string source;

    struct Size
    {
        std::optional<float> width;          // WIDTH, m
        std::optional<float> radius;         // RADIUS, m
        std::optional<float> rimRadius;      // RIM_RADIUS, m
        std::optional<float> angularInertia; // ANGULAR_INERTIA, kg m^2 (wheel and tyre)
        std::optional<float> radiusGrowthMm; // RADIUS_ANGULAR_K, mm of radius per rad/s of spin

        bool operator==(const Size&) const = default;
    } size;
    struct Vertical
    {
        std::optional<float> rate;    // RATE, N/m
        std::optional<float> damping; // DAMP, N s/m

        bool operator==(const Vertical&) const = default;
    } vertical;
    struct Grip
    {
        std::optional<float> referenceLoad;            // FZ0, N
        std::optional<float> longitudinalReference;    // DX_REF: peak friction at FZ0
        std::optional<float> lateralReference;         // DY_REF
        std::optional<float> longitudinalLoadExponent; // LS_EXPX
        std::optional<float> lateralLoadExponent;      // LS_EXPY
        std::optional<float> dx0;                      // DX0, DX1, DY0, DY1: the older linear friction
        std::optional<float> dx1;
        std::optional<float> dy0;
        std::optional<float> dy1;
        std::optional<float> speedSensitivity;     // SPEED_SENSITIVITY
        std::optional<float> brakeLongitudinalMod; // BRAKE_DX_MOD
        std::optional<float> xmu;                  // XMU

        bool operator==(const Grip&) const = default;
    } grip;
    struct Slip
    {
        std::optional<float> frictionLimitAngleDegrees;  // FRICTION_LIMIT_ANGLE
        std::optional<float> falloffLevel;               // FALLOFF_LEVEL: share of the peak left past it
        std::optional<float> falloffSpeed;               // FALLOFF_SPEED
        std::optional<float> longitudinalStiffnessRatio; // CX_MULT
        std::optional<float> combinedFactor;             // COMBINED_FACTOR
        std::optional<float> relaxationLength;           // RELAXATION_LENGTH, m

        bool operator==(const Slip&) const = default;
    } slip;
    struct Carcass
    {
        std::optional<float> flex;     // FLEX
        std::optional<float> flexGain; // FLEX_GAIN

        bool operator==(const Carcass&) const = default;
    } carcass;
    struct Camber
    {
        std::optional<float> gain;     // CAMBER_GAIN
        std::optional<float> dcamber0; // DCAMBER_0
        std::optional<float> dcamber1; // DCAMBER_1

        bool operator==(const Camber&) const = default;
    } camber;
    struct Rolling
    {
        std::optional<float> resistance0;    // ROLLING_RESISTANCE_0
        std::optional<float> resistance1;    // ROLLING_RESISTANCE_1 (on speed squared)
        std::optional<float> resistanceSlip; // ROLLING_RESISTANCE_SLIP

        bool operator==(const Rolling&) const = default;
    } rolling;
    struct Pressure
    {
        std::optional<float> staticPsi;             // PRESSURE_STATIC (cold)
        std::optional<float> idealPsi;              // PRESSURE_IDEAL
        std::optional<float> springGain;            // PRESSURE_SPRING_GAIN, N/m per psi
        std::optional<float> flexGain;              // PRESSURE_FLEX_GAIN
        std::optional<float> rollingResistanceGain; // PRESSURE_RR_GAIN
        std::optional<float> footprintGain;         // PRESSURE_D_GAIN

        bool operator==(const Pressure&) const = default;
    } pressure;
    struct Thermal
    {
        std::optional<float> surfaceTransfer;      // [THERMAL_*] SURFACE_TRANSFER
        std::optional<float> patchTransfer;        // PATCH_TRANSFER
        std::optional<float> coreTransfer;         // CORE_TRANSFER
        std::optional<float> internalCoreTransfer; // INTERNAL_CORE_TRANSFER
        std::optional<float> frictionK;            // FRICTION_K
        std::optional<float> rollingK;             // ROLLING_K
        std::optional<float> surfaceRollingK;      // SURFACE_ROLLING_K
        std::optional<float> coolFactor;           // COOL_FACTOR
        std::vector<glm::vec2> performanceCurve;   // PERFORMANCE_CURVE: grip by temperature

        bool operator==(const Thermal&) const = default;
    } thermal;
    struct Wear
    {
        std::vector<glm::vec2> wearCurve;  // WEAR_CURVE: grip by virtual km
        std::optional<float> grainGain;    // [THERMAL_*] GRAIN_GAIN
        std::optional<float> grainGamma;   // GRAIN_GAMMA
        std::optional<float> blisterGain;  // BLISTER_GAIN
        std::optional<float> blisterGamma; // BLISTER_GAMMA

        bool operator==(const Wear&) const = default;
    } wear;

    std::map<std::string, float> extraValues;
    std::map<std::string, std::vector<glm::vec2>> extraCurves;

    bool operator==(const TyreSpec&) const = default;
};

// A scalar of the table: its group and key in a .tyre.yaml, the key Assetto Corsa gives it (without the
// "THERMAL_" of a thermal section's), whether it sits in the thermal section, and the field.
struct TyreSpecField
{
    const char* group;
    const char* key;
    const char* acKey;
    bool thermalSection;
    std::function<std::optional<float>&(TyreSpec&)> field;
};
// And a curve.
struct TyreSpecCurve
{
    const char* group;
    const char* key;
    const char* acKey;
    bool thermalSection;
    std::function<std::vector<glm::vec2>&(TyreSpec&)> field;
};
const std::vector<TyreSpecField>& TyreSpecFields();
const std::vector<TyreSpecCurve>& TyreSpecCurves();

// The tyre from one axle of a compound as the kn5 import keeps tyres.ini (VehicleTyreData): every number
// of its section by key, the thermal section's under "THERMAL_", and the curves its .lut files hold.
TyreSpec TyreSpecFromAc(
    const std::string& name, const std::string& shortName, const std::map<std::string, float>& values,
    const std::map<std::string, std::vector<glm::vec2>>& curves);

// The peak friction along and across the wheel at a load (N): the reference friction at FZ0 scaled by the
// load to the sensitivity exponent less one, or DX0 + DX1 (DY0 + DY1) without those. 0 when the tyre
// says nothing.
float LongitudinalGripAtLoad(const TyreSpec& spec, float loadNewtons);
float LateralGripAtLoad(const TyreSpec& spec, float loadNewtons);
}
