#pragma once

#include <engine/editor/services/vehicle_drive_service.h>

#include <glm/glm.hpp>
#include <imgui.h>

#include <vector>

namespace me
{

// Draws a driven car's suspension and tyre physics over the viewport's rectangle (`origin` and `size`,
// in screen points) on `drawList`, as seen through `viewProjection`: each spring with a gauge of its
// travel, each tyre with its contact patch and the forces there, and a friction circle per wheel in the
// bottom left corner. `wheels` are front left, front right, rear left, rear right in world space.
void DrawVehiclePhysicsOverlay(
    ImDrawList& drawList,
    const ImVec2& origin,
    const ImVec2& size,
    const glm::mat4& viewProjection,
    const std::vector<VehicleWheelState>& wheels,
    const VehiclePhysicsOverlaySettings& settings,
    float uiScale);

// The share of a tyre's grip in use: 0 for none, 1 at the limit of the friction it has on the ground,
// more when it slides past it.
float ComputeTyreGripUsage(const VehicleWheelState& wheel);

// From green through yellow to red as `t` runs from 0 to 1.
ImU32 GetGripUsageColor(float t, int alpha = 255);
}
