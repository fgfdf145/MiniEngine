#include "ac_car_data.h"

#include "kn5_reader.h"

#include <engine/core/log/log.h>

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iterator>
#include <numbers>

namespace me
{

namespace
{
std::string Trim(const std::string& text)
{
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
    {
        return {};
    }
    const size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::string ToUpperAscii(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c)
                   {
                       return static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c);
                   });
    return text;
}

std::string ToLowerAscii(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c)
                   {
                       return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
                   });
    return text;
}

// A number at the start of the text; nullopt when it does not start with one.
std::optional<float> ParseNumber(const std::string& text)
{
    const std::string trimmed = Trim(text);
    if (trimmed.empty())
    {
        return std::nullopt;
    }
    char* end = nullptr;
    const float value = std::strtof(trimmed.c_str(), &end);
    if (end == trimmed.c_str() || !std::isfinite(value))
    {
        return std::nullopt;
    }
    return value;
}

class IniView
{
  public:
    explicit IniView(const AcCarData::Ini* ini) : m_ini(ini)
    {
    }

    std::optional<float> Number(const std::string& section, const std::string& key) const
    {
        const std::optional<std::string> value = Text(section, key);
        return value.has_value() ? ParseNumber(*value) : std::nullopt;
    }

    std::optional<std::string> Text(const std::string& section, const std::string& key) const
    {
        if (m_ini == nullptr)
        {
            return std::nullopt;
        }
        const auto sectionIt = m_ini->find(section);
        if (sectionIt == m_ini->end())
        {
            return std::nullopt;
        }
        const auto keyIt = sectionIt->second.find(key);
        return keyIt == sectionIt->second.end() ? std::nullopt : std::optional<std::string>(keyIt->second);
    }

    bool HasSection(const std::string& section) const
    {
        return m_ini != nullptr && m_ini->count(section) != 0;
    }

  private:
    const AcCarData::Ini* m_ini;
};

const std::string* FindFile(const AcdArchive::Files& files, const std::string& name)
{
    const auto found = files.find(name);
    return found == files.end() ? nullptr : &found->second;
}

AcCarData::Ini ParseFile(const AcdArchive::Files& files, const std::string& name)
{
    const std::string* text = FindFile(files, name);
    return text != nullptr ? AcCarData::ParseIni(*text) : AcCarData::Ini{};
}

float LutValue(const std::vector<std::pair<float, float>>& lut, float rpm)
{
    if (lut.empty())
    {
        return 0.0f;
    }
    if (rpm <= lut.front().first)
    {
        return lut.front().second;
    }
    for (size_t index = 1; index < lut.size(); ++index)
    {
        if (rpm <= lut[index].first)
        {
            const float span = lut[index].first - lut[index - 1].first;
            const float t = span > 0.0f ? (rpm - lut[index - 1].first) / span : 1.0f;
            return lut[index - 1].second + (lut[index].second - lut[index - 1].second) * t;
        }
    }
    return lut.back().second;
}

// "1.5, -0.3,2" as numbers; nullopt unless there are exactly `count`.
std::optional<std::vector<float>> ParseNumberList(const std::string& text, size_t count)
{
    std::vector<float> numbers;
    size_t position = 0;
    while (position <= text.size())
    {
        size_t comma = text.find(',', position);
        if (comma == std::string::npos)
        {
            comma = text.size();
        }
        const std::optional<float> number = ParseNumber(text.substr(position, comma - position));
        if (!number.has_value())
        {
            return std::nullopt;
        }
        numbers.push_back(*number);
        position = comma + 1;
    }
    return numbers.size() == count ? std::optional<std::vector<float>>(numbers) : std::nullopt;
}

// A curve named by a LUT key: a lut file of the archive, or written inline as "(|x=y|x=y|)".
std::vector<glm::vec2> LutPoints(const AcdArchive::Files& files, const std::string& fileName)
{
    std::vector<glm::vec2> points;
    const std::string name = Trim(fileName);
    if (!name.empty() && name.front() == '(')
    {
        size_t position = 1;
        while (position < name.size())
        {
            const size_t end = std::min(name.find('|', position), name.find(')', position));
            const std::string pair = name.substr(position, end == std::string::npos ? std::string::npos : end - position);
            if (const size_t equals = pair.find('='); equals != std::string::npos)
            {
                const std::optional<float> x = ParseNumber(pair.substr(0, equals));
                const std::optional<float> y = ParseNumber(pair.substr(equals + 1));
                if (x.has_value() && y.has_value())
                {
                    points.emplace_back(*x, *y);
                }
            }
            if (end == std::string::npos)
            {
                break;
            }
            position = end + 1;
        }
        return points;
    }
    if (const std::string* text = FindFile(files, ToLowerAscii(name)))
    {
        for (const auto& [x, y] : AcCarData::ParseLut(*text))
        {
            points.emplace_back(x, y);
        }
    }
    return points;
}

// The [CONTROLLER_n] sections of a controllers file (ctrl_4ws.ini, ctrl_awd2.ini, ctrl_ers_0.ini, ...), in
// order, each with its curve; none when the file is missing.
std::vector<VehicleController> ReadControllers(const AcdArchive::Files& files, const std::string& fileName)
{
    std::vector<VehicleController> controllers;
    if (FindFile(files, fileName) == nullptr)
    {
        return controllers;
    }
    const AcCarData::Ini ini = ParseFile(files, fileName);
    const IniView view(&ini);
    for (int index = 0; index < 32; ++index)
    {
        const std::string section = "CONTROLLER_" + std::to_string(index);
        if (!view.HasSection(section))
        {
            break;
        }
        VehicleController controller;
        controller.input = Trim(view.Text(section, "INPUT").value_or(""));
        controller.combinator = Trim(view.Text(section, "COMBINATOR").value_or(""));
        controller.curve = LutPoints(files, view.Text(section, "LUT").value_or(""));
        controller.filter = view.Number(section, "FILTER").value_or(0.0f);
        controller.upLimit = view.Number(section, "UP_LIMIT").value_or(0.0f);
        controller.downLimit = view.Number(section, "DOWN_LIMIT").value_or(0.0f);
        controllers.push_back(std::move(controller));
    }
    return controllers;
}

