#include <engine/core/threading/task_system.h>
#include <engine/editor/services/scene_raycast.h>
#include <engine/editor/services/vehicle_drive_log.h>
#include <engine/editor/services/vehicle_path_follower.h>
#include <engine/logic/editor_world.h>
#include <engine/renderer/renderer_world.h>

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <chrono>
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

void ReversingKeepsTheCurve()
{
    SceneDrivePath path;
    path.speedKmh = 50.0f;
    path.points = {{{0.0, 0.0, 0.0}, 30.0f}, {{0.0, 0.0, 30.0}}, {{12.0, 1.0, 55.0}, 80.0f}, {{40.0, 2.0, 60.0}}};
    const SceneDrivePath reversed = ReversedDrivePath(path);
    Require(reversed.points.front() == path.points.back() && reversed.points.back() == path.points.front(), "the ends did not swap");
    Require(reversed.points[1].speedKmh == 80.0f, "a point lost its speed");
    Require(ReversedDrivePath(reversed) == path, "reversing twice is not the path");
    const DrivePathTrack forward = BuildDrivePathTrack(path);
    const DrivePathTrack backward = BuildDrivePathTrack(reversed);
    Require(std::abs(forward.length - backward.length) < 0.01, "the reversed curve is another length");
    for (const DrivePathSample& sample : backward.samples)
    {
        const double off = glm::length(ProjectOntoDrivePath(forward, sample.position).closest - sample.position);
        Require(off < 0.01, "the reversed curve is " + std::to_string(off) + " m off the original");
    }
    Require(glm::dot(forward.samples.front().tangent, backward.samples.back().tangent) < -0.999f, "the reversed curve does not end the other way");

    // A closed path keeps its start and goes round the other way.
    const SceneDrivePath circle = Circle(30.0, 12, 40.0f, 1);
    const SceneDrivePath around = ReversedDrivePath(circle);
    Require(around.points.front() == circle.points.front() && around.points[1] == circle.points.back(), "a closed path's start moved");
    const DrivePathTrack circleTrack = BuildDrivePathTrack(around);
    Require(circleTrack.samples[10].curvature < 0.0f, "the reversed circle does not turn left");
}

