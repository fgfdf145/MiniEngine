#pragma once

#include <engine/physics/vehicle_settings.h>
#include <engine/scene/scene_drive_path.h>

#include <glm/glm.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace me
{

// How a drive path is sampled and the speeds along it planned
// (docs/design/2026-10-09-drive-path-follow-design.md).
struct DrivePathTrackSettings
{
    // Metres between the samples along the curve.
    float sampleSpacing = 0.5f;
    // Every target speed times this, to sweep a test through speeds.
    float speedScale = 1.0f;
    // Above 0: the speed in a corner is held to what this much lateral acceleration (g) allows, so a
    // speed set too high for a bend does not throw the car off. 0 leaves the speeds as set.
    float lateralGrip = 0.0f;
    // The plan brakes at this deceleration (g) before a slower stretch, so the car arrives at a point at
    // its speed rather than braking once it is there.
    float brakingDecel = 0.8f;
    // Above 0: the plan speeds up no quicker than this (g); 0 leaves it to the car.
    float accelLimit = 0.0f;
};

// A point on the sampled curve.
struct DrivePathSample
{
    glm::dvec3 position{0.0};
    // Along the curve, unit length.
    glm::vec3 tangent{0.0f, 0.0f, 1.0f};
    // Arc length from the first point (m).
    double distance = 0.0;
    // How sharply the curve turns across the ground (1/m), positive to the right.
    float curvature = 0.0f;
    // The planned speed (m/s).
    float speed = 0.0f;
};

// A drive path sampled at even spacing with its speed plan: what the follower steers along. A closed
// track's last sample joins back to its first, and `length` includes that last stretch.
struct DrivePathTrack
{
    std::string name;
    std::vector<DrivePathSample> samples;
    double length = 0.0;
    bool closed = false;
    int laps = 1;

    bool Empty() const
    {
        return samples.size() < 2 || length <= 0.0;
    }
};

// Samples the path's centripetal Catmull-Rom spline (through every point) and plans its speeds. Points
// on top of the one before are dropped; a closed path needs three points, else it is followed open. An
// open path's speed comes down to 0 at its end. Empty with fewer than two distinct points.
DrivePathTrack BuildDrivePathTrack(const SceneDrivePath& path, const DrivePathTrackSettings& settings = {});

// Where a point is against the track.
struct DrivePathProjection
{
    // The segment from samples[segment] to the next one the closest point is on.
    size_t segment = 0;
    // The closest point's arc length (m), from 0 to the track's length.
    double distance = 0.0;
    glm::dvec3 closest{0.0};
    // Across the ground from the closest point: positive when the point is to the right of the track.
    float lateralError = 0.0f;
};

// The closest point on the track to `position`. With `searchMetres` above 0 only the stretch that far
// either side of `hintSegment` is searched (the car moves little in a frame); 0 searches the lot.
DrivePathProjection ProjectOntoDrivePath(const DrivePathTrack& track, const glm::dvec3& position, size_t hintSegment = 0, double searchMetres = 0.0);

// The point `distance` metres along the track. A closed track wraps round; an open one carries on
// straight past either end.
glm::dvec3 DrivePathPointAt(const DrivePathTrack& track, double distance);
// The planned speed there (m/s): 0 past an open track's end.
float DrivePathSpeedAt(const DrivePathTrack& track, double distance);
// The tangent there.
glm::vec3 DrivePathTangentAt(const DrivePathTrack& track, double distance);

// How the follower drives: pure pursuit for the steering, a PI controller with the plan's deceleration
// fed forward for the speed.
struct PathFollowerSettings
{
    // The steering aims at the point this far ahead along the path: lookaheadBase plus lookaheadSeconds of
    // travel, between lookaheadMin and lookaheadMax (m).
    float lookaheadBase = 3.0f;
    float lookaheadSeconds = 0.8f;
    float lookaheadMin = 4.0f;
    float lookaheadMax = 40.0f;
    // The fastest the steering moves, in full locks per second, as a driver's hands would.
    float steerRate = 2.5f;
    // The speed aimed for is the plan's this far ahead in time, so the controller reacts a little early.
    float speedPreviewSeconds = 0.3f;
    // Throttle per m/s under the target speed, and the integral's gain (per metre: m/s times seconds).
    float throttleGain = 0.25f;
    float throttleIntegralGain = 0.1f;
    // Brake per m/s over the target, once it is over by brakeDeadband (m/s).
    float brakeGain = 0.15f;
    float brakeDeadband = 0.5f;
    // The deceleration (g) full brake is taken to give, for the plan's braking fed forward.
    float fullBrakeDecel = 1.0f;
    // The test fails when the car is this far off the path (m), or stuck under 0.3 m/s this long (s)
    // where it is meant to move.
    float abortDistance = 8.0f;
    float stuckSeconds = 10.0f;
    // An open path is done once the car is within a metre of its end and slower than this (m/s).
    float finishSpeed = 0.5f;
};

// The car, this frame.
struct PathFollowerInput
{
    // The body's origin and its forward and up axes in world space.
    glm::dvec3 position{0.0};
    glm::vec3 forward{0.0f, 0.0f, 1.0f};
    glm::vec3 up{0.0f, 1.0f, 0.0f};
    // Along the body's forward axis (m/s).
    float forwardSpeed = 0.0f;
    float wheelbase = 2.6f;
    // The rear axle along the body's forward axis from its origin (m, negative behind): the point pure
    // pursuit steers from.
    float rearAxleOffset = -1.3f;
    // The front wheels' full lock (degrees).
    float maxSteerDegrees = 35.0f;
};

enum class PathFollowerStatus
{
    Running,
    Finished,
    Failed,
};

// Carried from frame to frame; a new one starts the path again.
struct PathFollowerState
{
    bool started = false;
    size_t segment = 0;
    // Along the track on this lap (m), and the laps done.
    double distance = 0.0;
    int lap = 0;
    float steering = 0.0f;
    float speedIntegral = 0.0f;
    float stuckSeconds = 0.0f;
    PathFollowerStatus status = PathFollowerStatus::Running;
    std::string failure;
};

// What the follower saw this frame, for the log and the viewport.
struct PathFollowerOutput
{
    double distance = 0.0;
    int lap = 0;
    float lateralError = 0.0f;
    // The car's heading against the path's (degrees, positive when the car points right of it).
    float headingErrorDegrees = 0.0f;
    float targetSpeed = 0.0f;
    glm::dvec3 closest{0.0};
    glm::dvec3 lookahead{0.0};
};

// The controls that keep the car on the track at its planned speed, -1 to 1 steering (right positive),
// throttle 0 to 1 and brake 0 to 1 (never reverse). Once the state is Finished or Failed the car is
// braked to a stop. `output` (optional) takes what the follower saw.
VehicleControls ComputePathFollowControls(
    const DrivePathTrack& track,
    const PathFollowerSettings& settings,
    const PathFollowerInput& input,
    PathFollowerState& state,
    float deltaSeconds,
    PathFollowerOutput* output = nullptr);

const char* PathFollowerStatusName(PathFollowerStatus status);
}