// The weight on the front axle, as a fraction of the car's.
float FrontWeightShare(const AcdArchive::Files& files)
{
    const AcCarData::Ini ini = ParseFile(files, "suspensions.ini");
    return std::clamp(IniView(&ini).Number("BASIC", "CG_LOCATION").value_or(0.5f), 0.2f, 0.8f);
}

void ReadEngine(const AcdArchive::Files& files, VehicleCarSpec& spec)
{
    const AcCarData::Ini engineIni = ParseFile(files, "engine.ini");
    const IniView engine(&engineIni);
    if (const std::optional<float> inertia = engine.Number("ENGINE_DATA", "INERTIA"); inertia.has_value() && *inertia > 0.0f)
    {
        spec.engineInertia = *inertia;
    }
    if (const std::optional<float> rpm = engine.Number("COAST_REF", "RPM"); rpm.has_value() && *rpm > 0.0f)
    {
        spec.coastRpm = *rpm;
        spec.coastTorque = engine.Number("COAST_REF", "TORQUE");
    }
    for (const auto& [section, keys] : engineIni)
    {
        if (section.rfind("TURBO_", 0) != 0)
        {
            continue;
        }
        VehicleTurbo turbo;
        turbo.maxBoost = engine.Number(section, "MAX_BOOST").value_or(0.0f);
        turbo.wastegate = engine.Number(section, "WASTEGATE").value_or(0.0f);
        turbo.referenceRpm = engine.Number(section, "REFERENCE_RPM").value_or(0.0f);
        turbo.gamma = engine.Number(section, "GAMMA").value_or(1.0f);
        turbo.lagUp = engine.Number(section, "LAG_UP").value_or(0.0f);
        turbo.lagDown = engine.Number(section, "LAG_DN").value_or(0.0f);
        spec.turbos.push_back(turbo);
    }

    const std::string* lutText = FindFile(files, ToLowerAscii(engine.Text("HEADER", "POWER_CURVE").value_or("power.lut")));
    if (lutText == nullptr)
    {
        return;
    }
    const std::vector<std::pair<float, float>> lut = AcCarData::ParseLut(*lutText);
    if (lut.size() < 2)
    {
        return;
    }

    float lastRpm = 0.0f;
    for (const auto& [rpm, torque] : lut)
    {
        float boost = 0.0f;
        for (const VehicleTurbo& turbo : spec.turbos)
        {
            boost += AcCarData::TurboBoost(rpm, turbo.maxBoost, turbo.wastegate, turbo.referenceRpm, turbo.gamma);
        }
        spec.torqueCurve.emplace_back(rpm, std::max(torque, 0.0f) * (1.0f + boost));
        lastRpm = std::max(lastRpm, rpm);
    }

    const float limiter = engine.Number("ENGINE_DATA", "LIMITER").value_or(0.0f);
    spec.maxRpm = limiter > 0.0f ? limiter : lastRpm;
    const float minimum = engine.Number("ENGINE_DATA", "MINIMUM").value_or(0.0f);
    if (minimum > 0.0f)
    {
        spec.minRpm = minimum;
    }
}

