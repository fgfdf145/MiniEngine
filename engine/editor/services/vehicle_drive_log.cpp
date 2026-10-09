#include "vehicle_drive_log.h"

#include <fmt/format.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace me
{
namespace
{
std::vector<std::string_view> SplitCells(std::string_view line)
{
    std::vector<std::string_view> cells;
    size_t start = 0;
    while (start <= line.size())
    {
        const size_t end = std::min(line.find(',', start), line.size());
        cells.push_back(line.substr(start, end - start));
        start = end + 1;
    }
    return cells;
}

template <typename T>
T ParseCell(std::string_view cell, const std::string& what)
{
    T value{};
    const auto result = std::from_chars(cell.data(), cell.data() + cell.size(), value);
    if (result.ec != std::errc{} || result.ptr != cell.data() + cell.size())
    {
        throw std::runtime_error("cannot read " + what + " ('" + std::string(cell) + "')");
    }
    return value;
}

std::string_view Trim(std::string_view text)
{
    while (!text.empty() && (text.back() == '\r' || text.back() == ' '))
    {
        text.remove_suffix(1);
    }
    while (!text.empty() && text.front() == ' ')
    {
        text.remove_prefix(1);
    }
    return text;
}

// A column of the log: its name, and how a sample's value is written to it and read back from it.
struct Column
{
    std::string name;
    std::function<std::string(const DriveLogSample&)> write;
    std::function<void(DriveLogSample&, std::string_view)> read;
};

// A number held in the sample where `field` points, written with `format` ("{}" writes the shortest text
// that reads back to the same float or double). A flag is written 1 or 0.
template <typename Field>
Column Number(std::string name, const char* format, Field field)
{
    using T = std::remove_reference_t<decltype(field(std::declval<DriveLogSample&>()))>;
    Column column;
    column.name = std::move(name);
    column.write = [format, field](const DriveLogSample& sample)
    {
        const T value = field(const_cast<DriveLogSample&>(sample));
        if constexpr (std::is_same_v<T, bool>)
        {
            return std::string(value ? "1" : "0");
        }
        else
        {
            return fmt::format(fmt::runtime(format), value);
        }
    };
    column.read = [what = column.name, field](DriveLogSample& sample, std::string_view cell)
    {
        if constexpr (std::is_same_v<T, bool>)
        {
            field(sample) = ParseCell<int>(cell, what) != 0;
        }
        else
        {
            field(sample) = ParseCell<T>(cell, what);
        }
    };
    return column;
}

constexpr const char* kWheelNames[] = {"fl", "fr", "rl", "rr"};

std::vector<Column> BuildColumns()
{
    using S = DriveLogSample;
    std::vector<Column> columns = {
        Number("time", "{:.4f}", [](S& s) -> double& { return s.time; }),
        Number("dt", "{}", [](S& s) -> float& { return s.deltaSeconds; }),
        Number("steps", "{}", [](S& s) -> int& { return s.physicsSteps; }),
        Number("x", "{:.4f}", [](S& s) -> double& { return s.position.x; }),
        Number("y", "{:.4f}", [](S& s) -> double& { return s.position.y; }),
        Number("z", "{:.4f}", [](S& s) -> double& { return s.position.z; }),
        Number("yaw_deg", "{:.3f}", [](S& s) -> float& { return s.yawDegrees; }),
        Number("speed_kmh", "{:.3f}", [](S& s) -> float& { return s.speedKmh; }),
        Number("target_kmh", "{:.3f}", [](S& s) -> float& { return s.targetKmh; }),
        Number("path_m", "{:.3f}", [](S& s) -> double& { return s.pathDistance; }),
        Number("lap", "{}", [](S& s) -> int& { return s.lap; }),
        Number("lateral_error_m", "{:.4f}", [](S& s) -> float& { return s.lateralError; }),
        Number("heading_error_deg", "{:.3f}", [](S& s) -> float& { return s.headingErrorDegrees; }),
        Number("throttle", "{}", [](S& s) -> float& { return s.controls.throttle; }),
        Number("brake", "{}", [](S& s) -> float& { return s.controls.brake; }),
        Number("steering", "{}", [](S& s) -> float& { return s.controls.steering; }),
        Number("hand_brake", "{}", [](S& s) -> float& { return s.controls.handBrake; }),
        Number("gear_shifts", "{}", [](S& s) -> int& { return s.controls.gearShifts; }),
        Number("clutch_pedal", "", [](S& s) -> bool& { return s.controls.clutchPedal; }),
        Number("manual_gearbox", "", [](S& s) -> bool& { return s.controls.manualGearbox; }),
        Number("gear", "{}", [](S& s) -> int& { return s.gear; }),
        Number("rpm", "{:.0f}", [](S& s) -> float& { return s.rpm; }),
        Number("long_g", "{:.4f}", [](S& s) -> float& { return s.longitudinalG; }),
        Number("lat_g", "{:.4f}", [](S& s) -> float& { return s.lateralG; }),
        Number("yaw_rate_dps", "{:.3f}", [](S& s) -> float& { return s.yawRateDegrees; }),
        Number("body_slip_deg", "{:.3f}", [](S& s) -> float& { return s.bodySlipDegrees; }),
        Number("abs", "", [](S& s) -> bool& { return s.absActive; }),
        Number("tc", "", [](S& s) -> bool& { return s.tractionControlCut; }),
        Number("wheels_on_ground", "{}", [](S& s) -> uint32_t& { return s.wheelsOnGround; }),
        Number("qw", "{}", [](S& s) -> float& { return s.rotation.w; }),
        Number("qx", "{}", [](S& s) -> float& { return s.rotation.x; }),
        Number("qy", "{}", [](S& s) -> float& { return s.rotation.y; }),
        Number("qz", "{}", [](S& s) -> float& { return s.rotation.z; }),
        Number("pitch_deg", "{:.3f}", [](S& s) -> float& { return s.pitchDegrees; }),
        Number("roll_deg", "{:.3f}", [](S& s) -> float& { return s.rollDegrees; }),
        Number("right_kmh", "{:.3f}", [](S& s) -> float& { return s.rightKmh; }),
        Number("vertical_ms", "{:.4f}", [](S& s) -> float& { return s.verticalSpeed; }),
    };
    for (size_t w = 0; w < std::size(kWheelNames); ++w)
    {
        const std::string p = std::string(kWheelNames[w]) + "_";
        const auto wheel = [w](S& s) -> DriveLogWheel& { return s.wheels[w]; };
        const auto add = [&](const char* name, const char* format, auto field)
        {
            columns.push_back(Number(p + name, format, [wheel, field](S& s) -> decltype(auto) { return field(wheel(s)); }));
        };
        add("x", "{:.4f}", [](DriveLogWheel& v) -> double& { return v.position.x; });
        add("y", "{:.4f}", [](DriveLogWheel& v) -> double& { return v.position.y; });
        add("z", "{:.4f}", [](DriveLogWheel& v) -> double& { return v.position.z; });
        add("qw", "{}", [](DriveLogWheel& v) -> float& { return v.rotation.w; });
        add("qx", "{}", [](DriveLogWheel& v) -> float& { return v.rotation.x; });
        add("qy", "{}", [](DriveLogWheel& v) -> float& { return v.rotation.y; });
        add("qz", "{}", [](DriveLogWheel& v) -> float& { return v.rotation.z; });
        add("contact", "", [](DriveLogWheel& v) -> bool& { return v.inContact; });
        add("cx", "{:.4f}", [](DriveLogWheel& v) -> double& { return v.contactPosition.x; });
        add("cy", "{:.4f}", [](DriveLogWheel& v) -> double& { return v.contactPosition.y; });
        add("cz", "{:.4f}", [](DriveLogWheel& v) -> double& { return v.contactPosition.z; });
        add("nx", "{:.5f}", [](DriveLogWheel& v) -> float& { return v.contactNormal.x; });
        add("ny", "{:.5f}", [](DriveLogWheel& v) -> float& { return v.contactNormal.y; });
        add("nz", "{:.5f}", [](DriveLogWheel& v) -> float& { return v.contactNormal.z; });
        add("fx", "{:.5f}", [](DriveLogWheel& v) -> float& { return v.contactLongitudinal.x; });
        add("fy", "{:.5f}", [](DriveLogWheel& v) -> float& { return v.contactLongitudinal.y; });
        add("fz", "{:.5f}", [](DriveLogWheel& v) -> float& { return v.contactLongitudinal.z; });
        add("sx", "{:.5f}", [](DriveLogWheel& v) -> float& { return v.contactLateral.x; });
        add("sy", "{:.5f}", [](DriveLogWheel& v) -> float& { return v.contactLateral.y; });
        add("sz", "{:.5f}", [](DriveLogWheel& v) -> float& { return v.contactLateral.z; });
        add("carcass_x", "{:.6f}", [](DriveLogWheel& v) -> float& { return v.carcassDeflection.x; });
        add("carcass_y", "{:.6f}", [](DriveLogWheel& v) -> float& { return v.carcassDeflection.y; });
        add("carcass_twist", "{:.6f}", [](DriveLogWheel& v) -> float& { return v.carcassDeflection.z; });
        add("carcass_bend", "{}", [](DriveLogWheel& v) -> float& { return v.carcassBendingShape; });
        add("load_n", "{:.1f}", [](DriveLogWheel& v) -> float& { return v.load; });
        add("travel_mm", "{:.2f}", [](DriveLogWheel& v) -> float& { return v.travelMm; });
        add("slip_ratio", "{:.4f}", [](DriveLogWheel& v) -> float& { return v.slipRatio; });
        add("slip_deg", "{:.3f}", [](DriveLogWheel& v) -> float& { return v.slipAngleDegrees; });
    }
    return columns;
}

const std::vector<Column>& Columns()
{
    static const std::vector<Column> columns = BuildColumns();
    return columns;
}

DriveLogWheel MixWheels(const DriveLogWheel& before, const DriveLogWheel& after, float t)
{
    DriveLogWheel wheel = after;
    wheel.position = glm::mix(before.position, after.position, static_cast<double>(t));
    wheel.rotation = glm::slerp(before.rotation, after.rotation, t);
    if (before.inContact && after.inContact)
    {
        wheel.contactPosition = glm::mix(before.contactPosition, after.contactPosition, static_cast<double>(t));
    }
    return wheel;
}
}

std::string DriveLogColumns()
{
    std::string columns;
    for (const Column& column : Columns())
    {
        if (!columns.empty())
        {
            columns += ',';
        }
        columns += column.name;
    }
    return columns;
}

std::string FormatDriveLogRow(const DriveLogSample& sample)
{
    std::string row;
    for (const Column& column : Columns())
    {
        if (!row.empty())
        {
            row += ',';
        }
        row += column.write(sample);
    }
    return row;
}

void DriveLogWriter::Open(const std::filesystem::path& path, const DriveLogHeader& header)
{
    Close();
    if (path.has_parent_path())
    {
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
    }
    m_file.open(path, std::ios::binary | std::ios::trunc);
    if (!m_file)
    {
        throw std::runtime_error("cannot write the drive log '" + path.string() + "'");
    }
    m_path = path;
    m_file << "# MiniEngine drive log\n";
    // The start as x y z and the rotation's w x y z, each read back to the same number.
    const glm::quat& rotation = header.startRotation;
    m_file << fmt::format(
        "# start {} {} {} {} {} {} {}\n", header.startPosition.x, header.startPosition.y, header.startPosition.z, rotation.w, rotation.x,
        rotation.y, rotation.z);
    if (header.stepSeconds > 0.0f)
    {
        m_file << fmt::format("# step_seconds {}\n", header.stepSeconds);
    }
    if (!header.car.empty())
    {
        m_file << "# car " << header.car << '\n';
    }
    if (!header.path.empty())
    {
        m_file << "# path " << header.path << '\n';
    }
    m_file << DriveLogColumns() << '\n';
}

void DriveLogWriter::Write(const DriveLogSample& sample)
{
    if (m_file.is_open())
    {
        m_file << FormatDriveLogRow(sample) << '\n';
    }
}

void DriveLogWriter::Close(const std::string& summary)
{
    if (!m_file.is_open())
    {
        return;
    }
    if (!summary.empty())
    {
        m_file << "# summary " << summary << '\n';
    }
    m_file.close();
}

DriveReplay ReadDriveLog(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        throw std::runtime_error("cannot open the drive log '" + path.string() + "'");
    }
    DriveReplay replay;
    // For each of the file's columns, ours of that name; columns we do not know are passed over.
    std::vector<const Column*> fileColumns;
    std::string line;
    size_t lineNumber = 0;
    while (std::getline(file, line))
    {
        ++lineNumber;
        const std::string_view text = Trim(line);
        if (text.empty())
        {
            continue;
        }
        if (text.front() == '#')
        {
            std::istringstream comment{std::string(text.substr(1))};
            std::string key;
            comment >> key;
            std::vector<std::string> values;
            for (std::string value; comment >> value;)
            {
                values.push_back(value);
            }
            const std::string where = fmt::format("line {}", lineNumber);
            if (key == "start" && values.size() == 7)
            {
                DriveLogHeader& header = replay.header;
                header.startPosition = glm::dvec3(
                    ParseCell<double>(values[0], "the start at " + where),
                    ParseCell<double>(values[1], "the start at " + where),
                    ParseCell<double>(values[2], "the start at " + where));
                header.startRotation = glm::quat(
                    ParseCell<float>(values[3], "the start at " + where),
                    ParseCell<float>(values[4], "the start at " + where),
                    ParseCell<float>(values[5], "the start at " + where),
                    ParseCell<float>(values[6], "the start at " + where));
            }
            else if (key == "step_seconds" && values.size() == 1)
            {
                replay.header.stepSeconds = ParseCell<float>(values[0], "the step at " + where);
            }
            else if (key == "car" || key == "path")
            {
                // The rest of the line, spaces and all.
                std::string_view value = text.substr(1);
                value.remove_prefix(std::min(value.find(key) + key.size(), value.size()));
                (key == "car" ? replay.header.car : replay.header.path) = std::string(Trim(value));
            }
            continue;
        }
        const std::vector<std::string_view> cells = SplitCells(text);
        if (fileColumns.empty())
        {
            std::unordered_map<std::string_view, const Column*> byName;
            for (const Column& column : Columns())
            {
                byName.emplace(column.name, &column);
            }
            for (const std::string_view cell : cells)
            {
                const auto it = byName.find(cell);
                fileColumns.push_back(it != byName.end() ? it->second : nullptr);
            }
            const auto has = [&](std::string_view name)
            {
                return std::any_of(fileColumns.begin(), fileColumns.end(), [&](const Column* column)
                                   {
                                       return column != nullptr && column->name == name;
                                   });
            };
            if (!has("time") || !has("x"))
            {
                throw std::runtime_error("'" + path.string() + "' has no time or position: it is not a drive log");
            }
            if (!has("qw") || !has("fl_qw"))
            {
                throw std::runtime_error("'" + path.string() + "' was written before the body's rotation and the wheels were: write the drive again to play it back");
            }
            continue;
        }
        if (cells.size() < fileColumns.size())
        {
            throw std::runtime_error(fmt::format("'{}' line {}: {} cells, {} columns", path.string(), lineNumber, cells.size(), fileColumns.size()));
        }
        DriveLogSample sample;
        for (size_t index = 0; index < fileColumns.size(); ++index)
        {
            if (fileColumns[index] == nullptr)
            {
                continue;
            }
            try
            {
                fileColumns[index]->read(sample, cells[index]);
            }
            catch (const std::exception& error)
            {
                throw std::runtime_error(fmt::format("'{}' line {}: {}", path.string(), lineNumber, error.what()));
            }
        }
        replay.samples.push_back(sample);
    }
    if (fileColumns.empty())
    {
        throw std::runtime_error("'" + path.string() + "' is empty: it is not a drive log");
    }
    return replay;
}