void LinkLeavesAndArrivesWithoutAKink()
{
    // Two straights heading +Z, the second 40 m across and 50 m on: the link is an S between them.
    SceneDrivePath a;
    a.name = "a";
    a.speedKmh = 60.0f;
    a.points = {{{0.0, 0.0, 0.0}}, {{0.0, 0.0, 50.0}}};
    SceneDrivePath b;
    b.name = "b";
    b.speedKmh = 40.0f;
    b.points = {{{40.0, 0.0, 100.0}}, {{40.0, 0.0, 150.0}}};
    const SceneDrivePath aBefore = a;
    const SceneDrivePath bBefore = b;

    const std::vector<SceneDrivePathPoint> link = DrivePathLinkPoints(a, DrivePathEnd::End, b, DrivePathEnd::Start);
    Require(link.size() >= 3, "the link has " + std::to_string(link.size()) + " points");
    Require(link.front().position == a.points.back().position && link.back().position == b.points.front().position, "the link misses the ends");
    Require(link.front().speedKmh == 60.0f && link.back().speedKmh == 40.0f, "the link's speeds do not run from end to end");
    for (size_t index = 1; index < link.size(); ++index)
    {
        const double gap = glm::length(link[index].position - link[index - 1].position);
        const bool hold = index == 1 || index + 1 == link.size();
        Require(hold ? std::abs(gap - 0.25) < 0.01 : gap > 1.0 && gap <= 2.0 + 1.0e-6, "link points are " + std::to_string(gap) + " m apart");
    }
    SceneDrivePath linkPath;
    linkPath.points = link;
    DrivePathTrackSettings fine;
    fine.sampleSpacing = 0.05f; // tangents from 5 cm chords: the spline's own direction at the ends
    const DrivePathTrack linkTrack = BuildDrivePathTrack(linkPath, fine);
    const float leave = glm::dot(linkTrack.samples.front().tangent, glm::vec3(0.0f, 0.0f, 1.0f));
    const float arrive = glm::dot(linkTrack.samples.back().tangent, glm::vec3(0.0f, 0.0f, 1.0f));
    std::cout << "link: " << link.size() << " points, " << linkTrack.length << " m, leaves " << glm::degrees(std::acos(std::min(leave, 1.0f)))
              << " deg and arrives " << glm::degrees(std::acos(std::min(arrive, 1.0f))) << " deg off the paths\n";
    Require(leave > 0.9999f && arrive > 0.9999f, "the link kinks at an end");

    // Joined: a, the link, then b, with each path's own points and
    // speeds where they were.
    const SceneDrivePath joined = JoinDrivePaths(a, DrivePathEnd::End, b, DrivePathEnd::Start);
    Require(joined.points.size() == a.points.size() + link.size() - 2 + b.points.size(), "the joined path has " + std::to_string(joined.points.size()) + " points");
    Require(joined.points.front().position == a.points.front().position && joined.points.back().position == b.points.back().position, "the joined path's ends are wrong");
    Require(joined.points.front().speedKmh == 60.0f && joined.points.back().speedKmh == 40.0f, "the joined path lost the paths' speeds");
    const DrivePathTrack joinedTrack = BuildDrivePathTrack(joined, fine);
    for (const SceneDrivePathPoint& point : a.points)
    {
        Require(glm::length(ProjectOntoDrivePath(joinedTrack, point.position).closest - point.position) < 0.02, "the joined path misses a point of a");
    }
    // The middle of a and of b: the joined curve stays on the straights.
    for (const glm::dvec3 middle : {glm::dvec3(0.0, 0.0, 25.0), glm::dvec3(40.0, 0.0, 125.0)})
    {
        const double off = glm::length(ProjectOntoDrivePath(joinedTrack, middle).closest - middle);
        Require(off < 0.05, "the joined path leaves a straight by " + std::to_string(off) + " m");
    }
    // Through the joins it runs on along the straights.
    for (const glm::dvec3 join : {a.points.back().position, b.points.front().position})
    {
        const float along = glm::dot(DrivePathTangentAt(joinedTrack, ProjectOntoDrivePath(joinedTrack, join).distance), glm::vec3(0.0f, 0.0f, 1.0f));
        Require(along > 0.9999f, "the joined path turns " + std::to_string(glm::degrees(std::acos(std::min(along, 1.0f)))) + " deg off at a join");
    }
    Require(a == aBefore && b == bBefore, "linking changed a path");

    // b's end to a's start backwards: the same link reversed, b and a driven the other way.
    const SceneDrivePath back = JoinDrivePaths(b, DrivePathEnd::Start, a, DrivePathEnd::End);
    Require(back.points.front().position == b.points.back().position && back.points.back().position == a.points.front().position, "the backward join's ends are wrong");

    // A path's end linked to its own start closes it.
    const SceneDrivePath loop = JoinDrivePaths(a, DrivePathEnd::End, a, DrivePathEnd::Start);
    Require(loop.closed && loop.points.size() > a.points.size(), "a path linked to itself is not a loop");
    Require(DrivePathLinkPoints(a, DrivePathEnd::End, a, DrivePathEnd::End).empty(), "an end linked to itself gave a link");
    Require(JoinDrivePaths(Circle(30.0, 12, 40.0f, 1), DrivePathEnd::End, b, DrivePathEnd::Start).points.empty(), "a closed path was joined");
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
        sample.position = glm::dvec3(10.0 * frame, 100.25, -3.5);
        sample.rotation = glm::normalize(glm::quat(0.9f, 0.01f * frame, 0.4f, -0.02f));
        for (size_t wheel = 0; wheel < sample.wheels.size(); ++wheel)
        {
            DriveLogWheel& logged = sample.wheels[wheel];
            logged.position = sample.position + glm::dvec3(0.8 * static_cast<double>(wheel), -0.3, 1.25);
            logged.rotation = glm::normalize(glm::quat(0.5f, 0.5f, 0.1f * static_cast<float>(wheel + frame), 0.3f));
            logged.inContact = wheel != 3;
            logged.load = 3000.5f + 100.0f * static_cast<float>(wheel);
        }
        writer.Write(sample);
    }
    writer.Close("done");

    const std::string columns = DriveLogColumns();
    const std::string row = FormatDriveLogRow(sample);
    Require(std::count(columns.begin(), columns.end(), ',') == std::count(row.begin(), row.end(), ','), "a row's cells do not match the columns");

    const DriveReplay replay = ReadDriveLog(file);
    Require(replay.samples.size() == 3, "the log reads back " + std::to_string(replay.samples.size()) + " frames");
    Require(replay.header.car == "skyline_r34_vspec" && replay.header.path == "lane change 2", "the header reads back wrong");
    Require(replay.header.startPosition == header.startPosition && replay.header.startRotation == header.startRotation, "the start reads back wrong");
    Require(replay.header.stepSeconds == header.stepSeconds, "the step reads back wrong");
    for (int frame = 0; frame < 3; ++frame)
    {
        const DriveLogSample& read = replay.samples[frame];
        Require(read.deltaSeconds == kFrame * (1.0f + 0.1f * frame) && read.physicsSteps == 16 + frame, "dt is not read back exactly");
        Require(read.controls.throttle == 0.123456789f * frame && read.controls.steering == -0.333333343f, "controls are not read back exactly");
        Require(read.controls.gearShifts == (frame == 1 ? 1 : 0) && read.controls.manualGearbox, "the gearbox is not read back");
        const glm::quat rotation = glm::normalize(glm::quat(0.9f, 0.01f * frame, 0.4f, -0.02f));
        Require(read.rotation == rotation && read.position == glm::dvec3(10.0 * frame, 100.25, -3.5), "the body is not read back exactly");
        for (size_t wheel = 0; wheel < read.wheels.size(); ++wheel)
        {
            const DriveLogWheel& logged = read.wheels[wheel];
            Require(logged.rotation == glm::normalize(glm::quat(0.5f, 0.5f, 0.1f * static_cast<float>(wheel + frame), 0.3f)), "a wheel's rotation reads back wrong");
            Require(glm::length(logged.position - (read.position + glm::dvec3(0.8 * static_cast<double>(wheel), -0.3, 1.25))) < 1e-4, "a wheel's place reads back wrong");
            Require(logged.inContact == (wheel != 3) && logged.load == 3000.5f + 100.0f * static_cast<float>(wheel), "a wheel's contact reads back wrong");
        }
    }

    // Played back between two frames, the body is between them; before the first and after the last, at them.
    size_t cursor = 0;
    const double between = 0.25 * replay.samples[0].time + 0.75 * replay.samples[1].time;
    const DriveLogSample played = SampleDriveAt(replay.samples, between, cursor);
    Require(cursor == 0 && std::abs(played.position.x - 7.5) < 1e-9, "a quarter from the second frame, x = " + std::to_string(played.position.x));
    Require(SampleDriveAt(replay.samples, -1.0, cursor).position == replay.samples.front().position, "before the drive, its first frame");
    Require(SampleDriveAt(replay.samples, 99.0, cursor).position == replay.samples.back().position && cursor == 2, "after it, its last");
    Require(SampleDriveAt(replay.samples, 0.0, cursor).position == replay.samples.front().position && cursor == 0, "and back to the start");
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