void ReadDrivetrain(const AcdArchive::Files& files, VehicleCarSpec& spec)
{
    const AcCarData::Ini ini = ParseFile(files, "drivetrain.ini");
    const IniView drivetrain(&ini);
    if (const std::optional<std::string> type = drivetrain.Text("TRACTION", "TYPE"))
    {
        const std::string upper = ToUpperAscii(Trim(*type));
        if (upper == "RWD")
        {
            spec.drive = VehicleDrive::RearWheel;
        }
        else if (upper == "FWD")
        {
            spec.drive = VehicleDrive::FrontWheel;
        }
        else if (upper.rfind("AWD", 0) == 0)
        {
            spec.drive = VehicleDrive::AllWheel;
            // AWD has a centre differential ([AWD]), AWD2 a coupling to the front ([AWD2]); a car may carry
            // both sections, its TYPE says which it drives on.
            const bool coupling = upper == "AWD2";
            const std::string section = coupling ? "AWD2" : "AWD";
            if (drivetrain.HasSection(section))
            {
                VehicleAllWheelDrive awd;
                awd.coupling = coupling;
                awd.frontShare = drivetrain.Number(section, "FRONT_SHARE").value_or(0.5f);
                awd.frontDiffPower = drivetrain.Number(section, "FRONT_DIFF_POWER").value_or(0.0f);
                awd.frontDiffCoast = drivetrain.Number(section, "FRONT_DIFF_COAST").value_or(0.0f);
                awd.frontDiffPreload = drivetrain.Number(section, "FRONT_DIFF_PRELOAD").value_or(0.0f);
                awd.centreDiffPower = drivetrain.Number(section, "CENTRE_DIFF_POWER").value_or(0.0f);
                awd.centreDiffCoast = drivetrain.Number(section, "CENTRE_DIFF_COAST").value_or(0.0f);
                awd.centreDiffPreload = drivetrain.Number(section, "CENTRE_DIFF_PRELOAD").value_or(0.0f);
                awd.rearDiffPower = drivetrain.Number(section, "REAR_DIFF_POWER").value_or(0.0f);
                awd.rearDiffCoast = drivetrain.Number(section, "REAR_DIFF_COAST").value_or(0.0f);
                awd.rearDiffPreload = drivetrain.Number(section, "REAR_DIFF_PRELOAD").value_or(0.0f);
                awd.centreRampTorque = drivetrain.Number(section, "CENTRE_RAMP_TORQUE").value_or(0.0f);
                awd.centreMaxTorque = drivetrain.Number(section, "CENTRE_MAX_TORQUE").value_or(0.0f);
                awd.centreControllers = ReadControllers(files, coupling ? "ctrl_awd2.ini" : "ctrl_awd_center_lock.ini");
                spec.allWheelDrive = std::move(awd);
            }
        }
    }

    // GEAR_1 up to COUNT, or as far as they go when COUNT is missing.
    const int declared = static_cast<int>(drivetrain.Number("GEARS", "COUNT").value_or(0.0f));
    for (int gear = 1; gear <= 12 && (declared <= 0 || gear <= declared); ++gear)
    {
        const std::optional<float> ratio = drivetrain.Number("GEARS", "GEAR_" + std::to_string(gear));
        if (!ratio.has_value() || *ratio <= 0.0f)
        {
            break;
        }
        spec.gearRatios.push_back(*ratio);
    }
    if (const std::optional<float> reverse = drivetrain.Number("GEARS", "GEAR_R"); reverse.has_value() && *reverse != 0.0f)
    {
        spec.reverseGearRatio = -std::abs(*reverse);
    }
    if (const std::optional<float> final = drivetrain.Number("GEARS", "FINAL"); final.has_value() && *final > 0.0f)
    {
        spec.finalDriveRatio = *final;
    }
    if (drivetrain.HasSection("DIFFERENTIAL"))
    {
        // An open differential locks nothing either way.
        const float power = drivetrain.Number("DIFFERENTIAL", "POWER").value_or(0.0f);
        const float coast = drivetrain.Number("DIFFERENTIAL", "COAST").value_or(0.0f);
        spec.limitedSlipDifferentials = power > 0.0f || coast > 0.0f;
        spec.differentialPower = power;
        spec.differentialCoast = coast;
        spec.differentialPreload = drivetrain.Number("DIFFERENTIAL", "PRELOAD");
    }

    // The gearbox and clutch. Times are milliseconds in the file.
    const auto seconds = [&](const char* section, const char* key) -> std::optional<float>
    {
        const std::optional<float> milliseconds = drivetrain.Number(section, key);
        return milliseconds.has_value() && *milliseconds >= 0.0f ? std::optional<float>(*milliseconds / 1000.0f) : std::nullopt;
    };
    spec.changeUpSeconds = seconds("GEARBOX", "CHANGE_UP_TIME");
    spec.changeDownSeconds = seconds("GEARBOX", "CHANGE_DN_TIME");
    spec.autoCutoffSeconds = seconds("GEARBOX", "AUTO_CUTOFF_TIME");
    spec.clutchMaxTorque = drivetrain.Number("CLUTCH", "MAX_TORQUE");
    spec.autoClutchMinRpm = drivetrain.Number("AUTOCLUTCH", "MIN_RPM");
    spec.autoClutchMaxRpm = drivetrain.Number("AUTOCLUTCH", "MAX_RPM");
    // A profile is a section of POINT_n times, or NONE.
    const auto profile = [&](const char* key) -> std::vector<float>
    {
        std::vector<float> points;
        const std::string name = ToUpperAscii(Trim(drivetrain.Text("AUTOCLUTCH", key).value_or("NONE")));
        for (int point = 0; point < 32; ++point)
        {
            const std::optional<float> time = drivetrain.Number(name, "POINT_" + std::to_string(point));
            if (!time.has_value())
            {
                break;
            }
            points.push_back(*time / 1000.0f);
        }
        return points;
    };
    spec.upshiftClutchProfile = profile("UPSHIFT_PROFILE");
    spec.downshiftClutchProfile = profile("DOWNSHIFT_PROFILE");

    // What the physics engine takes of it: a change is over in the time the file gives, and the clutch
    // then takes as long as the upshift's profile (or a tenth of a second without one) to bite.
    if (spec.changeUpSeconds.has_value() && *spec.changeUpSeconds > 0.0f)
    {
        spec.gearSwitchSeconds = spec.changeUpSeconds;
    }
    spec.clutchReleaseSeconds = spec.upshiftClutchProfile.empty() ? 0.1f : std::max(spec.upshiftClutchProfile.back(), 0.05f);
}

void ReadBrakes(const AcdArchive::Files& files, VehicleCarSpec& spec)
{
    const AcCarData::Ini ini = ParseFile(files, "brakes.ini");
    const IniView brakes(&ini);
    if (const std::optional<float> total = brakes.Number("DATA", "MAX_TORQUE"); total.has_value() && *total > 0.0f)
    {
        // The game's figure is for the whole car.
        spec.brakeTorquePerWheel = *total / 4.0f;
    }
    if (const std::optional<float> share = brakes.Number("DATA", "FRONT_SHARE"); share.has_value() && *share > 0.0f && *share < 1.0f)
    {
        spec.frontBrakeShare = *share;
    }
    if (const std::optional<float> hand = brakes.Number("DATA", "HANDBRAKE_TORQUE"); hand.has_value() && *hand >= 0.0f)
    {
        spec.handBrakeTorquePerWheel = *hand / 2.0f;
    }
}

