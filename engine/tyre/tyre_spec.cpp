#include "tyre_spec.h"

#include <cmath>
#include <set>

namespace me::tyre
{

namespace
{
constexpr const char* kThermalPrefix = "THERMAL_";

#define ME_TYRE_FIELD(group, key, acKey, thermal, member) \
    TyreSpecField{group, key, acKey, thermal, [](TyreSpec& s) -> std::optional<float>& { return s.member; }}
#define ME_TYRE_CURVE(group, key, acKey, thermal, member) \
    TyreSpecCurve{group, key, acKey, thermal, [](TyreSpec& s) -> std::vector<glm::vec2>& { return s.member; }}

float GripAtLoad(
    const std::optional<float>& reference, const std::optional<float>& referenceLoad, const std::optional<float>& exponent,
    const std::optional<float>& base, const std::optional<float>& slope, float loadNewtons)
{
    const float ref = reference.value_or(0.0f);
    const float fz0 = referenceLoad.value_or(0.0f);
    const float exp = exponent.value_or(0.0f);
    if (ref > 0.0f && fz0 > 0.0f && exp > 0.0f && loadNewtons > 0.0f)
    {
        return ref * std::pow(loadNewtons / fz0, exp - 1.0f);
    }
    return base.value_or(0.0f) + slope.value_or(0.0f);
}
}

const std::vector<TyreSpecField>& TyreSpecFields()
{
    static const std::vector<TyreSpecField> fields{
        ME_TYRE_FIELD("size", "width", "WIDTH", false, size.width),
        ME_TYRE_FIELD("size", "radius", "RADIUS", false, size.radius),
        ME_TYRE_FIELD("size", "rim_radius", "RIM_RADIUS", false, size.rimRadius),
        ME_TYRE_FIELD("size", "angular_inertia", "ANGULAR_INERTIA", false, size.angularInertia),
        ME_TYRE_FIELD("size", "radius_growth_mm", "RADIUS_ANGULAR_K", false, size.radiusGrowthMm),
        ME_TYRE_FIELD("vertical", "rate", "RATE", false, vertical.rate),
        ME_TYRE_FIELD("vertical", "damping", "DAMP", false, vertical.damping),
        ME_TYRE_FIELD("grip", "reference_load", "FZ0", false, grip.referenceLoad),
        ME_TYRE_FIELD("grip", "longitudinal_reference", "DX_REF", false, grip.longitudinalReference),
        ME_TYRE_FIELD("grip", "lateral_reference", "DY_REF", false, grip.lateralReference),
        ME_TYRE_FIELD("grip", "longitudinal_load_exponent", "LS_EXPX", false, grip.longitudinalLoadExponent),
        ME_TYRE_FIELD("grip", "lateral_load_exponent", "LS_EXPY", false, grip.lateralLoadExponent),
        ME_TYRE_FIELD("grip", "dx0", "DX0", false, grip.dx0),
        ME_TYRE_FIELD("grip", "dx1", "DX1", false, grip.dx1),
        ME_TYRE_FIELD("grip", "dy0", "DY0", false, grip.dy0),
        ME_TYRE_FIELD("grip", "dy1", "DY1", false, grip.dy1),
        ME_TYRE_FIELD("grip", "speed_sensitivity", "SPEED_SENSITIVITY", false, grip.speedSensitivity),
        ME_TYRE_FIELD("grip", "brake_longitudinal_mod", "BRAKE_DX_MOD", false, grip.brakeLongitudinalMod),
        ME_TYRE_FIELD("grip", "xmu", "XMU", false, grip.xmu),
        ME_TYRE_FIELD("slip", "friction_limit_angle_degrees", "FRICTION_LIMIT_ANGLE", false, slip.frictionLimitAngleDegrees),
        ME_TYRE_FIELD("slip", "falloff_level", "FALLOFF_LEVEL", false, slip.falloffLevel),
        ME_TYRE_FIELD("slip", "falloff_speed", "FALLOFF_SPEED", false, slip.falloffSpeed),
        ME_TYRE_FIELD("slip", "longitudinal_stiffness_ratio", "CX_MULT", false, slip.longitudinalStiffnessRatio),
        ME_TYRE_FIELD("slip", "combined_factor", "COMBINED_FACTOR", false, slip.combinedFactor),
        ME_TYRE_FIELD("slip", "relaxation_length", "RELAXATION_LENGTH", false, slip.relaxationLength),
        ME_TYRE_FIELD("carcass", "flex", "FLEX", false, carcass.flex),
        ME_TYRE_FIELD("carcass", "flex_gain", "FLEX_GAIN", false, carcass.flexGain),
        ME_TYRE_FIELD("camber", "gain", "CAMBER_GAIN", false, camber.gain),
        ME_TYRE_FIELD("camber", "dcamber0", "DCAMBER_0", false, camber.dcamber0),
        ME_TYRE_FIELD("camber", "dcamber1", "DCAMBER_1", false, camber.dcamber1),
        ME_TYRE_FIELD("rolling", "resistance0", "ROLLING_RESISTANCE_0", false, rolling.resistance0),
        ME_TYRE_FIELD("rolling", "resistance1", "ROLLING_RESISTANCE_1", false, rolling.resistance1),
        ME_TYRE_FIELD("rolling", "resistance_slip", "ROLLING_RESISTANCE_SLIP", false, rolling.resistanceSlip),
        ME_TYRE_FIELD("pressure", "static_psi", "PRESSURE_STATIC", false, pressure.staticPsi),
        ME_TYRE_FIELD("pressure", "ideal_psi", "PRESSURE_IDEAL", false, pressure.idealPsi),
        ME_TYRE_FIELD("pressure", "spring_gain", "PRESSURE_SPRING_GAIN", false, pressure.springGain),
        ME_TYRE_FIELD("pressure", "flex_gain", "PRESSURE_FLEX_GAIN", false, pressure.flexGain),
        ME_TYRE_FIELD("pressure", "rolling_resistance_gain", "PRESSURE_RR_GAIN", false, pressure.rollingResistanceGain),
        ME_TYRE_FIELD("pressure", "footprint_gain", "PRESSURE_D_GAIN", false, pressure.footprintGain),
        ME_TYRE_FIELD("thermal", "surface_transfer", "SURFACE_TRANSFER", true, thermal.surfaceTransfer),
        ME_TYRE_FIELD("thermal", "patch_transfer", "PATCH_TRANSFER", true, thermal.patchTransfer),
        ME_TYRE_FIELD("thermal", "core_transfer", "CORE_TRANSFER", true, thermal.coreTransfer),
        ME_TYRE_FIELD("thermal", "internal_core_transfer", "INTERNAL_CORE_TRANSFER", true, thermal.internalCoreTransfer),
        ME_TYRE_FIELD("thermal", "friction_k", "FRICTION_K", true, thermal.frictionK),
        ME_TYRE_FIELD("thermal", "rolling_k", "ROLLING_K", true, thermal.rollingK),
        ME_TYRE_FIELD("thermal", "surface_rolling_k", "SURFACE_ROLLING_K", true, thermal.surfaceRollingK),
        ME_TYRE_FIELD("thermal", "cool_factor", "COOL_FACTOR", true, thermal.coolFactor),
        ME_TYRE_FIELD("wear", "grain_gain", "GRAIN_GAIN", true, wear.grainGain),
        ME_TYRE_FIELD("wear", "grain_gamma", "GRAIN_GAMMA", true, wear.grainGamma),
        ME_TYRE_FIELD("wear", "blister_gain", "BLISTER_GAIN", true, wear.blisterGain),
        ME_TYRE_FIELD("wear", "blister_gamma", "BLISTER_GAMMA", true, wear.blisterGamma),
    };
    return fields;
}

const std::vector<TyreSpecCurve>& TyreSpecCurves()
{
    static const std::vector<TyreSpecCurve> curves{
        ME_TYRE_CURVE("thermal", "performance_curve", "PERFORMANCE_CURVE", true, thermal.performanceCurve),
        ME_TYRE_CURVE("wear", "wear_curve", "WEAR_CURVE", false, wear.wearCurve),
    };
    return curves;
}

#undef ME_TYRE_FIELD
#undef ME_TYRE_CURVE

TyreSpec TyreSpecFromAc(
    const std::string& name, const std::string& shortName, const std::map<std::string, float>& values,
    const std::map<std::string, std::vector<glm::vec2>>& curves)
{
    TyreSpec spec;
    spec.name = name;
    spec.shortName = shortName;
    const auto keyOf = [](const char* acKey, bool thermal) { return (thermal ? std::string(kThermalPrefix) : std::string()) + acKey; };
    std::set<std::string> known;
    for (const TyreSpecField& field : TyreSpecFields())
    {
        const std::string key = keyOf(field.acKey, field.thermalSection);
        known.insert(key);
        if (const auto found = values.find(key); found != values.end())
        {
            field.field(spec) = found->second;
        }
    }
    for (const TyreSpecCurve& curve : TyreSpecCurves())
    {
        const std::string key = keyOf(curve.acKey, curve.thermalSection);
        known.insert(key);
        if (const auto found = curves.find(key); found != curves.end())
        {
            curve.field(spec) = found->second;
        }
    }
    for (const auto& [key, value] : values)
    {
        if (!known.contains(key))
        {
            spec.extraValues[key] = value;
        }
    }
    for (const auto& [key, points] : curves)
    {
        if (!known.contains(key))
        {
            spec.extraCurves[key] = points;
        }
    }
    return spec;
}

float LongitudinalGripAtLoad(const TyreSpec& spec, float loadNewtons)
{
    return GripAtLoad(
        spec.grip.longitudinalReference, spec.grip.referenceLoad, spec.grip.longitudinalLoadExponent, spec.grip.dx0, spec.grip.dx1,
        loadNewtons);
}

float LateralGripAtLoad(const TyreSpec& spec, float loadNewtons)
{
    return GripAtLoad(
        spec.grip.lateralReference, spec.grip.referenceLoad, spec.grip.lateralLoadExponent, spec.grip.dy0, spec.grip.dy1, loadNewtons);
}
}
