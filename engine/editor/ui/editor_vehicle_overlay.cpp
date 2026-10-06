#include "editor_vehicle_overlay.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <numbers>

namespace me
{

namespace
{
// Sizes at UI scale 1, multiplied by the effective UI scale like the rest of the viewport's overlay.
constexpr float kLineThickness = 2.0f;
constexpr float kArrowHeadPixels = 8.0f;
constexpr float kSpringAmplitudePixels = 5.0f;
constexpr float kFrictionCircleRadiusPixels = 34.0f;
constexpr float kOverlayMarginPixels = 12.0f;
constexpr int kTyreSegments = 28;
constexpr int kSpringCoils = 7;

constexpr ImU32 kLoadColor = IM_COL32(90, 230, 120, 255);
constexpr ImU32 kDriveColor = IM_COL32(255, 160, 50, 255);
constexpr ImU32 kCornerColor = IM_COL32(70, 200, 255, 255);
constexpr ImU32 kBumpStopColor = IM_COL32(255, 70, 60, 255);
constexpr ImU32 kRailColor = IM_COL32(200, 200, 210, 150);
constexpr ImU32 kAirborneColor = IM_COL32(140, 150, 170, 200);
constexpr ImU32 kTextColor = IM_COL32(255, 255, 255, 235);
constexpr ImU32 kTextShadowColor = IM_COL32(0, 0, 0, 170);

// A tyre's grip is in play as the share of its peak that its force makes, along and across it.
float GripUsageAlong(const VehicleWheelState& wheel)
{
    const float limit = wheel.longitudinalPeakFriction * wheel.suspensionForce;
    return limit > 1.0f ? std::abs(wheel.longitudinalForce) / limit : 0.0f;
}

float GripUsageAcross(const VehicleWheelState& wheel)
{
    const float limit = wheel.lateralPeakFriction * wheel.suspensionForce;
    return limit > 1.0f ? std::abs(wheel.lateralForce) / limit : 0.0f;
}

// The suspension travelled from full droop (0) to full bump (1).
float ComputeCompression(const VehicleWheelState& wheel)
{
    const float travel = wheel.suspensionMaxLength - wheel.suspensionMinLength;
    if (travel <= 1e-4f)
    {
        return 0.0f;
    }
    return std::clamp((wheel.suspensionMaxLength - wheel.suspensionLength) / travel, 0.0f, 1.0f);
}

ImU32 Mix(ImU32 a, ImU32 b, float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    const auto channel = [&](int shift)
    {
        const float from = static_cast<float>((a >> shift) & 0xFF);
        const float to = static_cast<float>((b >> shift) & 0xFF);
        return static_cast<ImU32>(from + (to - from) * t + 0.5f);
    };
    return channel(IM_COL32_R_SHIFT) << IM_COL32_R_SHIFT | channel(IM_COL32_G_SHIFT) << IM_COL32_G_SHIFT |
           channel(IM_COL32_B_SHIFT) << IM_COL32_B_SHIFT | channel(IM_COL32_A_SHIFT) << IM_COL32_A_SHIFT;
}

ImU32 WithAlpha(ImU32 color, int alpha)
{
    return (color & ~(0xFFu << IM_COL32_A_SHIFT)) | (static_cast<ImU32>(std::clamp(alpha, 0, 255)) << IM_COL32_A_SHIFT);
}

// Green while the spring has room, red as it comes to its bump stop.
ImU32 GetCompressionColor(float compression)
{
    return GetGripUsageColor((compression - 0.4f) / 0.6f);
}

struct Painter
{
    ImDrawList& drawList;
    ImVec2 origin;
    ImVec2 size;
    glm::mat4 viewProjection;
    float uiScale;

    bool Project(const glm::vec3& world, ImVec2& out) const
    {
        const glm::vec4 clip = viewProjection * glm::vec4(world, 1.0f);
        if (clip.w <= 0.0f)
        {
            return false;
        }
        const glm::vec3 ndc = glm::vec3(clip) / clip.w;
        if (ndc.z < 0.0f || ndc.z > 1.0f)
        {
            return false;
        }
        out = ImVec2(origin.x + (ndc.x * 0.5f + 0.5f) * size.x, origin.y + (1.0f - (ndc.y * 0.5f + 0.5f)) * size.y);
        return true;
    }