// One axle's linkage and rates. Assetto Corsa gives hardpoints from the wheel centre with x towards
// the car's centre, y up and z forward; the spec wants (forward, outward, up).
std::optional<VehicleSuspensionAxle> ReadSuspensionAxle(const IniView& suspension, const std::string& axle, float antiRollBar)
{
    const std::string type = ToUpperAscii(Trim(suspension.Text(axle, "TYPE").value_or("")));
    VehicleSuspensionAxle out;
    if (type == "DWB")
    {
        out.type = VehicleSuspensionType::DoubleWishbone;
    }
    else if (type == "STRUT")
    {
        out.type = VehicleSuspensionType::MacPherson;
    }
    else if (type == "AXLE")
    {
        out.type = VehicleSuspensionType::SolidAxle;
    }
    else
    {
        return std::nullopt; // ML and others are not modelled
    }
    const auto point = [&](const char* key, glm::vec3& target) {
        const std::optional<std::string> text = suspension.Text(axle, key);
        const std::optional<std::vector<float>> xyz = text.has_value() ? ParseNumberList(*text, 3) : std::nullopt;
        if (!xyz.has_value())
        {
            return false;
        }
        target = glm::vec3((*xyz)[2], -(*xyz)[0], (*xyz)[1]);
        return true;
    };
    bool complete = true;
    if (out.type == VehicleSuspensionType::SolidAxle)
    {
        // The [AXLE] section: LINK_COUNT links from Jn_CAR to Jn_AXLE, from the axle's centre with x
        // to the car's left, y up and z forward.
        const auto link = [&](const std::string& key, glm::vec3& target) {
            const std::optional<std::string> text = suspension.Text("AXLE", key);
            const std::optional<std::vector<float>> xyz = text.has_value() ? ParseNumberList(*text, 3) : std::nullopt;
            if (!xyz.has_value())
            {
                return false;
            }
            target = glm::vec3((*xyz)[2], (*xyz)[0], (*xyz)[1]);
            return true;
        };
        const int count = static_cast<int>(suspension.Number("AXLE", "LINK_COUNT").value_or(0.0f));
        for (int i = 0; i < count; ++i)
        {
            VehicleAxleLink l;
            const std::string name = "J" + std::to_string(i);
            complete = complete && link(name + "_CAR", l.chassis) && link(name + "_AXLE", l.axle);
            out.axleLinks.push_back(l);
        }
        complete = complete && count >= 3;
        out.axleSpringPosition = suspension.Number("AXLE", "ATTACH_REL_POS").value_or(1.0f);
        out.axleLateralStiffness = suspension.Number("AXLE", "LEAF_SPRING_LAT_K").value_or(0.0f);
        out.axleTorqueReaction = suspension.Number("AXLE", "TORQUE_REACTION").value_or(0.0f);
    }
    else
    {
        complete = point("WBCAR_BOTTOM_FRONT", out.lowerFront) && point("WBCAR_BOTTOM_REAR", out.lowerRear) && point("WBTYRE_BOTTOM", out.lowerBall) && point("WBCAR_STEER", out.tieInner) && point("WBTYRE_STEER", out.tieOuter);
    }
    if (out.type == VehicleSuspensionType::DoubleWishbone)
    {
        complete = complete && point("WBCAR_TOP_FRONT", out.upperFront) && point("WBCAR_TOP_REAR", out.upperRear) && point("WBTYRE_TOP", out.upperBall);
    }
    else if (out.type == VehicleSuspensionType::MacPherson)
    {
        complete = complete && point("STRUT_CAR", out.strutTop) && point("STRUT_TYRE", out.strutLower);
    }
    if (!complete)
    {
        return std::nullopt;
    }
    const auto number = [&](const char* key) {
        return suspension.Number(axle, key).value_or(0.0f);
    };
    out.staticCamberDegrees = number("STATIC_CAMBER");
    out.toeOutRodLength = number("TOE_OUT");
    out.track = number("TRACK");
    out.wheelRate = number("SPRING_RATE");
    out.progressiveRate = number("PROGRESSIVE_SPRING_RATE");
    out.bumpStopRate = number("BUMP_STOP_RATE");
    out.bumpStopTravel = number("BUMPSTOP_UP");
    out.reboundStopTravel = number("BUMPSTOP_DN");
    out.dampBump = number("DAMP_BUMP");
    out.dampFastBump = number("DAMP_FAST_BUMP");
    out.dampFastBumpThreshold = number("DAMP_FAST_BUMPTHRESHOLD");
    out.dampRebound = number("DAMP_REBOUND");
    out.dampFastRebound = number("DAMP_FAST_REBOUND");
    out.dampFastReboundThreshold = number("DAMP_FAST_REBOUNDTHRESHOLD");
    out.hubMass = number("HUB_MASS");
    // BASEY is the wheel centre's height against the centre of mass (negative: the wheel below it).
    // The comment in the game's files ("wheel radius + BASEY = CoG") has the sign the other way, but
    // the data settles it: the SUVs have the most negative BASEY (Cayenne -0.28) and the F1 and LMP cars
    // positive ones (F138 +0.14), and the AE86's -0.25 would put its centre of mass 4 cm off the road.
    out.centerOfMassAboveWheel = -number("BASEY");
    out.antiRollBarRate = antiRollBar;
    return out;
}

// The default compound's vertical tyre: RADIUS, RATE and DAMP of tyres.ini's [FRONT] or [REAR].
void ReadVerticalTyre(const AcdArchive::Files& files, const std::string& axle, VehicleSuspensionAxle& out)
{
    const AcCarData::Ini ini = ParseFile(files, "tyres.ini");
    const IniView tyres(&ini);
    out.tyreRadius = tyres.Number(axle, "RADIUS").value_or(0.0f);
    out.tyreRate = tyres.Number(axle, "RATE").value_or(0.0f);
    out.tyreDamping = tyres.Number(axle, "DAMP").value_or(0.0f);
}