DriveLogSample SampleDriveAt(const std::vector<DriveLogSample>& samples, double seconds, size_t& cursor)
{
    if (samples.empty())
    {
        return {};
    }
    cursor = std::min(cursor, samples.size() - 1);
    if (samples[cursor].time > seconds)
    {
        cursor = 0; // gone back
    }
    while (cursor + 1 < samples.size() && samples[cursor + 1].time <= seconds)
    {
        ++cursor;
    }
    const DriveLogSample& before = samples[cursor];
    if (seconds <= before.time || cursor + 1 >= samples.size())
    {
        return before;
    }
    const DriveLogSample& after = samples[cursor + 1];
    const double span = after.time - before.time;
    const float t = span > 0.0 ? static_cast<float>((seconds - before.time) / span) : 1.0f;
    DriveLogSample sample = after;
    sample.time = seconds;
    sample.position = glm::mix(before.position, after.position, static_cast<double>(t));
    sample.rotation = glm::slerp(before.rotation, after.rotation, t);
    for (size_t index = 0; index < sample.wheels.size(); ++index)
    {
        sample.wheels[index] = MixWheels(before.wheels[index], after.wheels[index], t);
    }
    return sample;
}

void DriveRunStats::Add(const DriveLogSample& sample)
{
    seconds += sample.deltaSeconds;
    metres += std::abs(sample.speedKmh / 3.6) * sample.deltaSeconds;
    topSpeedKmh = std::max(topSpeedKmh, std::abs(sample.speedKmh));
    maxLateralG = std::max(maxLateralG, std::abs(sample.lateralG));
    maxBrakingG = std::max(maxBrakingG, -sample.longitudinalG);
    maxAccelG = std::max(maxAccelG, sample.longitudinalG);
    if (followedPath)
    {
        maxLateralError = std::max(maxLateralError, std::abs(sample.lateralError));
        lateralErrorSquares += static_cast<double>(sample.lateralError) * sample.lateralError;
    }
    ++frames;
}

float DriveRunStats::RmsLateralError() const
{
    return frames > 0 ? static_cast<float>(std::sqrt(lateralErrorSquares / static_cast<double>(frames))) : 0.0f;
}

std::string DriveRunStats::Describe() const
{
    std::string text = fmt::format(
        "{:.2f} s, {:.0f} m, top {:.1f} km/h, lateral {:.2f} g, braking {:.2f} g, accelerating {:.2f} g",
        seconds, metres, topSpeedKmh, maxLateralG, maxBrakingG, maxAccelG);
    if (followedPath)
    {
        text += fmt::format(", off the path {:.2f} m RMS, {:.2f} m at most", RmsLateralError(), maxLateralError);
    }
    return text;
}
}
