#include "vehicle_drive_log.h"

#include <fmt/format.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace me
{
namespace
{
constexpr const char* kColumns[] = {
    "time",
    "dt",
    "x",
    "y",
    "z",
    "yaw_deg",
    "speed_kmh",
    "target_kmh",
    "path_m",
    "lap",
    "lateral_error_m",
    "heading_error_deg",
    "throttle",
    "brake",
    "steering",
    "hand_brake",
    "gear_shifts",
    "clutch_pedal",
    "manual_gearbox",
    "gear",
    "rpm",
    "long_g",
    "lat_g",
    "yaw_rate_dps",
    "body_slip_deg",
    "abs",
    "tc",
    "wheels_on_ground",
};

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
}

std::string DriveLogColumns()
{
    std::string columns;
    for (const char* column : kColumns)
    {
        if (!columns.empty())
        {
            columns += ',';
        }
        columns += column;
    }
    return columns;
}

std::string FormatDriveLogRow(const DriveLogSample& sample)
{
    const VehicleControls& controls = sample.controls;
    // {} writes the shortest text that reads back to the same float or double.
    return fmt::format(
        "{:.4f},{},{:.4f},{:.4f},{:.4f},{:.3f},{:.3f},{:.3f},{:.3f},{},{:.4f},{:.3f},{},{},{},{},{},{},{},{},{:.0f},{:.4f},{:.4f},{:.3f},{:.3f},{},{},{}",
        sample.time,
        sample.deltaSeconds,
        sample.position.x,
        sample.position.y,
        sample.position.z,
        sample.yawDegrees,
        sample.speedKmh,
        sample.targetKmh,
        sample.pathDistance,
        sample.lap,
        sample.lateralError,
        sample.headingErrorDegrees,
        controls.throttle,
        controls.brake,
        controls.steering,
        controls.handBrake,
        controls.gearShifts,
        controls.clutchPedal ? 1 : 0,
        controls.manualGearbox ? 1 : 0,
        sample.gear,
        sample.rpm,
        sample.longitudinalG,
        sample.lateralG,
        sample.yawRateDegrees,
        sample.bodySlipDegrees,
        sample.absActive ? 1 : 0,
        sample.tractionControlCut ? 1 : 0,
        sample.wheelsOnGround);
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
    m_file << fmt::format(
        "# start {} {} {} {}\n", header.startPosition.x, header.startPosition.y, header.startPosition.z, header.startYawDegrees);
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
    std::unordered_map<std::string, size_t> columns;
    std::string line;
    size_t lineNumber = 0;
    const auto column = [&](const char* name)
    {
        const auto it = columns.find(name);
        if (it == columns.end())
        {
            throw std::runtime_error("'" + path.string() + "' has no " + name + " column: it is not a drive log");
        }
        return it->second;
    };
    size_t dt = 0, throttle = 0, brake = 0, steering = 0, handBrake = 0, gearShifts = 0, clutch = 0, manual = 0;
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
            if (key == "start")
            {
                comment >> replay.header.startPosition.x >> replay.header.startPosition.y >> replay.header.startPosition.z >>
                    replay.header.startYawDegrees;
            }
            else if (key == "car" || key == "path")
            {
                std::string value;
                std::getline(comment >> std::ws, value);
                (key == "car" ? replay.header.car : replay.header.path) = value;
            }
            continue;
        }
        const std::vector<std::string_view> cells = SplitCells(text);
        if (columns.empty())
        {
            for (size_t index = 0; index < cells.size(); ++index)
            {
                columns.emplace(std::string(cells[index]), index);
            }
            dt = column("dt");
            throttle = column("throttle");
            brake = column("brake");
            steering = column("steering");
            handBrake = column("hand_brake");
            gearShifts = column("gear_shifts");
            clutch = column("clutch_pedal");
            manual = column("manual_gearbox");
            continue;
        }
        if (cells.size() < columns.size())
        {
            throw std::runtime_error(fmt::format("'{}' line {}: {} cells, {} columns", path.string(), lineNumber, cells.size(), columns.size()));
        }
        const std::string where = fmt::format("line {}", lineNumber);
        DriveReplayFrame frame;
        frame.deltaSeconds = ParseCell<float>(cells[dt], "dt at " + where);
        frame.controls.throttle = ParseCell<float>(cells[throttle], "throttle at " + where);
        frame.controls.brake = ParseCell<float>(cells[brake], "brake at " + where);
        frame.controls.steering = ParseCell<float>(cells[steering], "steering at " + where);
        frame.controls.handBrake = ParseCell<float>(cells[handBrake], "hand_brake at " + where);
        frame.controls.gearShifts = ParseCell<int>(cells[gearShifts], "gear_shifts at " + where);
        frame.controls.clutchPedal = ParseCell<int>(cells[clutch], "clutch_pedal at " + where) != 0;
        frame.controls.manualGearbox = ParseCell<int>(cells[manual], "manual_gearbox at " + where) != 0;
        replay.frames.push_back(frame);
    }
    if (columns.empty())
    {
        throw std::runtime_error("'" + path.string() + "' is empty: it is not a drive log");
    }
    return replay;
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