void ReadSuspension(const AcdArchive::Files& files, VehicleCarSpec& spec)
{
    if (!spec.massKg.has_value())
    {
        return;
    }
    const AcCarData::Ini ini = ParseFile(files, "suspensions.ini");
    const IniView suspension(&ini);
    if (suspension.HasSection("ARB"))
    {
        const float front = suspension.Number("ARB", "FRONT").value_or(0.0f);
        const float rear = suspension.Number("ARB", "REAR").value_or(0.0f);
        spec.antiRollBars = front > 0.0f || rear > 0.0f;
    }
    spec.frontSuspension = ReadSuspensionAxle(suspension, "FRONT", suspension.Number("ARB", "FRONT").value_or(0.0f));
    spec.rearSuspension = ReadSuspensionAxle(suspension, "REAR", suspension.Number("ARB", "REAR").value_or(0.0f));
    if (spec.frontSuspension.has_value())
    {
        ReadVerticalTyre(files, "FRONT", *spec.frontSuspension);
    }
    if (spec.rearSuspension.has_value())
    {
        ReadVerticalTyre(files, "REAR", *spec.rearSuspension);
    }
    if (const std::optional<float> wheelbase = suspension.Number("BASIC", "WHEELBASE"); wheelbase.has_value() && *wheelbase > 0.0f)
    {
        spec.wheelbase = *wheelbase;
    }
    if (suspension.Number("BASIC", "CG_LOCATION").has_value())
    {
        spec.frontWeightShare = FrontWeightShare(files);
    }
    const AcCarData::Ini carIni = ParseFile(files, "car.ini");
    if (const std::optional<std::string> box = IniView(&carIni).Text("BASIC", "INERTIA"))
    {
        if (const std::optional<std::vector<float>> xyz = ParseNumberList(*box, 3))
        {
            spec.inertiaBox = glm::vec3((*xyz)[0], (*xyz)[1], (*xyz)[2]);
        }
    }

    // Each axle's spring as a natural frequency of the sprung mass on one of its wheels, and its
    // dampers as a fraction of critical damping. The motion ratio of the linkage is not known.
    const float frontWeight = FrontWeightShare(files);
    float frequencySum = 0.0f;
    float dampingSum = 0.0f;
    int axles = 0;
    for (const char* axle : {"FRONT", "REAR"})
    {
        const std::optional<float> rate = suspension.Number(axle, "SPRING_RATE");
        if (!rate.has_value() || *rate <= 0.0f)
        {
            continue;
        }
        const float share = axle[0] == 'F' ? frontWeight : 1.0f - frontWeight;
        const float axleMass = *spec.massKg * share * 0.5f;
        const float unsprung = suspension.Number(axle, "HUB_MASS").value_or(0.0f);
        const float sprung = std::max(axleMass - unsprung, axleMass * 0.5f);
        frequencySum += std::sqrt(*rate / sprung) / (2.0f * std::numbers::pi_v<float>);
        const float bump = suspension.Number(axle, "DAMP_BUMP").value_or(0.0f);
        const float rebound = suspension.Number(axle, "DAMP_REBOUND").value_or(bump);
        dampingSum += (bump + rebound) * 0.5f / (2.0f * std::sqrt(*rate * sprung));
        ++axles;
    }
    if (axles > 0)
    {
        spec.suspensionFrequencyHz = frequencySum / static_cast<float>(axles);
        spec.suspensionDamping = std::clamp(dampingSum / static_cast<float>(axles), 0.05f, 1.2f);
    }
}

// Every number of a section by key, and the curves its lut files hold; `prefix` goes before each key.
void ReadSection(
    const AcdArchive::Files& files, const AcCarData::Ini& ini, const std::string& section, const std::string& prefix, VehicleTyreData& out)
{
    const auto found = ini.find(section);
    if (found == ini.end())
    {
        return;
    }
    for (const auto& [key, text] : found->second)
    {
        if (const std::optional<float> number = ParseNumber(text))
        {
            out.values[prefix + key] = *number;
        }
        else if (ToLowerAscii(text).size() > 4 && ToLowerAscii(text).compare(ToLowerAscii(text).size() - 4, 4, ".lut") == 0)
        {
            out.curves[prefix + key] = LutPoints(files, text);
        }
    }
}

// The friction coefficient of a tyre at a wheel load along one direction: the reference friction at the
// reference load scaled by the load raised to the sensitivity exponent less one, or the file's plain
// coefficients without those.
float GripAtLoad(const VehicleTyreData& tyre, const char* reference, const char* exponent, const char* base, const char* slope, float loadNewtons)
{
    const auto value = [&](const char* key) -> float
    {
        const auto found = tyre.values.find(key);
        return found == tyre.values.end() ? 0.0f : found->second;
    };
    const float referenceLoad = value("FZ0");
    if (value(reference) > 0.0f && referenceLoad > 0.0f && value(exponent) > 0.0f && loadNewtons > 0.0f)
    {
        return value(reference) * std::pow(loadNewtons / referenceLoad, value(exponent) - 1.0f);
    }
    return value(base) + value(slope);
}

// The physics engine's tyre from a compound's axle at the load one wheel carries at rest.
VehicleTyreSettings TyreSettingsFor(const VehicleTyreData& tyre, float staticLoadNewtons)
{
    const auto value = [&](const char* key) -> float
    {
        const auto found = tyre.values.find(key);
        return found == tyre.values.end() ? 0.0f : found->second;
    };
    VehicleTyreSettings settings;
    settings.longitudinalGrip = GripAtLoad(tyre, "DX_REF", "LS_EXPX", "DX0", "DX1", staticLoadNewtons);
    settings.lateralGrip = GripAtLoad(tyre, "DY_REF", "LS_EXPY", "DY0", "DY1", staticLoadNewtons);
    const float limitAngle = value("FRICTION_LIMIT_ANGLE");
    if (limitAngle > 0.0f)
    {
        settings.peakSlipAngleDegrees = limitAngle;
        // A brush tyre reaches its longitudinal peak at about the slip the lateral one does.
        settings.peakSlipRatio = std::tan(limitAngle * std::numbers::pi_v<float> / 180.0f);
    }
    if (const float falloff = value("FALLOFF_LEVEL"); falloff > 0.0f && falloff <= 1.0f)
    {
        settings.postPeakShare = falloff;
    }
    settings.inertia = value("ANGULAR_INERTIA");
    return settings;
}

