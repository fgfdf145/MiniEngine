#include <engine/editor/services/vehicle_drive_log.h>
#include <engine/editor/services/vehicle_path_follower.h>
#include <engine/logic/editor_world.h>

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

// main() stays in the global namespace; everything it drives lives in me::.
using namespace me;

namespace
{
void Require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

constexpr float kFrame = 1.0f / 60.0f;
constexpr float kGravity = 9.81f;

// A kinematic bicycle: the rear axle rolls along the body's heading, the front wheels turn it, the
// steering follows the command with a short lag, and throttle and brake give a plain acceleration.
struct BicycleCar
{
    glm::dvec3 rearAxle{0.0};
    float heading = 0.0f; // atan2(forward.x, forward.z)
    float speed = 0.0f;
    float wheelAngle = 0.0f;
    float wheelbase = 2.6f;
    float maxSteerDegrees = 35.0f;
    // Above 0 the car understeers: its path curves as tan(wheel angle) / (wheelbase + this * speed^2).
    float understeerGradient = 0.0f;

    glm::vec3 Forward() const
    {
        return glm::vec3(std::sin(heading), 0.0f, std::cos(heading));
    }
    // The body's origin: mid-wheelbase.
    glm::dvec3 Origin() const
    {
        return rearAxle + glm::dvec3(Forward()) * static_cast<double>(wheelbase * 0.5f);
    }
    PathFollowerInput Input() const
    {
        PathFollowerInput input;
        input.position = Origin();
        input.forward = Forward();
        input.forwardSpeed = speed;
        input.wheelbase = wheelbase;
        input.rearAxleOffset = -wheelbase * 0.5f;
        input.maxSteerDegrees = maxSteerDegrees;
        return input;
    }
    void Step(const VehicleControls& controls, float dt)
    {
        const float wanted = controls.steering * glm::radians(maxSteerDegrees);
        wheelAngle += (wanted - wheelAngle) * std::min(dt / 0.08f, 1.0f);
        const float accel = controls.throttle * 4.0f - controls.brake * 9.0f * (speed > 0.0f ? 1.0f : 0.0f) - 0.0004f * speed * speed;
        speed = std::max(speed + accel * dt, 0.0f);
        // Steering right turns the heading down (+X is the car's left).
        heading -= speed / (wheelbase + understeerGradient * speed * speed) * std::tan(wheelAngle) * dt;
        rearAxle += glm::dvec3(Forward()) * static_cast<double>(speed * dt);
    }
};

// Puts the car on the track's start, facing along it.
BicycleCar CarAtStart(const DrivePathTrack& track)
{
    BicycleCar car;
    const glm::vec3 tangent = track.samples.front().tangent;
    car.heading = std::atan2(tangent.x, tangent.z);
    car.rearAxle = track.samples.front().position - glm::dvec3(car.Forward()) * static_cast<double>(car.wheelbase * 0.5f);
    return car;
}

struct RunResult
{
    PathFollowerState state;
    float seconds = 0.0f;
    float maxLateralError = 0.0f;
    float maxLateralErrorAfterSettling = 0.0f;
    float topSpeed = 0.0f;
    // On the last lap of a closed path.
    float lastLapMaxLateralError = 0.0f;
    glm::dvec3 end{0.0};
};

RunResult Run(const DrivePathTrack& track, BicycleCar car, float maxSeconds, const PathFollowerSettings& settings = {})
{
    RunResult result;
    while (result.seconds < maxSeconds && result.state.status == PathFollowerStatus::Running)
    {
        PathFollowerOutput output;
        const VehicleControls controls = ComputePathFollowControls(track, settings, car.Input(), result.state, kFrame, &output);
        car.Step(controls, kFrame);
        result.seconds += kFrame;
        result.maxLateralError = std::max(result.maxLateralError, std::abs(output.lateralError));
        if (result.seconds > 3.0f)
        {
            result.maxLateralErrorAfterSettling = std::max(result.maxLateralErrorAfterSettling, std::abs(output.lateralError));
        }
        result.topSpeed = std::max(result.topSpeed, car.speed);
        if (track.closed && output.lap == track.laps - 1)
        {
            result.lastLapMaxLateralError = std::max(result.lastLapMaxLateralError, std::abs(output.lateralError));
        }
    }
    result.end = car.Origin();
    return result;
}

SceneDrivePath Circle(double radius, int points, float speedKmh, int laps)
{
    SceneDrivePath path;
    path.name = "circle";
    path.closed = true;
    path.speedKmh = speedKmh;
    path.laps = laps;
    for (int index = 0; index < points; ++index)
    {
        // From +X towards +Z: the centre is on the car's right (-X of a car heading +Z), so it turns right.
        const double angle = 2.0 * glm::pi<double>() * index / points;
        path.points.push_back({glm::dvec3(radius * std::cos(angle), 0.0, radius * std::sin(angle)), 0.0f});
    }
    return path;
}

void SplinePassesThroughItsPoints()
{
    SceneDrivePath path;
    path.speedKmh = 50.0f;
    path.points = {{{0.0, 0.0, 0.0}}, {{0.0, 0.0, 30.0}}, {{12.0, 1.0, 55.0}}, {{40.0, 2.0, 60.0}}, {{41.0, 2.0, 61.0}}};
    const DrivePathTrack track = BuildDrivePathTrack(path);
    Require(!track.Empty(), "the track is empty");
    for (const SceneDrivePathPoint& point : path.points)
    {
        const DrivePathProjection projection = ProjectOntoDrivePath(track, point.position);
        const double off = glm::length(projection.closest - point.position);
        Require(off < 0.02, "the curve misses a point by " + std::to_string(off) + " m");
    }
    // Spacing stays near the setting.
    for (size_t index = 1; index < track.samples.size(); ++index)
    {
        const double gap = track.samples[index].distance - track.samples[index - 1].distance;
        Require(gap > 0.4 && gap < 0.6, "samples are " + std::to_string(gap) + " m apart");
    }
}

void StraightPathBrakesToItsEnd()
{
    SceneDrivePath path;
    path.speedKmh = 72.0f; // 20 m/s
    path.points = {{{0.0, 0.0, 0.0}}, {{0.0, 0.0, 200.0}}};
    const DrivePathTrack track = BuildDrivePathTrack(path);
    Require(std::abs(track.length - 200.0) < 0.01, "a straight's length is " + std::to_string(track.length));
    Require(track.samples.back().speed == 0.0f, "an open path does not end at rest");
    Require(std::abs(track.samples.front().speed - 20.0f) < 0.01f, "the start speed is " + std::to_string(track.samples.front().speed));
    // No faster than braking at 0.8 g allows to stop at the end.
    for (const DrivePathSample& sample : track.samples)
    {
        const float allowed = std::sqrt(2.0f * 0.8f * kGravity * static_cast<float>(track.length - sample.distance)) + 1.0e-3f;
        Require(sample.speed <= allowed, "the plan does not brake in time at " + std::to_string(sample.distance) + " m");
    }
    // At 20 m/s the braking takes 20^2 / (2 * 0.8 g) = 25.5 m.
    Require(DrivePathSpeedAt(track, 170.0) > 19.9f, "the plan brakes too early");
}

void SpeedCapsAndScale()
{
    DrivePathTrackSettings settings;
    settings.lateralGrip = 0.5f;
    const DrivePathTrack capped = BuildDrivePathTrack(Circle(30.0, 16, 100.0f, 1), settings);
    const float limit = std::sqrt(0.5f * kGravity * 30.0f);
    for (const DrivePathSample& sample : capped.samples)
    {
        Require(sample.speed <= limit * 1.05f, "the lateral grip cap lets " + std::to_string(sample.speed) + " m/s through");
        Require(sample.curvature > 0.02f && sample.curvature < 0.047f, "a right-hand circle's curvature is " + std::to_string(sample.curvature));
    }
    settings = {};
    settings.speedScale = 0.5f;
    const DrivePathTrack scaled = BuildDrivePathTrack(Circle(30.0, 16, 72.0f, 1), settings);
    Require(std::abs(scaled.samples[10].speed - 10.0f) < 0.01f, "the speed scale gives " + std::to_string(scaled.samples[10].speed));
}

void FollowsACircleForItsLaps()
{
    const DrivePathTrack track = BuildDrivePathTrack(Circle(30.0, 12, 40.0f, 2));
    const RunResult result = Run(track, CarAtStart(track), 120.0f);
    Require(result.state.status == PathFollowerStatus::Finished, "the circle ends " + std::string(PathFollowerStatusName(result.state.status)) + ": " + result.state.failure);
    std::cout << "circle: " << result.seconds << " s, off by " << result.maxLateralErrorAfterSettling << " m after settling, top "
              << result.topSpeed * 3.6f << " km/h\n";
    Require(result.state.lap == 2, "laps: " + std::to_string(result.state.lap));
    Require(result.maxLateralErrorAfterSettling < 0.5f, "off the circle by " + std::to_string(result.maxLateralErrorAfterSettling) + " m");
    Require(result.topSpeed > 40.0f / 3.6f * 0.95f && result.topSpeed < 40.0f / 3.6f * 1.05f, "top speed " + std::to_string(result.topSpeed * 3.6f) + " km/h");
    // Two laps of 188 m at 11.1 m/s, plus getting up to speed.
    Require(result.seconds > 30.0f && result.seconds < 45.0f, "two laps took " + std::to_string(result.seconds) + " s");
}

void HoldsACircleWhenTheCarUndersteers()
{
    // At 50 km/h on a 30 m radius this car needs about half as much lock again as the geometry says.
    const DrivePathTrack track = BuildDrivePathTrack(Circle(30.0, 12, 50.0f, 3));
    BicycleCar car = CarAtStart(track);
    car.understeerGradient = 0.007f;
    const RunResult result = Run(track, car, 120.0f);
    Require(result.state.status == PathFollowerStatus::Finished, "the understeering circle ends " + std::string(PathFollowerStatusName(result.state.status)));
    PathFollowerSettings pursuitOnly;
    pursuitOnly.lateralIntegralGain = 0.0f;
    const RunResult plain = Run(track, car, 120.0f, pursuitOnly);
    std::cout << "understeering circle: off by " << result.maxLateralErrorAfterSettling << " m at most, " << result.lastLapMaxLateralError
              << " m on the last lap (" << plain.lastLapMaxLateralError << " m with pure pursuit alone)\n";
    Require(plain.maxLateralErrorAfterSettling > 0.5f, "the test car does not understeer enough to matter");
    Require(result.lastLapMaxLateralError < 0.3f, "the integral leaves it " + std::to_string(result.lastLapMaxLateralError) + " m off on the last lap");
}

void FollowsALaneChangeAndStops()
{
    SceneDrivePath path;
    path.speedKmh = 60.0f;
    path.points = {{{0.0, 0.0, 0.0}}, {{0.0, 0.0, 60.0}}, {{-3.5, 0.0, 85.0}}, {{-3.5, 0.0, 150.0}}};
    const DrivePathTrack track = BuildDrivePathTrack(path);
    const RunResult result = Run(track, CarAtStart(track), 60.0f);
    Require(result.state.status == PathFollowerStatus::Finished, "the lane change ends " + std::string(PathFollowerStatusName(result.state.status)) + ": " + result.state.failure);
    std::cout << "lane change: " << result.seconds << " s, off by " << result.maxLateralError << " m, stopped "
              << glm::length(result.end - path.points.back().position) << " m from the end\n";
    Require(result.maxLateralError < 0.6f, "off the lane change by " + std::to_string(result.maxLateralError) + " m");
    const double miss = glm::length(result.end - path.points.back().position);
    Require(miss < 2.0, "it stops " + std::to_string(miss) + " m from the end");
}

void FailsWhenFarOff()
{
    SceneDrivePath path;
    path.points = {{{0.0, 0.0, 0.0}}, {{0.0, 0.0, 100.0}}};
    const DrivePathTrack track = BuildDrivePathTrack(path);
    BicycleCar car = CarAtStart(track);
    car.rearAxle.x += 20.0;
    const RunResult result = Run(track, car, 5.0f);
    Require(result.state.status == PathFollowerStatus::Failed, "a car 20 m off does not fail");
    Require(result.state.failure.find("left the path") != std::string::npos, "the failure says " + result.state.failure);
}

void StartJustBehindAClosedStartCountsNoLap()
{
    const DrivePathTrack track = BuildDrivePathTrack(Circle(30.0, 12, 30.0f, 1));
    BicycleCar car = CarAtStart(track);
    car.rearAxle -= glm::dvec3(car.Forward()) * 0.5; // the origin just short of the start
    const RunResult result = Run(track, car, 60.0f);
    Require(result.state.status == PathFollowerStatus::Finished, "the lap ends " + std::string(PathFollowerStatusName(result.state.status)));
    // One whole lap of 188 m at 8.3 m/s, not a lap counted on crossing the start at once.
    Require(result.seconds > 20.0f, "one lap took only " + std::to_string(result.seconds) + " s");
}

void DriveLogRoundTrips(const std::filesystem::path& folder)
{
    const std::filesystem::path file = folder / "drive_path_tests_log.csv";
    DriveLogHeader header;
    header.startPosition = glm::dvec3(62.087, 100.0006, -20.827);
    header.startRotation = glm::normalize(glm::quat(0.1f, 0.2f, 0.95f, -0.05f));
    header.stepSeconds = 1.0f / 1000.0f;
    header.car = "skyline_r34_vspec";
    header.path = "lane change 2";
    DriveLogWriter writer;
    writer.Open(file, header);
    DriveLogSample sample;
    for (int frame = 0; frame < 3; ++frame)
    {
        sample.time = frame * kFrame;
        sample.deltaSeconds = kFrame * (1.0f + 0.1f * frame);
        sample.physicsSteps = 16 + frame;
        sample.controls.throttle = 0.123456789f * frame;
        sample.controls.steering = -0.333333343f;
        sample.controls.brake = frame == 2 ? 1.0f : 0.0f;
        sample.controls.gearShifts = frame == 1 ? 1 : 0;
        sample.controls.manualGearbox = true;
        writer.Write(sample);
    }
    writer.Close("done");

    const std::string columns = DriveLogColumns();
    const std::string row = FormatDriveLogRow(sample);
    Require(std::count(columns.begin(), columns.end(), ',') == std::count(row.begin(), row.end(), ','), "a row's cells do not match the columns");

    const DriveReplay replay = ReadDriveLog(file);
    Require(replay.frames.size() == 3, "the log reads back " + std::to_string(replay.frames.size()) + " frames");
    Require(replay.header.car == "skyline_r34_vspec" && replay.header.path == "lane change 2", "the header reads back wrong");
    Require(replay.header.startPosition == header.startPosition && replay.header.startRotation == header.startRotation, "the start reads back wrong");
    Require(replay.header.stepSeconds == header.stepSeconds, "the step reads back wrong");
    for (int frame = 0; frame < 3; ++frame)
    {
        const DriveReplayFrame& read = replay.frames[frame];
        Require(read.deltaSeconds == kFrame * (1.0f + 0.1f * frame) && read.physicsSteps == 16 + frame, "dt is not read back exactly");
        Require(read.controls.throttle == 0.123456789f * frame && read.controls.steering == -0.333333343f, "controls are not read back exactly");
        Require(read.controls.gearShifts == (frame == 1 ? 1 : 0) && read.controls.manualGearbox, "the gearbox is not read back");
    }
    std::filesystem::remove(file);
}

void ScenePathsRoundTrip(const std::filesystem::path& folder)
{
    const std::filesystem::path file = folder / "drive_path_tests_scene.yaml";
    SerializedSceneData scene;
    SceneDrivePath path;
    path.name = "lap";
    path.closed = true;
    path.laps = 3;
    path.speedKmh = 80.0f;
    path.points = {{{1.5, 100.0, -2.25}, 0.0f}, {{10.0, 100.5, 30.125}, 120.0f}, {{-5.0, 100.0, 12.0}, 0.0f}};
    scene.drivePaths.push_back(path);
    SaveEditorSceneDataToFile(scene, file.string());
    const SerializedSceneData read = LoadEditorSceneDataFromFile(file.string());
    Require(read.drivePaths.size() == 1 && read.drivePaths[0] == path, "drive paths do not survive a save and load");
    std::filesystem::remove(file);
}
}

int main()
{
    try
    {
        const std::filesystem::path folder = std::filesystem::temp_directory_path();
        SplinePassesThroughItsPoints();
        StraightPathBrakesToItsEnd();
        SpeedCapsAndScale();
        FollowsACircleForItsLaps();
        HoldsACircleWhenTheCarUndersteers();
        FollowsALaneChangeAndStops();
        FailsWhenFarOff();
        StartJustBehindAClosedStartCountsNoLap();
        DriveLogRoundTrips(folder);
        ScenePathsRoundTrip(folder);
    }
    catch (const std::exception& exception)
    {
        std::cerr << "drive path tests failed: " << exception.what() << '\n';
        return 1;
    }
    std::cout << "drive path tests passed\n";
    return 0;
}
