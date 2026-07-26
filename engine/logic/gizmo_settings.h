#pragma once

#include <world_units.h>

#include <imgui.h>
#include <ImGuizmo.h>
#include <glm/glm.hpp>

#include <array>

inline constexpr ImGuizmo::OPERATION kCombinedGizmoOperation =
    static_cast<ImGuizmo::OPERATION>(
        static_cast<int>(ImGuizmo::TRANSLATE) |
        static_cast<int>(ImGuizmo::ROTATE)
    );

enum class GizmoSnapFamily
{
    None,
    Translation,
    Rotation,
    Scale
};

struct GizmoSettings
{
    ImGuizmo::OPERATION operation = kCombinedGizmoOperation;
    ImGuizmo::MODE mode = ImGuizmo::WORLD;
    bool useSnap = false;
    glm::vec3 translationSnap = WorldUnits::kDefaultTranslationSnapMeters;
    float rotationSnap = WorldUnits::kDefaultRotationSnapDegrees;
    glm::vec3 scaleSnap = WorldUnits::kDefaultScaleSnap;
};

ImGuizmo::OPERATION ToggleGizmoOperation(ImGuizmo::OPERATION operation);
std::array<float, 3> BuildGizmoSnapValues(const GizmoSettings& settings, GizmoSnapFamily family);

class GizmoDragSnapState
{
public:
    void PrepareForManipulate(
        ImGuizmo::OPERATION operation,
        bool gizmoIsUsing,
        bool translationHandleHovered,
        bool rotationHandleHovered
    );
    void FinishManipulate(bool gizmoIsUsing);
    GizmoSnapFamily Family() const { return m_family; }

private:
    GizmoSnapFamily m_family = GizmoSnapFamily::None;
};