void ReadTyres(const AcdArchive::Files& files, VehicleCarSpec& spec)
{
    const AcCarData::Ini ini = ParseFile(files, "tyres.ini");
    if (ini.empty())
    {
        return;
    }
    const IniView tyres(&ini);
    // Compound 0's sections carry no number: [FRONT], [THERMAL_FRONT]; the others [FRONT_1].
    for (int index = 0; index < 16; ++index)
    {
        const std::string suffix = index == 0 ? "" : "_" + std::to_string(index);
        if (!tyres.HasSection("FRONT" + suffix) && !tyres.HasSection("REAR" + suffix))
        {
            break;
        }
        VehicleTyreCompound compound;
        for (const bool front : {true, false})
        {
            VehicleTyreData& data = front ? compound.front : compound.rear;
            const std::string axle = front ? "FRONT" : "REAR";
            data.name = Trim(tyres.Text(axle + suffix, "NAME").value_or(""));
            data.shortName = Trim(tyres.Text(axle + suffix, "SHORT_NAME").value_or(""));
            ReadSection(files, ini, axle + suffix, "", data);
            ReadSection(files, ini, "THERMAL_" + axle + suffix, "THERMAL_", data);
        }
        spec.tyreCompounds.push_back(std::move(compound));
    }
    if (spec.tyreCompounds.empty())
    {
        return;
    }
    const int defaultIndex = static_cast<int>(tyres.Number("COMPOUND_DEFAULT", "INDEX").value_or(0.0f));
    spec.defaultTyreCompound = std::clamp(defaultIndex, 0, static_cast<int>(spec.tyreCompounds.size()) - 1);

    if (spec.massKg.has_value())
    {
        // The load one wheel carries standing still.
        constexpr float kGravity = 9.81f;
        const float frontWeight = FrontWeightShare(files);
        const VehicleTyreCompound& compound = spec.tyreCompounds[static_cast<size_t>(*spec.defaultTyreCompound)];
        spec.frontTyres = TyreSettingsFor(compound.front, *spec.massKg * kGravity * frontWeight * 0.5f);
        spec.rearTyres = TyreSettingsFor(compound.rear, *spec.massKg * kGravity * (1.0f - frontWeight) * 0.5f);
    }
}

void ReadAero(const AcdArchive::Files& files, VehicleCarSpec& spec)
{
    const AcCarData::Ini ini = ParseFile(files, "aero.ini");
    const IniView aero(&ini);
    // WING_0 up to the last that follows; the controllers name a wing by this index.
    for (int index = 0; index < 32; ++index)
    {
        const std::string section = "WING_" + std::to_string(index);
        if (!aero.HasSection(section))
        {
            break;
        }
        VehicleAeroWing wing;
        wing.name = Trim(aero.Text(section, "NAME").value_or(section));
        wing.chord = aero.Number(section, "CHORD").value_or(1.0f);
        wing.span = aero.Number(section, "SPAN").value_or(1.0f);
        if (const std::optional<std::string> position = aero.Text(section, "POSITION"))
        {
            if (const std::optional<std::vector<float>> xyz = ParseNumberList(*position, 3))
            {
                wing.position = glm::vec3((*xyz)[0], (*xyz)[1], (*xyz)[2]);
            }
        }
        wing.angleDegrees = aero.Number(section, "ANGLE").value_or(0.0f);
        wing.liftGain = aero.Number(section, "CL_GAIN").value_or(1.0f);
        wing.dragGain = aero.Number(section, "CD_GAIN").value_or(1.0f);
        for (const auto& [key, text] : ini.at(section))
        {
            if (key == "LUT_AOA_CL")
            {
                wing.liftCurve = LutPoints(files, text);
            }
            else if (key == "LUT_AOA_CD")
            {
                wing.dragCurve = LutPoints(files, text);
            }
            else if (key.rfind("LUT_", 0) == 0)
            {
                if (!Trim(text).empty())
                {
                    wing.curves[key] = LutPoints(files, text);
                }
            }
            else if (key.rfind("ZONE_", 0) == 0)
            {
                if (const std::optional<float> number = ParseNumber(text))
                {
                    wing.values[key] = *number;
                }
            }
        }
        spec.aeroWings.push_back(std::move(wing));
    }
    for (int index = 0; index < 32; ++index)
    {
        const std::string section = "DYNAMIC_CONTROLLER_" + std::to_string(index);
        if (!aero.HasSection(section))
        {
            break;
        }
        VehicleAeroController controller;
        controller.wing = static_cast<int>(aero.Number(section, "WING").value_or(0.0f));
        controller.input = Trim(aero.Text(section, "INPUT").value_or(""));
        controller.combinator = Trim(aero.Text(section, "COMBINATOR").value_or(""));
        controller.curve = LutPoints(files, aero.Text(section, "LUT").value_or(""));
        controller.filter = aero.Number(section, "FILTER").value_or(0.0f);
        controller.upLimit = aero.Number(section, "UP_LIMIT").value_or(0.0f);
        controller.downLimit = aero.Number(section, "DOWN_LIMIT").value_or(0.0f);
        spec.aeroControllers.push_back(std::move(controller));
    }
}

// ers.ini: the kinetic motor's curves and figures, the heat recovery's, and every ctrl_ers_N.ini
// profile with its controllers' curves. Nothing without a torque curve.
void ReadErs(const AcdArchive::Files& files, VehicleCarSpec& spec)
{
    const AcCarData::Ini ersIni = ParseFile(files, "ers.ini");
    const IniView ers(&ersIni);
    VehicleErs out;
    out.torqueCurve = LutPoints(files, ers.Text("KINETIC", "TORQUE_CURVE").value_or(""));
    if (out.torqueCurve.empty())
    {
        return;
    }
    out.coastCurve = LutPoints(files, ers.Text("KINETIC", "COAST_CURVE").value_or(""));
    out.chargeK = ers.Number("KINETIC", "CHARGE_K").value_or(0.0f);
    out.dischargeSeconds = ers.Number("KINETIC", "DISCHARGE_TIME").value_or(0.0f) / 1000.0f;
    out.maxKjPerLap = ers.Number("KINETIC", "MAX_KJ_PER_LAP").value_or(0.0f);
    out.hasButtonOverride = ers.Number("KINETIC", "HAS_BUTTON_OVERRIDE").value_or(0.0f) != 0.0f;
    out.brakeRearCorrection = ers.Number("KINETIC", "BRAKE_REAR_CORRECTION").value_or(0.0f);
    out.defaultProfile = static_cast<int>(ers.Number("KINETIC", "DEFAULT_CONTROLLER").value_or(0.0f));
    out.heatChargeK = ers.Number("HEAT", "CHARGE_K").value_or(0.0f);
    out.heatTorquePercent = ers.Number("HEAT", "TORQUE_PERC").value_or(0.0f);
    for (int index = 0; index < 16; ++index)
    {
        const std::string fileName = "ctrl_ers_" + std::to_string(index) + ".ini";
        if (FindFile(files, fileName) == nullptr)
        {
            break;
        }
        const AcCarData::Ini profileIni = ParseFile(files, fileName);
        VehicleErsProfile profile;
        profile.name = Trim(IniView(&profileIni).Text("HEADER", "NAME").value_or(""));
        profile.controllers = ReadControllers(files, fileName);
        out.profiles.push_back(std::move(profile));
    }
    spec.ers = std::move(out);
}

