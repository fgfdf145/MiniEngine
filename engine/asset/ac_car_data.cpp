#include "ac_car_data.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
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

struct Turbo
{
    float maxBoost = 0.0f;
    float wastegate = 0.0f;
    float referenceRpm = 0.0f;
    float gamma = 1.0f;
};

void ReadEngine(const AcdArchive::Files& files, VehicleCarSpec& spec)
{
    const AcCarData::Ini engineIni = ParseFile(files, "engine.ini");
    const IniView engine(&engineIni);
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

    std::vector<Turbo> turbos;
    for (const auto& [section, keys] : engineIni)
    {
        if (section.rfind("TURBO_", 0) != 0)
        {
            continue;
        }
        Turbo turbo;
        turbo.maxBoost = engine.Number(section, "MAX_BOOST").value_or(0.0f);
        turbo.wastegate = engine.Number(section, "WASTEGATE").value_or(0.0f);
        turbo.referenceRpm = engine.Number(section, "REFERENCE_RPM").value_or(0.0f);
        turbo.gamma = engine.Number(section, "GAMMA").value_or(1.0f);
        turbos.push_back(turbo);
    }

    float lastRpm = 0.0f;
    for (const auto& [rpm, torque] : lut)
    {
        float boost = 0.0f;
        for (const Turbo& turbo : turbos)
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
    }
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

    // Each axle's spring as a natural frequency of the sprung mass on one of its wheels, and its
    // dampers as a fraction of critical damping. The motion ratio of the linkage is not known.
    const float frontWeight = std::clamp(suspension.Number("BASIC", "CG_LOCATION").value_or(0.5f), 0.2f, 0.8f);
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
        std::snprintf(buffer, sizeof(buffer), "%.0f kg", *spec.massKg);
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
    float level = std::max(maxBoost, 0.0f);
    if (wastegate > 0.0f)
    {
        level = std::min(level, wastegate);
    }
    if (referenceRpm > 0.0f)
    {
        level *= std::clamp(std::pow(std::max(rpm, 0.0f) / referenceRpm, std::max(gamma, 0.0f)), 0.0f, 1.0f);
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
    const std::optional<float> lock = car.Number("CONTROLS", "STEER_LOCK");
    const std::optional<float> ratio = car.Number("CONTROLS", "STEER_RATIO");
    if (lock.has_value() && ratio.has_value() && *lock > 0.0f && *ratio > 0.0f)
    {
        // The steering wheel's lock over the steering ratio is how far the front wheels turn.
        spec.steeringWheelLockDegrees = *lock;
        spec.maxSteerAngleDegrees = std::clamp(*lock / *ratio, 8.0f, 60.0f);
    }

    ReadEngine(files, spec);
    ReadDrivetrain(files, spec);
    ReadBrakes(files, spec);
    ReadSuspension(files, spec);
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
    return BuildSpec(files);
}
}
}