    void Line(const glm::vec3& from, const glm::vec3& to, ImU32 color, float thickness = kLineThickness) const
    {
        ImVec2 a;
        ImVec2 b;
        if (Project(from, a) && Project(to, b))
        {
            drawList.AddLine(a, b, color, thickness * uiScale);
        }
    }

    void Dot(const glm::vec3& at, float radiusPixels, ImU32 color) const
    {
        ImVec2 p;
        if (Project(at, p))
        {
            drawList.AddCircleFilled(p, radiusPixels * uiScale, color);
        }
    }

    // A line from `from` along `vector` with a head at the tip.
    void Arrow(const glm::vec3& from, const glm::vec3& vector, ImU32 color) const
    {
        ImVec2 a;
        ImVec2 b;
        if (!Project(from, a) || !Project(from + vector, b))
        {
            return;
        }
        drawList.AddLine(a, b, color, kLineThickness * 1.5f * uiScale);
        const float dx = b.x - a.x;
        const float dy = b.y - a.y;
        const float length = std::sqrt(dx * dx + dy * dy);
        if (length < 3.0f * uiScale)
        {
            return;
        }
        const float head = std::min(kArrowHeadPixels * uiScale, length * 0.6f);
        const ImVec2 along(dx / length, dy / length);
        const ImVec2 across(-along.y, along.x);
        drawList.AddTriangleFilled(
            b,
            ImVec2(b.x - along.x * head + across.x * head * 0.5f, b.y - along.y * head + across.y * head * 0.5f),
            ImVec2(b.x - along.x * head - across.x * head * 0.5f, b.y - along.y * head - across.y * head * 0.5f),
            color);
    }

    // A coil spring symbol between two points, zigzagging across the way it runs on the screen.
    void Spring(const glm::vec3& from, const glm::vec3& to, ImU32 color) const
    {
        ImVec2 a;
        ImVec2 b;
        if (!Project(from, a) || !Project(to, b))
        {
            return;
        }
        const float dx = b.x - a.x;
        const float dy = b.y - a.y;
        const float length = std::sqrt(dx * dx + dy * dy);
        if (length < 6.0f * uiScale)
        {
            drawList.AddLine(a, b, color, kLineThickness * 1.5f * uiScale);
            return;
        }
        const ImVec2 along(dx / length, dy / length);
        const ImVec2 across(-along.y, along.x);
        const float amplitude = std::min(kSpringAmplitudePixels * uiScale, length * 0.25f);
        // Straight for the first and last tenth, coils between.
        std::array<ImVec2, kSpringCoils * 2 + 4> points;
        size_t count = 0;
        points[count++] = a;
        const auto at = [&](float t, float side)
        {
            return ImVec2(a.x + dx * t + across.x * side, a.y + dy * t + across.y * side);
        };
        points[count++] = at(0.1f, 0.0f);
        for (int coil = 0; coil < kSpringCoils * 2; ++coil)
        {
            const float t = 0.1f + 0.8f * (static_cast<float>(coil) + 0.5f) / static_cast<float>(kSpringCoils * 2);
            points[count++] = at(t, coil % 2 == 0 ? amplitude : -amplitude);
        }
        points[count++] = at(0.9f, 0.0f);
        points[count++] = b;
        drawList.AddPolyline(points.data(), static_cast<int>(count), color, kLineThickness * uiScale);
    }

