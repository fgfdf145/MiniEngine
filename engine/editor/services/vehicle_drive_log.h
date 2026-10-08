#pragma once

#include <engine/physics/vehicle_settings.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace me
{

// A drive written down frame by frame as CSV (docs/design/2026-10-09-drive-path-follow-design.md): for
// comparing runs, and its controls for replaying the drive exactly.
struct DriveLogHeader
{
    // Where the drive started: the body's position and heading (degrees, atan2 of its forward's x and z).
    glm::dvec3 startPosition{0.0};
    float startYawDegrees = 0.0f;
    std::string car;
    // The drive path followed; empty for a drive by hand or a replay.
    std::string path;
};

// One frame of the drive.
struct DriveLogSample
{
    double time = 0.0;
    // The simulated time this frame advanced (s): a replay steps the physics by it again.
    float deltaSeconds = 0.0f;
    glm::dvec3 position{0.0};
    float yawDegrees = 0.0f;
    float speedKmh = 0.0f;
    // Following a path: its planned speed, how far along it and on which lap, and how far off it.
    float targetKmh = 0.0f;
    double pathDistance = 0.0;
    int lap = 0;
    float lateralError = 0.0f;
    float headingErrorDegrees = 0.0f;
    // What the car was given, after the steering assist.
    VehicleControls controls;
    int gear = 0;
    float rpm = 0.0f;
    // In the body's frame (g): forward and to the right; yaw rate positive to the right.
    float longitudinalG = 0.0f;
    float lateralG = 0.0f;
    float yawRateDegrees = 0.0f;
    // The body's slip angle: its velocity against where it points (degrees, positive sliding right).
    float bodySlipDegrees = 0.0f;
    bool absActive = false;
    bool tractionControlCut = false;
    uint32_t wheelsOnGround = 0;
};

// The columns, comma separated, without a line end.
std::string DriveLogColumns();
// A sample as a row of those columns, without a line end. Floats are written with enough digits to
// read back exactly, so a replay gives the car the controls it had.
std::string FormatDriveLogRow(const DriveLogSample& sample);

class DriveLogWriter
{
  public:
    // Creates the file (and its folder) with the header; throws when it cannot.
    void Open(const std::filesystem::path& path, const DriveLogHeader& header);
    void Write(const DriveLogSample& sample);
    // Ends the file with a comment line (the run's summary) and closes it.
    void Close(const std::string& summary = {});
    bool IsOpen() const
    {
        return m_file.is_open();
    }
    const std::filesystem::path& Path() const
    {
        return m_path;
    }

  private:
    std::ofstream m_file;
    std::filesystem::path m_path;
};

// A frame of a drive to replay: how long it was and what the car was given.
struct DriveReplayFrame
{
    float deltaSeconds = 0.0f;
    VehicleControls controls;
};

struct DriveReplay
{
    DriveLogHeader header;
    std::vector<DriveReplayFrame> frames;
};

// Reads a drive log written by DriveLogWriter for replaying. Throws with the reason when the file is
// missing or not a drive log.
DriveReplay ReadDriveLog(const std::filesystem::path& path);

// What a run came to, gathered frame by frame.
struct DriveRunStats
{
    double seconds = 0.0;
    double metres = 0.0;
    float topSpeedKmh = 0.0f;
    float maxLateralG = 0.0f;
    float maxBrakingG = 0.0f;
    float maxAccelG = 0.0f;
    // Following a path: the lateral error's largest and its root mean square over the frames.
    bool followedPath = false;
    float maxLateralError = 0.0f;
    double lateralErrorSquares = 0.0;
    size_t frames = 0;

    void Add(const DriveLogSample& sample);
    float RmsLateralError() const;
    // One line: time, distance, top speed, g's and, following a path, its errors.
    std::string Describe() const;
};
}
