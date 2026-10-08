#include "tyre_spec.h"

#include <cmath>
#include <set>

namespace me::tyre
{

namespace
{
constexpr const char* kThermal = "THERMAL_";
constexpr const char* kAdditional = "ADDITIONAL1_";
constexpr const char* kVirtualKm = "VIRTUALKM_";

#define ME_TYRE_FIELD(group, key, acKey, prefix, member) \
    TyreSpecField{group, key, acKey, prefix, [](TyreSpec& s) -> std::optional<float>& { return s.member; }}
#define ME_TYRE_CURVE(group, key, acKey, prefix, member) \
    TyreSpecCurve{group, key, acKey, prefix, [](TyreSpec& s) -> std::vector<glm::vec2>& { return s.member; }}

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
        ME_TYRE_FIELD("size", "width", "WIDTH", "", size.width),
        ME_TYRE_FIELD("size", "radius", "RADIUS", "", size.radius),
        ME_TYRE_FIELD("size", "rim_radius", "RIM_RADIUS", "", size.rimRadius),
        ME_TYRE_FIELD("size", "angular_inertia", "ANGULAR_INERTIA", "", size.angularInertia),
        ME_TYRE_FIELD("size", "radius_growth_mm", "RADIUS_ANGULAR_K", "", size.radiusGrowthMm),
        ME_TYRE_FIELD("vertical", "rate", "RATE", "", vertical.rate),
        ME_TYRE_FIELD("vertical", "damping", "DAMP", "", vertical.damping),
        ME_TYRE_FIELD("grip", "reference_load", "FZ0", "", grip.referenceLoad),
        ME_TYRE_FIELD("grip", "longitudinal_reference", "DX_REF", "", grip.longitudinalReference),
        ME_TYRE_FIELD("grip", "lateral_reference", "DY_REF", "", grip.lateralReference),
        ME_TYRE_FIELD("grip", "longitudinal_load_exponent", "LS_EXPX", "", grip.longitudinalLoadExponent),
        ME_TYRE_FIELD("grip", "lateral_load_exponent", "LS_EXPY", "", grip.lateralLoadExponent),
        ME_TYRE_FIELD("grip", "dx0", "DX0", "", grip.dx0),
        ME_TYRE_FIELD("grip", "dx1", "DX1", "", grip.dx1),
        ME_TYRE_FIELD("grip", "dy0", "DY0", "", grip.dy0),
        ME_TYRE_FIELD("grip", "dy1", "DY1", "", grip.dy1),
        ME_TYRE_FIELD("grip", "speed_sensitivity", "SPEED_SENSITIVITY", "", grip.speedSensitivity),
        ME_TYRE_FIELD("grip", "brake_longitudinal_mod", "BRAKE_DX_MOD", "", grip.brakeLongitudinalMod),
        ME_TYRE_FIELD("grip", "xmu", "XMU", "", grip.xmu),
        ME_TYRE_FIELD("slip", "friction_limit_angle_degrees", "FRICTION_LIMIT_ANGLE", "", slip.frictionLimitAngleDegrees),
        ME_TYRE_FIELD("slip", "falloff_level", "FALLOFF_LEVEL", "", slip.falloffLevel),
        ME_TYRE_FIELD("slip", "falloff_speed", "FALLOFF_SPEED", "", slip.falloffSpeed),
        ME_TYRE_FIELD("slip", "longitudinal_stiffness_ratio", "CX_MULT", "", slip.longitudinalStiffnessRatio),
        ME_TYRE_FIELD("slip", "combined_factor", "COMBINED_FACTOR", "", slip.combinedFactor),
        ME_TYRE_FIELD("slip", "relaxation_length", "RELAXATION_LENGTH", "", slip.relaxationLength),
        ME_TYRE_FIELD("carcass", "flex", "FLEX", "", carcass.flex),
        ME_TYRE_FIELD("carcass", "flex_gain", "FLEX_GAIN", "", carcass.flexGain),
        ME_TYRE_FIELD("camber", "gain", "CAMBER_GAIN", "", camber.gain),
        ME_TYRE_FIELD("camber", "dcamber0", "DCAMBER_0", "", camber.dcamber0),
        ME_TYRE_FIELD("camber", "dcamber1", "DCAMBER_1", "", camber.dcamber1),
        ME_TYRE_FIELD("rolling", "resistance0", "ROLLING_RESISTANCE_0", "", rolling.resistance0),
        ME_TYRE_FIELD("rolling", "resistance1", "ROLLING_RESISTANCE_1", "", rolling.resistance1),
        ME_TYRE_FIELD("rolling", "resistance_slip", "ROLLING_RESISTANCE_SLIP", "", rolling.resistanceSlip),
        ME_TYRE_FIELD("pressure", "static_psi", "PRESSURE_STATIC", "", pressure.staticPsi),
        ME_TYRE_FIELD("pressure", "ideal_psi", "PRESSURE_IDEAL", "", pressure.idealPsi),
        ME_TYRE_FIELD("pressure", "spring_gain", "PRESSURE_SPRING_GAIN", "", pressure.springGain),
        ME_TYRE_FIELD("pressure", "flex_gain", "PRESSURE_FLEX_GAIN", "", pressure.flexGain),
        ME_TYRE_FIELD("pressure", "rolling_resistance_gain", "PRESSURE_RR_GAIN", "", pressure.rollingResistanceGain),
        ME_TYRE_FIELD("pressure", "footprint_gain", "PRESSURE_D_GAIN", "", pressure.footprintGain),
        ME_TYRE_FIELD("pressure", "temperature_gain", "PRESSURE_TEMPERATURE_GAIN", kAdditional, pressure.temperatureGain),
        ME_TYRE_FIELD("thermal", "surface_transfer", "SURFACE_TRANSFER", kThermal, thermal.surfaceTransfer),
        ME_TYRE_FIELD("thermal", "patch_transfer", "PATCH_TRANSFER", kThermal, thermal.patchTransfer),
        ME_TYRE_FIELD("thermal", "core_transfer", "CORE_TRANSFER", kThermal, thermal.coreTransfer),
        ME_TYRE_FIELD("thermal", "internal_core_transfer", "INTERNAL_CORE_TRANSFER", kThermal, thermal.internalCoreTransfer),
        ME_TYRE_FIELD("thermal", "friction_k", "FRICTION_K", kThermal, thermal.frictionK),
        ME_TYRE_FIELD("thermal", "rolling_k", "ROLLING_K", kThermal, thermal.rollingK),
        ME_TYRE_FIELD("thermal", "surface_rolling_k", "SURFACE_ROLLING_K", kThermal, thermal.surfaceRollingK),
        ME_TYRE_FIELD("thermal", "cool_factor", "COOL_FACTOR", kThermal, thermal.coolFactor),
        ME_TYRE_FIELD("thermal", "camber_spread", "CAMBER_TEMP_SPREAD_K", kAdditional, thermal.camberSpread),
        ME_TYRE_FIELD("thermal", "blankets_temperature", "BLANKETS_TEMP", kAdditional, thermal.blanketsTemperature),
        ME_TYRE_FIELD("wear", "grain_gain", "GRAIN_GAIN", kThermal, wear.grainGain),
        ME_TYRE_FIELD("wear", "grain_gamma", "GRAIN_GAMMA", kThermal, wear.grainGamma),
        ME_TYRE_FIELD("wear", "blister_gain", "BLISTER_GAIN", kThermal, wear.blisterGain),
        ME_TYRE_FIELD("wear", "blister_gamma", "BLISTER_GAMMA", kThermal, wear.blisterGamma),
        ME_TYRE_FIELD("wear", "use_load", "USE_LOAD", kVirtualKm, wear.useLoad),
    };
    return fields;
}

const std::vector<TyreSpecCurve>& TyreSpecCurves()
{
    static const std::vector<TyreSpecCurve> curves{
        ME_TYRE_CURVE("thermal", "performance_curve", "PERFORMANCE_CURVE", kThermal, thermal.performanceCurve),
        ME_TYRE_CURVE("wear", "wear_curve", "WEAR_CURVE", "", wear.wearCurve),
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
    const auto keyOf = [](const char* acKey, const char* prefix) { return std::string(prefix) + acKey; };
    std::set<std::string> known;
    for (const TyreSpecField& field : TyreSpecFields())
    {
        const std::string key = keyOf(field.acKey, field.sectionPrefix);
        known.insert(key);
        if (const auto found = values.find(key); found != values.end())
        {
            field.field(spec) = found->second;
        }
    }
    for (const TyreSpecCurve& curve : TyreSpecCurves())
    {
        const std::string key = keyOf(curve.acKey, curve.sectionPrefix);
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