    void Text(const ImVec2& position, const char* text) const
    {
        drawList.AddText(ImVec2(position.x + 1.0f, position.y + 1.0f), kTextShadowColor, text);
        drawList.AddText(position, kTextColor, text);
    }
};

// The wheel's axes in world space: the axle, the way up in the tyre's plane and the way it rolls.
struct WheelAxes
{
    glm::vec3 axle;
    glm::vec3 up;
    glm::vec3 forward;
};

WheelAxes GetWheelAxes(const VehicleWheelState& wheel)
{
    return {
        wheel.pose.rotation * glm::vec3(1.0f, 0.0f, 0.0f),
        wheel.pose.rotation * glm::vec3(0.0f, 1.0f, 0.0f),
        wheel.pose.rotation * glm::vec3(0.0f, 0.0f, 1.0f)};
}

void DrawSuspension(const Painter& painter, const VehicleWheelState& wheel, size_t index)
{
    const WheelAxes axes = GetWheelAxes(wheel);
    const glm::vec3 center = wheel.pose.position;
    const float compression = ComputeCompression(wheel);
    const ImU32 color = wheel.inContact ? GetCompressionColor(compression) : kAirborneColor;

    // The spring, from where it hangs to the wheel's centre.
    painter.Spring(wheel.mount, center, color);
    painter.Dot(wheel.mount, 3.0f, kTextColor);

    // The travel as a rail beside the tyre on the car's outer side: the mount's end is full bump, the far
    // end full droop, and the marker is where the wheel is. The left wheels are the even ones, and the
    // axle points to the car's left.
    const float outward = index % 2 == 0 ? 1.0f : -1.0f;
    const glm::vec3 offset = axes.axle * outward * (wheel.width * 0.5f + 0.1f);
    const glm::vec3 bump = wheel.mount + wheel.suspensionAxis * wheel.suspensionMinLength + offset;
    const glm::vec3 droop = wheel.mount + wheel.suspensionAxis * wheel.suspensionMaxLength + offset;
    painter.Line(bump, droop, kRailColor, 3.0f);
    painter.Line(bump - axes.forward * 0.03f, bump + axes.forward * 0.03f, kBumpStopColor, 2.0f);
    painter.Line(droop - axes.forward * 0.03f, droop + axes.forward * 0.03f, kRailColor, 2.0f);
    painter.Dot(wheel.mount + wheel.suspensionAxis * wheel.suspensionLength + offset, 4.5f, color);
}

void DrawTyre(const Painter& painter, const VehicleWheelState& wheel, bool drawsPatch)
{
    const WheelAxes axes = GetWheelAxes(wheel);
    const glm::vec3 center = wheel.pose.position;
    const float usage = wheel.inContact ? std::max(GripUsageAlong(wheel), GripUsageAcross(wheel)) : 0.0f;
    const ImU32 color = wheel.inContact ? GetGripUsageColor(usage) : kAirborneColor;

    // The tread's two edges, and a spoke that turns with the wheel.
    for (const float side : {-0.5f, 0.5f})
    {
        std::array<ImVec2, kTyreSegments + 1> ring;
        bool visible = true;
        for (int segment = 0; segment <= kTyreSegments && visible; ++segment)
        {
            const float angle = 2.0f * std::numbers::pi_v<float> * static_cast<float>(segment) / static_cast<float>(kTyreSegments);
            const glm::vec3 point =
                center + axes.axle * (wheel.width * side) + (axes.up * std::cos(angle) + axes.forward * std::sin(angle)) * wheel.radius;
            visible = painter.Project(point, ring[static_cast<size_t>(segment)]);
        }
        if (visible)
        {
            painter.drawList.AddPolyline(ring.data(), kTyreSegments + 1, color, kLineThickness * painter.uiScale);
        }
    }
    painter.Line(center, center + axes.up * wheel.radius, color, 2.0f);

    if (!wheel.inContact || drawsPatch)
    {
        return;
    }
    // The contact patch on the ground, a rectangle as long as a fraction of the radius and as wide as
    // the tread, filled by how much grip it uses.
    const glm::vec3 along = wheel.contactLongitudinal * (wheel.radius * 0.3f);
    const glm::vec3 across = wheel.contactLateral * (wheel.width * 0.5f);
    const std::array<glm::vec3, 4> corners = {
        wheel.contactPosition + along + across,
        wheel.contactPosition + along - across,
        wheel.contactPosition - along - across,
        wheel.contactPosition - along + across};
    std::array<ImVec2, 4> projected;
    for (size_t corner = 0; corner < corners.size(); ++corner)
    {
        if (!painter.Project(corners[corner], projected[corner]))
        {
            return;
        }
    }
    painter.drawList.AddConvexPolyFilled(projected.data(), 4, WithAlpha(color, 110));
    painter.drawList.AddPolyline(projected.data(), 4, color, 1.5f * painter.uiScale, ImDrawFlags_Closed);
}

constexpr ImU32 kStuckColor = IM_COL32(70, 220, 110, 255);
constexpr ImU32 kSlidingColor = IM_COL32(255, 70, 50, 255);
constexpr ImU32 kCarcassColor = IM_COL32(255, 220, 60, 255);
constexpr ImU32 kPatchAtRestColor = IM_COL32(220, 220, 230, 160);

// The brush tyre's contact patch in its frame (x forward, y left, on the ground): each rib a strip over
// its contact length on the carcass's centre line, which the carcass shifts by x_c and moves sideways by
// y_c + theta_c (x - x_c) - y_c Psi/2 (x - x_c)^2, all `scale` times their size. The leading part of a
// rib, where the bristles stick, is green; past the transition they slide, red. The patch at rest is
// outlined, and the carcass's centre line runs through it in yellow.
void DrawBrushPatch(const Painter& painter, const VehicleWheelState& wheel, float scale)
{
    if (!wheel.inContact || wheel.brushRibCount <= 0)
    {
        return;
    }
    const glm::vec3 origin = wheel.contactPosition + wheel.contactNormal * 0.004f;
    const glm::vec3 forward = wheel.contactLongitudinal;
    const glm::vec3 left = -wheel.contactLateral;
    const float xc = wheel.carcassDeflection.x;
    const float yc = wheel.carcassDeflection.y;
    const float thetac = wheel.carcassDeflection.z;
    const float psi = wheel.carcassBendingShape;
    const auto carcass = [&](float x)
    {
        const float dx = x - xc;
        return yc + thetac * dx - 0.5f * yc * psi * dx * dx;
    };
    const auto at = [&](float x, float y)
    {
        return origin + forward * x + left * y;
    };
    // Where a point of the patch is drawn: moved with the carcass, exaggerated.
    const auto deformed = [&](float x, float y)
    {
        return at(x + scale * xc, y + scale * carcass(x));
    };

    float longest = 0.0f;
    float halfWidth = 0.0f;
    const float ribWidth = wheel.width / static_cast<float>(wheel.brushRibCount);
    for (int index = 0; index < wheel.brushRibCount; ++index)
    {
        const VehicleWheelState::BrushRib& rib = wheel.brushRibs[static_cast<size_t>(index)];
        longest = std::max(longest, rib.length);
        halfWidth = std::max(halfWidth, std::abs(rib.y) + 0.5f * ribWidth);
    }
    if (longest <= 0.0f)
    {
        return;
    }

    // At rest: the patch as long as its longest rib and as wide as the tread.
    painter.Line(at(longest * 0.5f, halfWidth), at(longest * 0.5f, -halfWidth), kPatchAtRestColor, 1.0f);
    painter.Line(at(-longest * 0.5f, halfWidth), at(-longest * 0.5f, -halfWidth), kPatchAtRestColor, 1.0f);
    painter.Line(at(longest * 0.5f, halfWidth), at(-longest * 0.5f, halfWidth), kPatchAtRestColor, 1.0f);
    painter.Line(at(longest * 0.5f, -halfWidth), at(-longest * 0.5f, -halfWidth), kPatchAtRestColor, 1.0f);

    // The ribs, cut into slices along their length, each filled by whether its bristles stick.
    constexpr int kSlices = 12;
    const float direction = wheel.treadRollingForward ? 1.0f : -1.0f;
    const float half = 0.42f * ribWidth;
    for (int index = 0; index < wheel.brushRibCount; ++index)
    {
        const VehicleWheelState::BrushRib& rib = wheel.brushRibs[static_cast<size_t>(index)];
        if (rib.length <= 0.0f)
        {
            continue;
        }
        // One outline per region, from a to b along the rib (distances from the leading edge): down one
        // side of the bent strip and back up the other, so the region fills without seams.
        const auto drawRegion = [&](float a, float b, ImU32 color)
        {
            if (b - a <= 1e-5f)
            {
                return;
            }
            std::array<ImVec2, 2 * (kSlices + 1)> outline;
            for (int slice = 0; slice <= kSlices; ++slice)
            {
                const float distance = a + (b - a) * static_cast<float>(slice) / kSlices;
                const float x = direction * (0.5f * rib.length - distance);
                if (!painter.Project(deformed(x, rib.y + half), outline[static_cast<size_t>(slice)]) ||
                    !painter.Project(deformed(x, rib.y - half), outline[static_cast<size_t>(2 * kSlices + 1 - slice)]))
                {
                    return;
                }
            }
            painter.drawList.AddConcavePolyFilled(outline.data(), static_cast<int>(outline.size()), WithAlpha(color, 170));
        };
        const float split = std::clamp(rib.stuckLength, 0.0f, rib.length);
        drawRegion(0.0f, split, kStuckColor);
        drawRegion(split, rib.length, kSlidingColor);
        // The transition from sticking to sliding, across the rib.
        if (rib.stuckLength < rib.length)
        {
            const float x = direction * (0.5f * rib.length - rib.stuckLength);
            painter.Line(deformed(x, rib.y + half), deformed(x, rib.y - half), kTextColor, 1.5f);
        }
    }

    // The carcass's centre line over a little more than the patch.
    constexpr int kLinePoints = 16;
    for (int point = 0; point < kLinePoints; ++point)
    {
        const float a = (static_cast<float>(point) / kLinePoints - 0.5f) * longest * 1.4f;
        const float b = (static_cast<float>(point + 1) / kLinePoints - 0.5f) * longest * 1.4f;
        painter.Line(deformed(a, 0.0f), deformed(b, 0.0f), kCarcassColor, 2.0f);
    }
}

void DrawForces(const Painter& painter, const VehicleWheelState& wheel, float metresPerKilonewton)
{
    if (!wheel.inContact)
    {
        return;
    }
    const float metresPerNewton = metresPerKilonewton * 0.001f;
    // What the ground does to the car: up through the spring, and along and across the tyre.
    painter.Arrow(wheel.contactPosition, wheel.contactNormal * (wheel.suspensionForce * metresPerNewton), kLoadColor);
    painter.Arrow(wheel.contactPosition, wheel.contactLongitudinal * (wheel.longitudinalForce * metresPerNewton), kDriveColor);
    painter.Arrow(wheel.contactPosition, wheel.contactLateral * (wheel.lateralForce * metresPerNewton), kCornerColor);
}

void DrawFrictionCircles(const Painter& painter, const std::vector<VehicleWheelState>& wheels)
{
    // The circles sit as the wheels do seen from above, front at the top: left wheels on the left.
    const float radius = kFrictionCircleRadiusPixels * painter.uiScale;
    const float margin = kOverlayMarginPixels * painter.uiScale;
    const float lineHeight = ImGui::GetTextLineHeight();
    const float cellWidth = radius * 2.0f + 30.0f * painter.uiScale;
    const float cellHeight = radius * 2.0f + lineHeight * 3.0f + 10.0f * painter.uiScale;
    const float left = painter.origin.x + margin;
    const float bottom = painter.origin.y + painter.size.y - margin;
    const std::array<const char*, kVehicleWheelCount> names = {"FL", "FR", "RL", "RR"};

    for (size_t index = 0; index < wheels.size() && index < kVehicleWheelCount; ++index)
    {
        const VehicleWheelState& wheel = wheels[index];
        const float column = static_cast<float>(index % 2);
        const float row = static_cast<float>(index / 2);
        const ImVec2 cellMin(left + column * cellWidth, bottom - (2.0f - row) * cellHeight);
        const ImVec2 center(cellMin.x + radius + 4.0f * painter.uiScale, cellMin.y + radius + 2.0f * painter.uiScale);

        painter.drawList.AddCircleFilled(center, radius, IM_COL32(12, 16, 24, 150), 48);
        painter.drawList.AddLine(ImVec2(center.x - radius, center.y), ImVec2(center.x + radius, center.y), IM_COL32(255, 255, 255, 40));
        painter.drawList.AddLine(ImVec2(center.x, center.y - radius), ImVec2(center.x, center.y + radius), IM_COL32(255, 255, 255, 40));
        // The peak grip of the tyre.
        painter.drawList.AddCircle(center, radius, IM_COL32(255, 255, 255, 140), 48, 1.5f * painter.uiScale);

        const float usage = wheel.inContact ? std::max(GripUsageAlong(wheel), GripUsageAcross(wheel)) : 0.0f;
        if (wheel.inContact && wheel.suspensionForce > 1.0f)
        {
            // Across the tyre to the right, along it up; the circle's edge is the peak on each axis.
            const float across = wheel.lateralPeakFriction > 1e-3f ? wheel.lateralForce / (wheel.lateralPeakFriction * wheel.suspensionForce) : 0.0f;
            const float along = wheel.longitudinalPeakFriction > 1e-3f ? wheel.longitudinalForce / (wheel.longitudinalPeakFriction * wheel.suspensionForce) : 0.0f;
            const ImVec2 dot(
                center.x + std::clamp(across, -1.3f, 1.3f) * radius,
                center.y - std::clamp(along, -1.3f, 1.3f) * radius);
            painter.drawList.AddLine(center, dot, WithAlpha(GetGripUsageColor(usage), 160), 1.5f * painter.uiScale);
            painter.drawList.AddCircleFilled(dot, 4.0f * painter.uiScale, GetGripUsageColor(usage));
        }

        char text[64];
        const float textX = cellMin.x + 2.0f * painter.uiScale;
        float textY = center.y + radius + 3.0f * painter.uiScale;
        std::snprintf(text, sizeof(text), "%s  %.1f kN", names[index], wheel.suspensionForce * 0.001f);
        painter.Text(ImVec2(textX, textY), text);
        textY += lineHeight;
        std::snprintf(text, sizeof(text), "slip %+.0f%%  %+.1f\xC2\xB0", wheel.slipRatio * 100.0f, wheel.slipAngleDegrees);
        painter.Text(ImVec2(textX, textY), wheel.inContact ? text : "airborne");
        textY += lineHeight;
        std::snprintf(text, sizeof(text), "travel %.0f%%  brake %.0f Nm", ComputeCompression(wheel) * 100.0f, wheel.brakeTorque);
        painter.Text(ImVec2(textX, textY), text);
    }
}
}

void DrawVehicleLinkageOverlay(
    ImDrawList& drawList,
    const ImVec2& origin,
    const ImVec2& size,
    const glm::mat4& viewProjection,
    const VehicleLinkage& linkage,
    float uiScale)
{
    const Painter painter{drawList, origin, size, viewProjection, uiScale};
    constexpr ImU32 kLinkColor = IM_COL32(90, 165, 255, 255);
    constexpr ImU32 kCarrierColor = IM_COL32(255, 155, 50, 255);
    constexpr ImU32 kChassisColor = IM_COL32(205, 205, 210, 255);
    constexpr ImU32 kJointColor = IM_COL32(255, 215, 75, 255);
    for (const auto& [a, b] : linkage.links)
    {
        painter.Line(a, b, kLinkColor, 2.5f);
    }
    for (const auto& [a, b] : linkage.carriers)
    {
        painter.Line(a, b, kCarrierColor, 3.0f);
    }
    for (const glm::vec3& p : linkage.chassis)
    {
        painter.Dot(p, 3.5f, kChassisColor);
    }
    for (const glm::vec3& p : linkage.joints)
    {
        painter.Dot(p, 3.5f, kJointColor);
    }
}

float ComputeTyreGripUsage(const VehicleWheelState& wheel)
{
    return wheel.inContact ? std::max(GripUsageAlong(wheel), GripUsageAcross(wheel)) : 0.0f;
}

ImU32 GetGripUsageColor(float t, int alpha)
{
    constexpr ImU32 kGreen = IM_COL32(90, 230, 120, 255);
    constexpr ImU32 kYellow = IM_COL32(250, 220, 60, 255);
    constexpr ImU32 kRed = IM_COL32(255, 70, 60, 255);
    t = std::clamp(t, 0.0f, 1.0f);
    const ImU32 color = t < 0.5f ? Mix(kGreen, kYellow, t * 2.0f) : Mix(kYellow, kRed, (t - 0.5f) * 2.0f);
    return WithAlpha(color, alpha);
}

void DrawVehiclePhysicsOverlay(
    ImDrawList& drawList,
    const ImVec2& origin,
    const ImVec2& size,
    const glm::mat4& viewProjection,
    const std::vector<VehicleWheelState>& wheels,
    const VehiclePhysicsOverlaySettings& settings,
    float uiScale)
{
    if (!settings.enabled || wheels.empty() || size.x <= 0.0f || size.y <= 0.0f)
    {
        return;
    }
    const Painter painter{drawList, origin, size, viewProjection, uiScale};
    drawList.PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
    for (size_t index = 0; index < wheels.size(); ++index)
    {
        const bool brushPatch = settings.contactPatch && wheels[index].brushTyre;
        if (settings.tyres)
        {
            DrawTyre(painter, wheels[index], brushPatch);
        }
        if (brushPatch)
        {
            DrawBrushPatch(painter, wheels[index], settings.deformationScale);
        }
        if (settings.suspension)
        {
            DrawSuspension(painter, wheels[index], index);
        }
        if (settings.forces)
        {
            DrawForces(painter, wheels[index], settings.metresPerKilonewton);
        }
    }
    if (settings.frictionCircles)
    {
        DrawFrictionCircles(painter, wheels);
    }
    drawList.PopClipRect();
}
}
