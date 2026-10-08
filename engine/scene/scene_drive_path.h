#pragma once

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace me
{

// A point a drive path passes through, in world space (metres). speedKmh is the speed the car is to have
// there; 0 takes the path's own speedKmh.
struct SceneDrivePathPoint
{
    glm::dvec3 position{0.0};
    float speedKmh = 0.0f;

    bool operator==(const SceneDrivePathPoint&) const = default;
};

// A line drawn on the map for a car to follow by itself (VehiclePathFollower), for driving tests
// (docs/design/2026-10-09-drive-path-follow-design.md). The curve passes through every point (a
// centripetal Catmull-Rom spline); a closed path joins its last point back to its first and is lapped.
struct SceneDrivePath
{
    std::string name;
    std::vector<SceneDrivePathPoint> points;
    bool closed = false;
    // The speed at points that set none (km/h).
    float speedKmh = 60.0f;
    // Laps a closed path is followed for.
    int laps = 1;

    bool operator==(const SceneDrivePath&) const = default;
};
}