// A 20 m square in the entity's XZ plane at its origin, as a render submesh of `entity`.
CpuRenderSubmesh Square(entt::entity entity)
{
    auto mesh = std::make_shared<MeshData>();
    for (const auto& [x, z] : {std::pair{-10.0f, -10.0f}, std::pair{10.0f, -10.0f}, std::pair{10.0f, 10.0f}, std::pair{-10.0f, 10.0f}})
    {
        Vertex vertex{};
        vertex.position[0] = x;
        vertex.position[2] = z;
        mesh->vertices.push_back(vertex);
    }
    mesh->indices = {0, 1, 2, 0, 2, 3};
    CpuRenderSubmesh submesh;
    submesh.entity = entity;
    submesh.mesh = mesh;
    submesh.localBoundsRadius = 15.0f;
    return submesh;
}

entt::entity PlaceEntity(IEditorWorld& world, const char* name, glm::vec3 translation, glm::vec3 scale = glm::vec3(1.0f))
{
    SerializedEntityData data{};
    data.tagName = name;
    data.transform.translation = translation;
    data.transform.scale = scale;
    return world.CreateEntity(data);
}

// A click puts a drive path's point on what it is on: the ground far from the origin, a lifted floor over
// it, not a decal, the top of water or the driven car, and nothing past the scene.
void RayFindsTheGround()
{
    std::unique_ptr<IEditorWorld> world = CreateEditorWorld();
    RendererWorld renderWorld;
    renderWorld.SetSceneWorld(*world);
    // The ground at y = 100 out at x = 6000, scaled up 10 times (200 m across).
    const entt::entity ground = PlaceEntity(*world, "Ground", glm::vec3(6000.0f, 100.0f, 0.0f), glm::vec3(10.0f));
    const entt::entity decal = PlaceEntity(*world, "Decal", glm::vec3(6000.0f, 100.05f, 0.0f));
    const entt::entity water = PlaceEntity(*world, "Water", glm::vec3(6000.0f, 100.5f, 0.0f));
    const entt::entity car = PlaceEntity(*world, "Car", glm::vec3(6000.0f, 101.5f, 0.0f));
    const entt::entity deck = PlaceEntity(*world, "Deck", glm::vec3(6050.0f, 110.0f, 0.0f));
    std::vector<CpuRenderSubmesh> submeshes = {Square(ground), Square(decal), Square(water), Square(car), Square(deck)};
    submeshes[1].decal = true;
    submeshes[2].water = true;
    renderWorld.SetRenderSubmeshes(std::move(submeshes));

    // Slanting down from a camera up and to the side, at a point near the middle.
    const glm::dvec3 eye(5980.0, 140.0, -30.0);
    const glm::dvec3 target(6003.0, 100.0, 4.0);
    std::optional<SceneRayHit> hit = RaycastScene(renderWorld, *world, eye, target - eye, 20000.0, car);
    Require(hit.has_value() && hit->entity == ground, "the ray falls through the decal, the water and the driven car to the ground");
    Require(glm::length(hit->position - target) < 1e-6, "on the ground where it was clicked, off by " + std::to_string(glm::length(hit->position - target)));
    hit = RaycastScene(renderWorld, *world, eye, target - eye, 20000.0);
    Require(hit.has_value() && hit->entity == car && std::abs(hit->position.y - 101.5) < 1e-6, "a car not being driven is clicked on");
    hit = RaycastScene(renderWorld, *world, glm::dvec3(6052.0, 150.0, 3.0), glm::dvec3(0.0, -1.0, 0.0), 20000.0, car);
    Require(hit.has_value() && hit->entity == deck && std::abs(hit->position.y - 110.0) < 1e-6, "the deck over the ground is the first thing met");
    Require(!RaycastScene(renderWorld, *world, eye, glm::dvec3(0.0, 1.0, 0.0), 20000.0, car).has_value(), "the sky meets nothing");
    Require(!RaycastScene(renderWorld, *world, glm::dvec3(6400.0, 150.0, 0.0), glm::dvec3(0.0, -1.0, 0.0), 20000.0, car).has_value(),
            "past the ground's edge, nothing");
}