// colliders.ini: the body's boxes, centred from the centre of mass in the game's car axes (x left, y up,
// z forward, as the vehicle's), with their full sizes.
void ReadColliders(const AcdArchive::Files& files, VehicleCarSpec& spec)
{
    const AcCarData::Ini ini = ParseFile(files, "colliders.ini");
    const IniView colliders(&ini);
    for (int index = 0; index < 64; ++index)
    {
        const std::string section = "COLLIDER_" + std::to_string(index);
        if (!colliders.HasSection(section))
        {
            break;
        }
        const std::optional<std::vector<float>> center = ParseNumberList(colliders.Text(section, "CENTRE").value_or(""), 3);
        const std::optional<std::vector<float>> size = ParseNumberList(colliders.Text(section, "SIZE").value_or(""), 3);
        if (!center.has_value() || !size.has_value())
        {
            continue;
        }
        VehicleColliderBox box;
        box.center = glm::vec3((*center)[0], (*center)[1], (*center)[2]);
        box.size = glm::abs(glm::vec3((*size)[0], (*size)[1], (*size)[2]));
        box.groundEnabled = colliders.Number(section, "GROUND_ENABLE").value_or(1.0f) != 0.0f;
        spec.colliders.push_back(box);
    }
}

void ReadElectronics(const AcdArchive::Files& files, VehicleCarSpec& spec)
{
    for (const auto& [section, keys] : ParseFile(files, "electronics.ini"))
    {
        std::map<std::string, float> numbers;
        for (const auto& [key, text] : keys)
        {
            if (const std::optional<float> number = ParseNumber(text))
            {
                numbers[key] = *number;
            }
        }
        spec.electronics[section] = std::move(numbers);
    }
}
}

std::string DescribeCarSpec(const VehicleCarSpec& spec)
{
    std::string text;
    const auto append = [&text](const std::string& part)
    {
        text += text.empty() ? part : ", " + part;
    };
    char buffer[64];
    if (spec.massKg.has_value())
    {
        if (spec.fuelLitres.has_value() && *spec.fuelLitres > 0.0f)
        {
            std::snprintf(buffer, sizeof(buffer), "%.0f kg + %.0f L fuel", *spec.massKg, *spec.fuelLitres);
        }
        else
        {
            std::snprintf(buffer, sizeof(buffer), "%.0f kg", *spec.massKg);
        }
        append(buffer);
    }
    if (spec.drive.has_value())
    {
        append(*spec.drive == VehicleDrive::RearWheel ? "RWD" : *spec.drive == VehicleDrive::FrontWheel ? "FWD"
                                                                                                        : "AWD");
    }
    if (!spec.torqueCurve.empty())
    {
        float peak = 0.0f;
        for (const glm::vec2& point : spec.torqueCurve)
        {
            peak = std::max(peak, point.y);
        }
        std::snprintf(buffer, sizeof(buffer), "%.0f Nm", peak);
        append(buffer);
    }
    if (spec.ers.has_value())
    {
        float peak = 0.0f;
        for (const glm::vec2& point : spec.ers->torqueCurve)
        {
            peak = std::max(peak, point.y);
        }
        std::snprintf(buffer, sizeof(buffer), "ERS %.0f Nm", peak);
        append(buffer);
    }
    if (spec.maxRpm.has_value())
    {
        std::snprintf(buffer, sizeof(buffer), "%.0f rpm", *spec.maxRpm);
        append(buffer);
    }
    if (!spec.gearRatios.empty())
    {
        std::snprintf(buffer, sizeof(buffer), "%zu gears", spec.gearRatios.size());
        append(buffer);
    }
    if (spec.brakeTorquePerWheel.has_value())
    {
        append("brakes");
    }
    if (spec.suspensionFrequencyHz.has_value())
    {
        append("springs");
    }
    if (spec.frontTyres.has_value())
    {
        append("tyres");
    }
    if (!spec.aeroWings.empty())
    {
        append("aero");
    }
    return text;
}

