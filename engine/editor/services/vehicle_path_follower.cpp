#include "vehicle_path_follower.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace me
{
namespace
{
constexpr float kGravity = 9.81f;
// Points closer than this to the one before are the same point.
constexpr double kSamePointMetres = 0.01;
// The follower looks for the car this far either side of where it was in the last frame.
constexpr double kFollowSearchMetres = 50.0;

// The centripetal Catmull-Rom spline (alpha 0.5) from p1 to p2 at `u` (0 to 1), by Barry and Goldman's
// pyramid: it passes through p1 and p2, and unlike the uniform spline never loops or overshoots between
// points spaced unevenly.
glm::dvec3 CentripetalCatmullRom(const glm::dvec3& p0, const glm::dvec3& p1, const glm::dvec3& p2, const glm::dvec3& p3, double u)
{
    const auto knot = [](double previous, const glm::dvec3& a, const glm::dvec3& b)
    {
        return previous + std::max(std::sqrt(glm::length(b - a)), 1.0e-6);
    };
    const double t0 = 0.0;
    const double t1 = knot(t0, p0, p1);
    const double t2 = knot(t1, p1, p2);
    const double t3 = knot(t2, p2, p3);
    const double t = t1 + (t2 - t1) * u;
    const glm::dvec3 a1 = ((t1 - t) * p0 + (t - t0) * p1) / (t1 - t0);
    const glm::dvec3 a2 = ((t2 - t) * p1 + (t - t1) * p2) / (t2 - t1);
    const glm::dvec3 a3 = ((t3 - t) * p2 + (t - t2) * p3) / (t3 - t2);
    const glm::dvec3 b1 = ((t2 - t) * a1 + (t - t0) * a2) / (t2 - t0);
    const glm::dvec3 b2 = ((t3 - t) * a2 + (t - t1) * a3) / (t3 - t1);
    return ((t2 - t) * b1 + (t - t1) * b2) / (t2 - t1);
}

// Across the ground, to the right of `tangent` (unit length; zero for a vertical tangent).
glm::dvec3 GroundRight(const glm::vec3& tangent)
{
    const glm::dvec3 right(-tangent.z, 0.0, tangent.x);
    const double length = glm::length(right);
    return length > 1.0e-6 ? right / length : glm::dvec3(0.0);
}

float HeadingRadians(const glm::vec3& direction)
{
    return std::atan2(direction.x, direction.z);
}

float WrapRadians(float angle)
{
    return std::remainder(angle, 2.0f * glm::pi<float>());
}

size_t SegmentCount(const DrivePathTrack& track)
{
    return track.closed ? track.samples.size() : track.samples.size() - 1;
}

double SegmentLength(const DrivePathTrack& track, size_t segment)
{
    const size_t next = segment + 1;
    if (next < track.samples.size())
    {
        return track.samples[next].distance - track.samples[segment].distance;
    }
    return track.length - track.samples[segment].distance;
}

// The segment `distance` (0 to the length) is on, and how far along it (0 to 1).
size_t FindSegment(const DrivePathTrack& track, double distance, double& along)
{
    const auto it = std::upper_bound(
        track.samples.begin(), track.samples.end(), distance, [](double value, const DrivePathSample& sample)
        {
            return value < sample.distance;
        });
    size_t segment = it == track.samples.begin() ? 0 : static_cast<size_t>(it - track.samples.begin()) - 1;
    segment = std::min(segment, SegmentCount(track) - 1);
    const double length = SegmentLength(track, segment);
    along = length > 0.0 ? std::clamp((distance - track.samples[segment].distance) / length, 0.0, 1.0) : 0.0;
    return segment;
}

const DrivePathSample& SegmentEnd(const DrivePathTrack& track, size_t segment)
{
    return track.samples[(segment + 1) % track.samples.size()];
}

double WrapDistance(const DrivePathTrack& track, double distance)
{
    double wrapped = std::fmod(distance, track.length);
    if (wrapped < 0.0)
    {
        wrapped += track.length;
    }
    return wrapped;
}
}

DrivePathTrack BuildDrivePathTrack(const SceneDrivePath& path, const DrivePathTrackSettings& settings)
{
    DrivePathTrack track;
    track.name = path.name;
    track.laps = std::max(path.laps, 1);

    // The points, less any on top of the one before (and a closed path's last on its first), with their speeds.
    std::vector<glm::dvec3> points;
    std::vector<float> speeds;
    for (const SceneDrivePathPoint& point : path.points)
    {
        if (!points.empty() && glm::length(point.position - points.back()) < kSamePointMetres)
        {
            continue;
        }
        points.push_back(point.position);
        const float kmh = point.speedKmh > 0.0f ? point.speedKmh : path.speedKmh;
        speeds.push_back(std::max(kmh, 0.0f) / 3.6f * std::max(settings.speedScale, 0.0f));
    }
    if (path.closed && points.size() > 1 && glm::length(points.front() - points.back()) < kSamePointMetres)
    {
        points.pop_back();
        speeds.pop_back();
    }
    const size_t pointCount = points.size();
    if (pointCount < 2)
    {
        return track;
    }
    track.closed = path.closed && pointCount >= 3;
    const size_t segmentCount = track.closed ? pointCount : pointCount - 1;
    const double spacing = std::max(static_cast<double>(settings.sampleSpacing), 0.05);

    // The curve finely, with the arc length at each point it passes through.
    std::vector<glm::dvec3> dense;
    std::vector<double> denseDistance;
    std::vector<double> pointDistance(pointCount + 1, 0.0);
    const auto addDense = [&](const glm::dvec3& position)
    {
        const double distance = dense.empty() ? 0.0 : denseDistance.back() + glm::length(position - dense.back());
        dense.push_back(position);
        denseDistance.push_back(distance);
    };
    for (size_t segment = 0; segment < segmentCount; ++segment)
    {
        const glm::dvec3& p1 = points[segment];
        const glm::dvec3& p2 = points[(segment + 1) % pointCount];
        glm::dvec3 p0;
        glm::dvec3 p3;
        if (track.closed)
        {
            p0 = points[(segment + pointCount - 1) % pointCount];
            p3 = points[(segment + 2) % pointCount];
        }
        else
        {
            // Past the ends the curve carries on as the mirror image of its first and last stretch.
            p0 = segment > 0 ? points[segment - 1] : 2.0 * p1 - p2;
            p3 = segment + 2 < pointCount ? points[segment + 2] : 2.0 * p2 - p1;
        }
        const int steps = std::max(4, static_cast<int>(std::ceil(glm::length(p2 - p1) / (spacing * 0.25))));
        for (int step = 0; step < steps; ++step)
        {
            addDense(step == 0 ? p1 : CentripetalCatmullRom(p0, p1, p2, p3, static_cast<double>(step) / steps));
            if (step == 0)
            {
                pointDistance[segment] = denseDistance.back();
            }
        }
    }
    addDense(track.closed ? points.front() : points.back());
    pointDistance[segmentCount] = denseDistance.back();
    track.length = denseDistance.back();
    if (track.length <= 0.0)
    {
        return DrivePathTrack{};
    }

    // Even spacing along it. An open track keeps a sample on its end; a closed one's end is its start.
    const size_t intervals = std::max<size_t>(1, static_cast<size_t>(std::llround(track.length / spacing)));
    const double step = track.length / static_cast<double>(intervals);
    const size_t sampleCount = track.closed ? intervals : intervals + 1;
    track.samples.resize(sampleCount);
    size_t denseIndex = 0;
    for (size_t index = 0; index < sampleCount; ++index)
    {
        const double distance = std::min(static_cast<double>(index) * step, track.length);
        while (denseIndex + 2 < dense.size() && denseDistance[denseIndex + 1] < distance)
        {
            ++denseIndex;
        }
        const double span = denseDistance[denseIndex + 1] - denseDistance[denseIndex];
        const double along = span > 0.0 ? std::clamp((distance - denseDistance[denseIndex]) / span, 0.0, 1.0) : 0.0;
        track.samples[index].position = glm::mix(dense[denseIndex], dense[denseIndex + 1], along);
        track.samples[index].distance = distance;
    }

    // Tangents from the neighbours, one-sided at an open track's ends.
    for (size_t index = 0; index < sampleCount; ++index)
    {
        size_t before = index;
        size_t after = index;
        if (track.closed)
        {
            before = (index + sampleCount - 1) % sampleCount;
            after = (index + 1) % sampleCount;
        }
        else
        {
            before = index > 0 ? index - 1 : index;
            after = index + 1 < sampleCount ? index + 1 : index;
        }
        const glm::dvec3 direction = track.samples[after].position - track.samples[before].position;
        const double length = glm::length(direction);
        track.samples[index].tangent = length > 1.0e-9 ? glm::vec3(direction / length) : glm::vec3(0.0f, 0.0f, 1.0f);
    }

    // Curvature across the ground over about two metres either side: the heading's change per metre.
    const size_t reach = std::max<size_t>(1, static_cast<size_t>(std::llround(2.0 / step)));
    for (size_t index = 0; index < sampleCount; ++index)
    {
        size_t before = 0;
        size_t after = 0;
        double span = 0.0;
        if (track.closed)
        {
            const size_t window = std::min(reach, (sampleCount - 1) / 2);
            if (window == 0)
            {
                continue;
            }
            before = (index + sampleCount - window) % sampleCount;
            after = (index + window) % sampleCount;
            span = 2.0 * static_cast<double>(window) * step;
        }
        else
        {
            before = index >= reach ? index - reach : 0;
            after = std::min(index + reach, sampleCount - 1);
            span = track.samples[after].distance - track.samples[before].distance;
        }
        const glm::vec3& a = track.samples[before].tangent;
        const glm::vec3& b = track.samples[after].tangent;
        if (span <= 0.0 || glm::length(glm::vec2(a.x, a.z)) < 1.0e-3f || glm::length(glm::vec2(b.x, b.z)) < 1.0e-3f)
        {
            continue;
        }
        // Turning right lowers the heading (atan2(x, z)): +X is to the left of +Z.
        track.samples[index].curvature = -WrapRadians(HeadingRadians(b) - HeadingRadians(a)) / static_cast<float>(span);
    }

    // The speeds set at the points, along the arc length between them.
    for (DrivePathSample& sample : track.samples)
    {
        size_t segment = 0;
        while (segment + 1 < segmentCount && pointDistance[segment + 1] <= sample.distance)
        {
            ++segment;
        }
        const double start = pointDistance[segment];
        const double end = pointDistance[segment + 1];
        const float along = end > start ? static_cast<float>(std::clamp((sample.distance - start) / (end - start), 0.0, 1.0)) : 0.0f;
        sample.speed = glm::mix(speeds[segment], speeds[(segment + 1) % pointCount], along);
        if (settings.lateralGrip > 0.0f && std::abs(sample.curvature) > 1.0e-4f)
        {
            sample.speed = std::min(sample.speed, std::sqrt(settings.lateralGrip * kGravity / std::abs(sample.curvature)));
        }
    }
    if (!track.closed)
    {
        track.samples.back().speed = 0.0f;
    }

    // Braking before slower stretches (from the end back), and with an acceleration limit, speeding up
    // no quicker than it allows (from the start on; an open track starts from rest). A closed track goes
    // round twice so its end and start agree.
    const size_t passes = track.closed ? 2 * sampleCount : sampleCount - 1;
    if (settings.brakingDecel > 0.0f)
    {
        const double twiceDecel = 2.0 * settings.brakingDecel * kGravity;
        for (size_t pass = 0; pass < passes; ++pass)
        {
            const size_t index = (sampleCount - 1 - pass % sampleCount + sampleCount - 1) % sampleCount;
            const size_t next = (index + 1) % sampleCount;
            if (!track.closed && index + 1 >= sampleCount)
            {
                continue;
            }
            const double gap = track.closed ? SegmentLength(track, index) : track.samples[next].distance - track.samples[index].distance;
            const float reachable = static_cast<float>(std::sqrt(static_cast<double>(track.samples[next].speed) * track.samples[next].speed + twiceDecel * gap));
            track.samples[index].speed = std::min(track.samples[index].speed, reachable);
        }
    }
    if (settings.accelLimit > 0.0f)
    {
        if (!track.closed)
        {
            track.samples.front().speed = 0.0f;
        }
        const double twiceAccel = 2.0 * settings.accelLimit * kGravity;
        for (size_t pass = 0; pass < passes; ++pass)
        {
            const size_t index = pass % sampleCount;
            const size_t next = (index + 1) % sampleCount;
            if (!track.closed && index + 1 >= sampleCount)
            {
                continue;
            }
            const double gap = SegmentLength(track, index);
            const float reachable = static_cast<float>(std::sqrt(static_cast<double>(track.samples[index].speed) * track.samples[index].speed + twiceAccel * gap));
            track.samples[next].speed = std::min(track.samples[next].speed, reachable);
        }
    }
    return track;
}

DrivePathProjection ProjectOntoDrivePath(const DrivePathTrack& track, const glm::dvec3& position, size_t hintSegment, double searchMetres)
{
    DrivePathProjection best;
    if (track.Empty())
    {
        return best;
    }
    const size_t segments = SegmentCount(track);
    hintSegment = std::min(hintSegment, segments - 1);
    size_t first = 0;
    size_t count = segments;
    if (searchMetres > 0.0)
    {
        const double spacing = track.length / static_cast<double>(segments);
        const size_t reach = static_cast<size_t>(std::ceil(searchMetres / spacing));
        if (2 * reach + 1 < segments)
        {
            count = 2 * reach + 1;
            if (track.closed)
            {
                first = (hintSegment + segments - reach) % segments;
            }
            else
            {
                first = hintSegment > reach ? hintSegment - reach : 0;
                count = std::min(count, segments - first);
            }
        }
    }

    double bestDistanceSquared = std::numeric_limits<double>::max();
    for (size_t offset = 0; offset < count; ++offset)
    {
        const size_t segment = (first + offset) % segments;
        const glm::dvec3& a = track.samples[segment].position;
        const glm::dvec3& b = SegmentEnd(track, segment).position;
        const glm::dvec3 ab = b - a;
        const double lengthSquared = glm::dot(ab, ab);
        const double along = lengthSquared > 0.0 ? std::clamp(glm::dot(position - a, ab) / lengthSquared, 0.0, 1.0) : 0.0;
        const glm::dvec3 closest = a + ab * along;
        const glm::dvec3 offsetVector = position - closest;
        const double distanceSquared = glm::dot(offsetVector, offsetVector);
        if (distanceSquared < bestDistanceSquared)
        {
            bestDistanceSquared = distanceSquared;
            best.segment = segment;
            best.closest = closest;
            best.distance = track.samples[segment].distance + along * SegmentLength(track, segment);
        }
    }
    const glm::vec3 tangent = DrivePathTangentAt(track, best.distance);
    best.lateralError = static_cast<float>(glm::dot(position - best.closest, GroundRight(tangent)));
    return best;
}

glm::dvec3 DrivePathPointAt(const DrivePathTrack& track, double distance)
{
    if (track.Empty())
    {
        return track.samples.empty() ? glm::dvec3(0.0) : track.samples.front().position;
    }
    if (track.closed)
    {
        distance = WrapDistance(track, distance);
    }
    else if (distance <= 0.0)
    {
        return track.samples.front().position + glm::dvec3(track.samples.front().tangent) * distance;
    }
    else if (distance >= track.length)
    {
        return track.samples.back().position + glm::dvec3(track.samples.back().tangent) * (distance - track.length);
    }
    double along = 0.0;
    const size_t segment = FindSegment(track, distance, along);
    return glm::mix(track.samples[segment].position, SegmentEnd(track, segment).position, along);
}

float DrivePathSpeedAt(const DrivePathTrack& track, double distance)
{
    if (track.Empty())
    {
        return 0.0f;
    }
    if (track.closed)
    {
        distance = WrapDistance(track, distance);
    }
    else if (distance >= track.length)
    {
        return 0.0f;
    }
    else if (distance <= 0.0)
    {
        return track.samples.front().speed;
    }
    double along = 0.0;
    const size_t segment = FindSegment(track, distance, along);
    return glm::mix(track.samples[segment].speed, SegmentEnd(track, segment).speed, static_cast<float>(along));
}

glm::vec3 DrivePathTangentAt(const DrivePathTrack& track, double distance)
{
    if (track.Empty())
    {
        return glm::vec3(0.0f, 0.0f, 1.0f);
    }
    if (track.closed)
    {
        distance = WrapDistance(track, distance);
    }
    else if (distance <= 0.0)
    {
        return track.samples.front().tangent;
    }
    else if (distance >= track.length)
    {
        return track.samples.back().tangent;
    }
    double along = 0.0;
    const size_t segment = FindSegment(track, distance, along);
    const glm::vec3 tangent = glm::mix(track.samples[segment].tangent, SegmentEnd(track, segment).tangent, static_cast<float>(along));
    const float length = glm::length(tangent);
    return length > 1.0e-6f ? tangent / length : track.samples[segment].tangent;
}

VehicleControls ComputePathFollowControls(
    const DrivePathTrack& track,
    const PathFollowerSettings& settings,
    const PathFollowerInput& input,
    PathFollowerState& state,
    float deltaSeconds,
    PathFollowerOutput* output)
{
    VehicleControls controls;
    controls.steering = state.steering;
    controls.brake = 1.0f;
    const auto fail = [&](std::string reason)
    {
        state.status = PathFollowerStatus::Failed;
        state.failure = std::move(reason);
    };
    if (track.Empty())
    {
        if (state.status == PathFollowerStatus::Running)
        {
            fail("the path has fewer than two points");
        }
        return controls;
    }

    // Where the car is along the path; a closed path counts a lap each time the car passes its start.
    const DrivePathProjection projection =
        ProjectOntoDrivePath(track, input.position, state.segment, state.started ? kFollowSearchMetres : 0.0);
    if (!state.started && track.closed && projection.distance > track.length * 0.5)
    {
        state.lap = -1; // just short of the start: crossing it begins the first lap
    }
    if (state.started && track.closed)
    {
        if (projection.distance < state.distance - track.length * 0.5)
        {
            ++state.lap;
        }
        else if (projection.distance > state.distance + track.length * 0.5)
        {
            --state.lap;
        }
    }
    state.started = true;
    state.segment = projection.segment;
    state.distance = projection.distance;

    const float speed = input.forwardSpeed;
    const glm::vec3 tangent = DrivePathTangentAt(track, state.distance);
    const glm::vec3 forwardGround = glm::vec3(input.forward.x, 0.0f, input.forward.z);
    const glm::vec3 pathRight = glm::vec3(GroundRight(tangent));
    const float headingError = std::atan2(glm::dot(forwardGround, pathRight), glm::dot(forwardGround, glm::vec3(tangent.x, 0.0f, tangent.z)));

    // Pure pursuit: the arc from the rear axle through the point a lookahead ahead along the path.
    const float lookahead = std::clamp(settings.lookaheadBase + settings.lookaheadSeconds * std::abs(speed), settings.lookaheadMin, settings.lookaheadMax);
    const glm::dvec3 target = DrivePathPointAt(track, state.distance + lookahead);
    const glm::dvec3 rearAxle = input.position + glm::dvec3(input.forward) * static_cast<double>(input.rearAxleOffset);
    const glm::vec3 toTarget = glm::vec3(target - rearAxle);
    const glm::vec3 bodyRight = glm::cross(input.forward, input.up);
    const float across = glm::dot(toTarget, bodyRight);
    const float ahead = glm::dot(toTarget, input.forward);
    const float reach = std::max(std::sqrt(across * across + ahead * ahead), 0.1f);
    const float alpha = std::atan2(across, ahead);
    // And the lateral error's integral against understeer (oversteer), held while the car crawls.
    if (std::abs(speed) > 2.0f && state.status == PathFollowerStatus::Running)
    {
        const float limit = glm::radians(settings.lateralIntegralLimitDegrees) / std::max(settings.lateralIntegralGain, 1.0e-6f);
        state.lateralIntegral = std::clamp(state.lateralIntegral + projection.lateralError * deltaSeconds, -limit, limit);
    }
    // To the right of the path, the car steers left.
    const float wheelAngle =
        std::atan(2.0f * input.wheelbase * std::sin(alpha) / reach) - settings.lateralIntegralGain * state.lateralIntegral;
    const float wantedSteering = std::clamp(glm::degrees(wheelAngle) / std::max(input.maxSteerDegrees, 1.0f), -1.0f, 1.0f);
    const float maxSteerChange = settings.steerRate * std::max(deltaSeconds, 0.0f);
    state.steering += std::clamp(wantedSteering - state.steering, -maxSteerChange, maxSteerChange);

    // The speed the plan has a moment ahead, and how hard it brakes there.
    const double preview = state.distance + std::max(speed, 0.0f) * settings.speedPreviewSeconds;
    float targetSpeed = DrivePathSpeedAt(track, preview);
    const float nextSpeed = DrivePathSpeedAt(track, preview + 1.0);
    const float planAccel = (nextSpeed * nextSpeed - targetSpeed * targetSpeed) * 0.5f;
    const bool lastLapDone = track.closed && state.lap >= track.laps;
    const bool pastEnd = !track.closed && state.distance >= track.length - 1.0;
    if (pastEnd)
    {
        targetSpeed = 0.0f;
    }

    if (output != nullptr)
    {
        output->distance = state.distance;
        output->lap = state.lap;
        output->lateralError = projection.lateralError;
        output->headingErrorDegrees = glm::degrees(headingError);
        output->targetSpeed = targetSpeed;
        output->closest = projection.closest;
        output->lookahead = target;
    }

    if (state.status == PathFollowerStatus::Running)
    {
        if (input.up.y < 0.3f)
        {
            fail("the car rolled over");
        }
        else if (std::abs(projection.lateralError) > settings.abortDistance)
        {
            char reason[96];
            std::snprintf(reason, sizeof(reason), "the car left the path by %.1f m at %.0f m", projection.lateralError, state.distance);
            fail(reason);
        }
        else if (lastLapDone || (pastEnd && std::abs(speed) < settings.finishSpeed))
        {
            state.status = PathFollowerStatus::Finished;
        }
        else
        {
            state.stuckSeconds = targetSpeed > 1.0f && std::abs(speed) < 0.3f ? state.stuckSeconds + deltaSeconds : 0.0f;
            if (state.stuckSeconds > settings.stuckSeconds)
            {
                char reason[64];
                std::snprintf(reason, sizeof(reason), "the car got stuck at %.0f m", state.distance);
                fail(reason);
            }
        }
    }
    controls.steering = state.steering;
    if (state.status != PathFollowerStatus::Running)
    {
        state.speedIntegral = 0.0f;
        return controls;
    }

    // Throttle on a PI of the speed error, or brake on the plan's deceleration and the error once over it.
    const float error = targetSpeed - speed;
    float brake = settings.brakeGain * std::max(-error - settings.brakeDeadband, 0.0f);
    if (planAccel < 0.0f && error < 1.0f)
    {
        brake += -planAccel / (std::max(settings.fullBrakeDecel, 0.1f) * kGravity);
    }
    if (targetSpeed < 0.1f && std::abs(speed) < 2.0f)
    {
        brake = std::max(brake, 0.5f); // hold it at the end
    }
    if (brake > 0.02f)
    {
        state.speedIntegral = 0.0f;
        controls.throttle = 0.0f;
        controls.brake = std::min(brake, 1.0f);
    }
    else
    {
        // The integral only trims the last of the error: while the car is still getting up to speed it
        // would wind up and overshoot.
        if (std::abs(error) < 1.5f)
        {
            state.speedIntegral = std::clamp(state.speedIntegral + settings.throttleIntegralGain * error * deltaSeconds, 0.0f, 1.0f);
        }
        controls.throttle = std::clamp(settings.throttleGain * error + state.speedIntegral, 0.0f, 1.0f);
        controls.brake = 0.0f;
    }
    return controls;
}

const char* PathFollowerStatusName(PathFollowerStatus status)
{
    switch (status)
    {
    case PathFollowerStatus::Running:
        return "running";
    case PathFollowerStatus::Finished:
        return "finished";
    case PathFollowerStatus::Failed:
        return "failed";
    }
    return "unknown";
}
}
