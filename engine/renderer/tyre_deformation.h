#pragma once

#include <glm/glm.hpp>

#include <array>

namespace me
{

// A tyre squashed on the ground and bent by its carcass, for drawing
// (docs/design/2026-10-08-tyre-visual-deformation-design.md). The skinning pass runs
// shaders/vulkan/tyre_deform.comp, which mirrors these functions line for line, over each tyre's
// vertices at rest. Everything is in the tyre mesh's space at rest (the model's), so the wheel's own
// transform (spin, steer, travel) carries the deformed tyre after.
//
// A vertex at distance r from the axle, h of the way from the bead to the tread (h = (r - bead) /
// (radius - bead)), moves
// - in, towards the axle, by as much as lifts it out of the ground (the patch: the tread flattened on
//   it), the corner rounded over `smoothing` of penetration, and by h^2 of what the tread above it
//   moves (the sidewall squashed with it, the bead staying on the rim);
// - out along the axle, away from the tyre's middle, by bulge * sin(pi h) of the tread's move (the
//   sidewall bulging);
// - with the carcass, h^2 of the way: forward by x_c and to the left by the carcass's centre line
//   y_c + theta_c (x - x_c) - y_c Psi/2 (x - x_c)^2 at its place x along the patch (held to the
//   patch's length), fading out round the wheel over kTyreCarcassSpreadRadians past the patch.
struct TyreDeformation
{
    // False: the tyre keeps its shape at rest (in the air, or not driven).
    bool onGround = false;
    glm::vec3 center{0.0f};
    // Unit length.
    glm::vec3 axle{1.0f, 0.0f, 0.0f};
    float outerRadius = 0.0f;
    float innerRadius = 0.0f;
    // The tyre's middle along the axle from the centre.
    float axialCenter = 0.0f;
    // The ground as the plane dot(p, groundNormal) = groundOffset, its normal (unit) out of the ground.
    glm::vec3 groundNormal{0.0f, 1.0f, 0.0f};
    float groundOffset = 0.0f;
    // In the ground's plane, unit length: the tyre's heading and its left.
    glm::vec3 forward{0.0f, 0.0f, 1.0f};
    glm::vec3 left{1.0f, 0.0f, 0.0f};
    // The brush tyre's carcass against the rim (VehicleWheelState::carcassDeflection, carcassBendingShape)
    // in this space's units: shift forward and to the left, twist (rad) and bending shape (1 / length^2).
    float carcassForward = 0.0f;
    float carcassLeft = 0.0f;
    float carcassTwist = 0.0f;
    float carcassBending = 0.0f;
    float bulge = 0.0f;
    float smoothing = 0.0f;
};

// The sidewall's bulge along the axle per unit of the tread's move in, at its middle.
inline constexpr float kTyreBulge = 0.4f;
// The width of the patch's rounded edge, as a share of the tyre's radius.
inline constexpr float kTyreSmoothing = 0.015f;
// How far round the wheel past the patch the carcass's shift fades out.
inline constexpr float kTyreCarcassSpreadRadians = 1.2f;
// The least a radial move in lifts a point (the cosine of its angle from the ground's normal), so a
// point far round the wheel is not pushed through the axle to clear the ground.
inline constexpr float kTyreMinimumLift = 0.25f;

// The tyre deformation pass's two matrices for one tyre, as tyre_deform.comp reads them.
std::array<glm::mat4, 2> PackTyreDeformation(const TyreDeformation& deformation);
TyreDeformation UnpackTyreDeformation(const std::array<glm::mat4, 2>& packed);

// max(x, 0) with its corner rounded over |x| < width: C1, never below x or 0.
float TyreSmoothPositive(float x, float width);

// Where a point of the tyre at rest goes.
glm::vec3 DeformTyrePoint(const TyreDeformation& deformation, const glm::vec3& point);

struct TyreVertexFrame
{
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    glm::vec3 tangent{1.0f, 0.0f, 0.0f};
};

// A vertex moved with its normal and tangent: the deformation's differences over a step of 1e-3 of
// the radius along the tangent and across it.
TyreVertexFrame DeformTyreVertex(const TyreDeformation& deformation, const TyreVertexFrame& vertex);
}