namespace AcCarData
{
Ini ParseIni(const std::string& text)
{
    Ini ini;
    std::string section;
    size_t position = 0;
    while (position <= text.size())
    {
        size_t end = text.find('\n', position);
        if (end == std::string::npos)
        {
            end = text.size();
        }
        std::string line = text.substr(position, end - position);
        position = end + 1;

        const size_t comment = line.find(';');
        if (comment != std::string::npos)
        {
            line.resize(comment);
        }
        line = Trim(line);
        if (line.empty())
        {
            continue;
        }
        if (line.front() == '[')
        {
            const size_t close = line.find(']');
            section = ToUpperAscii(Trim(line.substr(1, close == std::string::npos ? std::string::npos : close - 1)));
            continue;
        }
        const size_t equals = line.find('=');
        if (equals == std::string::npos || section.empty())
        {
            continue;
        }
        ini[section][ToUpperAscii(Trim(line.substr(0, equals)))] = Trim(line.substr(equals + 1));
    }
    return ini;
}

std::vector<std::pair<float, float>> ParseLut(const std::string& text)
{
    std::vector<std::pair<float, float>> lut;
    size_t position = 0;
    while (position <= text.size())
    {
        size_t end = text.find('\n', position);
        if (end == std::string::npos)
        {
            end = text.size();
        }
        std::string line = text.substr(position, end - position);
        position = end + 1;

        const size_t comment = line.find(';');
        if (comment != std::string::npos)
        {
            line.resize(comment);
        }
        const size_t bar = line.find('|');
        if (bar == std::string::npos)
        {
            continue;
        }
        const std::optional<float> x = ParseNumber(line.substr(0, bar));
        const std::optional<float> y = ParseNumber(line.substr(bar + 1));
        if (x.has_value() && y.has_value())
        {
            lut.emplace_back(*x, *y);
        }
    }
    return lut;
}

float TurboBoost(float rpm, float maxBoost, float wastegate, float referenceRpm, float gamma)
{
    // As gro-ove's ac-torque-helper (src/acTurbo.jsx, calculateMultipler) has the game: the maximum
    // boost scaled by the revs, then cut at the wastegate. A turbo whose maximum is above its wastegate
    // so reaches the wastegate's level below the reference rpm.
    float level = std::max(maxBoost, 0.0f);
    if (referenceRpm > 0.0f)
    {
        level *= std::clamp(std::pow(std::max(rpm, 0.0f) / referenceRpm, std::max(gamma, 0.0f)), 0.0f, 1.0f);
    }
    if (wastegate > 0.0f)
    {
        level = std::min(level, wastegate);
    }
    return level;
}

VehicleCarSpec BuildSpec(const AcdArchive::Files& files)
{
    VehicleCarSpec spec;

    const Ini carIni = ParseFile(files, "car.ini");
    const IniView car(&carIni);
    if (const std::optional<float> mass = car.Number("BASIC", "TOTALMASS"); mass.has_value() && *mass > 0.0f)
    {
        spec.massKg = *mass;
    }
    // TOTALMASS has the driver but no fuel: the car starts with FUEL litres in its tank.
    if (const std::optional<float> fuel = car.Number("FUEL", "FUEL"); fuel.has_value() && *fuel > 0.0f)
    {
        spec.fuelLitres = *fuel;
        if (const std::optional<std::string> position = car.Text("FUELTANK", "POSITION"))
        {
            if (const std::optional<std::vector<float>> xyz = ParseNumberList(*position, 3))
            {
                spec.fuelTankPosition = glm::vec3((*xyz)[0], (*xyz)[1], (*xyz)[2]);
            }
        }
    }
    const std::optional<float> lock = car.Number("CONTROLS", "STEER_LOCK");
    const std::optional<float> ratio = car.Number("CONTROLS", "STEER_RATIO");
    if (lock.has_value() && ratio.has_value() && *lock > 0.0f && *ratio != 0.0f)
    {
        // The steering wheel's lock over the steering ratio is how far the front wheels turn. The
        // ratio's sign is only the steering's direction (the GT-R GT3 has -13.6).
        spec.steeringWheelLockDegrees = *lock;
        spec.maxSteerAngleDegrees = std::clamp(*lock / std::abs(*ratio), 8.0f, 60.0f);
    }

    ReadEngine(files, spec);
    ReadDrivetrain(files, spec);
    ReadBrakes(files, spec);
    ReadSuspension(files, spec);
    ReadTyres(files, spec);
    ReadAero(files, spec);
    ReadElectronics(files, spec);
    ReadErs(files, spec);
    ReadColliders(files, spec);
    spec.rearSteerControllers = ReadControllers(files, "ctrl_4ws.ini");
    return spec;
}

std::optional<VehicleCarSpec> ReadCarFolder(const std::filesystem::path& carFolder, std::string* problem)
{
    std::error_code ec;
    AcdArchive::Files files;
    try
    {
        const std::filesystem::path archive = carFolder / "data.acd";
        const std::filesystem::path unpacked = carFolder / "data";
        if (std::filesystem::is_regular_file(archive, ec))
        {
            files = AcdArchive::Load(archive);
        }
        else if (std::filesystem::is_directory(unpacked, ec))
        {
            for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(unpacked, ec))
            {
                if (!entry.is_regular_file())
                {
                    continue;
                }
                std::ifstream stream(entry.path(), std::ios::binary);
                files[ToLowerAscii(entry.path().filename().string())] =
                    std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
            }
        }
        else
        {
            return std::nullopt;
        }
    }
    catch (const std::exception& error)
    {
        if (problem != nullptr)
        {
            *problem = error.what();
        }
        return std::nullopt;
    }
    VehicleCarSpec spec = BuildSpec(files);
    // The body's shell (collider.kn5): its points in the model's frame, the dummies' transforms applied as
    // the car's kn5 has them. A shell that will not read leaves the car without one.
    const std::filesystem::path collider = carFolder / "collider.kn5";
    if (std::filesystem::is_regular_file(collider, ec))
    {
        try
        {
            const Kn5Model model = Kn5Reader::Load(collider);
            const std::function<void(const Kn5Node&, const glm::mat4&)> collect = [&](const Kn5Node& node, const glm::mat4& parent)
            {
                const glm::mat4 transform = node.HasGeometry() ? parent : parent * glm::make_mat4(node.matrix.data());
                for (const Kn5Vertex& vertex : node.vertices)
                {
                    const glm::vec3 point = glm::vec3(transform * glm::vec4(vertex.position[0], vertex.position[1], vertex.position[2], 1.0f));
                    if (std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z))
                    {
                        spec.colliderHull.push_back(point);
                    }
                }
                for (const Kn5Node& child : node.children)
                {
                    collect(child, transform);
                }
            };
            collect(model.root, glm::mat4(1.0f));
        }
        catch (const std::exception& error)
        {
            spec.colliderHull.clear();
            LOG_WARN("'{}': the body's collider was not read: {}", collider.string(), error.what());
        }
    }
    return spec;
}
}
}
