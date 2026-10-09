#pragma once

#include <engine/physics/vehicle_settings.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace me
{

// A drive written down frame by frame as CSV (docs/design/2026-10-09-drive-path-follow-design.md): for
// comparing runs, and for playing the drive back as it was, the body and wheels where they were.
struct DriveLogHeader
{
    // Where the drive started: the body's pose, as a replay puts it back.
    glm::dvec3 startPosition{0.0};
    glm::quat startRotation{1.0f, 0.0f, 0.0f, 0.0f};
    // The physics' fixed step (s), which a replay runs at; 0 when not known.
    float stepSeconds = 0.0f;
    std::string car;
    // The drive path followed; empty for a drive by hand or a replay.
    std::string path;
};

// A wheel in a frame of the drive (VehicleWheelState): where it was, as drawn, and what its tyre did.
struct DriveLogWheel
{
    // World space; the rotation includes the wheel's roll.
    glm::dvec3 position{0.0};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    // Where the tyre touched the ground and the ground's directions there, which the tyre is squashed by.
    bool inContact = false;
    glm::dvec3 contactPosition{0.0};
    glm::vec3 contactNormal{0.0f, 1.0f, 0.0f};
    glm::vec3 contactLongitudinal{0.0f, 0.0f, 1.0f};
    glm::vec3 contactLateral{1.0f, 0.0f, 0.0f};
    glm::vec3 carcassDeflection{0.0f};
    float carcassBendingShape = 0.0f;
    // The tyre's load (N), the suspension's travel from its design position (mm, bump positive; 0 without
    // a multibody suspension), and the slip.
    float load = 0.0f;
    float travelMm = 0.0f;
    float slipRatio = 0.0f;
    float slipAngleDegrees = 0.0f;
};

// One frame of the drive.
struct DriveLogSample
{
    double time = 0.0;
    // The simulated time this frame advanced (s), and the physics steps it took: a replay runs as many.
    float deltaSeconds = 0.0f;
    int physicsSteps = 0;
    glm::dvec3 position{0.0};
    float yawDegrees = 0.0f;
    float speedKmh = 0.0f;
    // The body's whole rotation, and its pitch (nose up positive) and roll (leaning right positive) in
    // degrees; its speed to the right and up (km/h, m/s).
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    float pitchDegrees = 0.0f;
    float rollDegrees = 0.0f;
    float rightKmh = 0.0f;
    float verticalSpeed = 0.0f;
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
    // Front left, front right, rear left, rear right.
    std::array<DriveLogWheel, 4> wheels{};
};

// The columns, comma separated, without a line end.
std::string DriveLogColumns();
// A sample as a row of those columns, without a line end. The controls and rotations are written with
// enough digits to read back exactly.
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

// A drive to play back: its frames as written.
struct DriveReplay
{
    DriveLogHeader header;
    std::vector<DriveLogSample> samples;
};

// Reads a drive log written by DriveLogWriter for playing back. Throws with the reason when the file is
// missing, not a drive log, or written before the body and wheels were (it cannot be played back).
DriveReplay ReadDriveLog(const std::filesystem::path& path);

// The drive at `seconds` (DriveLogSample::time) between the frames either side: the body and wheels
// moved between them, everything else as the later frame had it. Before the first frame, the first;
// after the last, the last. `cursor` is the frame looked from, kept between calls going forward.
DriveLogSample SampleDriveAt(const std::vector<DriveLogSample>& samples, double seconds, size_t& cursor);

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