// A click on a big map's ground (2 million triangles in one mesh) finds it on one thread and on all
// the workers alike; prints how long each took.
void RayCostOnABigGround()
{
    constexpr int kCells = 1000;
    auto mesh = std::make_shared<MeshData>();
    mesh->vertices.reserve((kCells + 1) * (kCells + 1));
    for (int z = 0; z <= kCells; ++z)
    {
        for (int x = 0; x <= kCells; ++x)
        {
            Vertex vertex{};
            vertex.position[0] = static_cast<float>(x) - kCells * 0.5f;
            vertex.position[1] = 0.25f * std::sin(0.1f * static_cast<float>(x + z));
            vertex.position[2] = static_cast<float>(z) - kCells * 0.5f;
            mesh->vertices.push_back(vertex);
        }
    }
    for (uint32_t z = 0; z < kCells; ++z)
    {
        for (uint32_t x = 0; x < kCells; ++x)
        {
            const uint32_t a = z * (kCells + 1) + x;
            const uint32_t b = a + 1;
            const uint32_t c = a + kCells + 1;
            const uint32_t d = c + 1;
            mesh->indices.insert(mesh->indices.end(), {a, c, b, b, c, d});
        }
    }
    std::unique_ptr<IEditorWorld> world = CreateEditorWorld();
    RendererWorld renderWorld;
    renderWorld.SetSceneWorld(*world);
    const entt::entity ground = PlaceEntity(*world, "Ground", glm::vec3(0.0f, 100.0f, 0.0f));
    CpuRenderSubmesh submesh;
    submesh.entity = ground;
    submesh.mesh = mesh;
    submesh.localBoundsRadius = kCells * 0.75f;
    renderWorld.SetRenderSubmeshes({submesh});

    const glm::dvec3 eye(-300.0, 160.0, -250.0);
    const glm::dvec3 direction = glm::dvec3(120.0, 100.0, 90.0) - eye;
    const auto cast = [&](const char* how)
    {
        const auto start = std::chrono::steady_clock::now();
        const std::optional<SceneRayHit> hit = RaycastScene(renderWorld, *world, eye, direction, 20000.0);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::cout << "a click on 2 million triangles, " << how << ": " << ms << " ms\n";
        Require(hit.has_value() && std::abs(hit->position.y - 100.0) < 0.3, std::string("the big ground is found ") + how);
        return hit->position;
    };
    const glm::dvec3 alone = cast("one thread");
    TaskSystem::Initialize();
    const glm::dvec3 shared = cast("all workers");
    TaskSystem::Shutdown();
    Require(alone == shared, "the workers find the same point");
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
        ReversingKeepsTheCurve();
        LinkLeavesAndArrivesWithoutAKink();
        DriveLogRoundTrips(folder);
        ScenePathsRoundTrip(folder);
        RayFindsTheGround();
        RayCostOnABigGround();
    }
    catch (const std::exception& exception)
    {
        std::cerr << "drive path tests failed: " << exception.what() << '\n';
        return 1;
    }
    std::cout << "drive path tests passed\n";
    return 0;
}
