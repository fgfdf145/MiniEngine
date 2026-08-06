#include "gizmo_settings.h"

namespace
{
bool ContainsOperation(ImGuizmo::OPERATION operation, ImGuizmo::OPERATION expected)
{
    const int operationBits = static_cast<int>(operation);
    const int expectedBits = static_cast<int>(expected);
    return (operationBits & expectedBits) == expectedBits;
}
}

ImGuizmo::OPERATION ToggleGizmoOperation(ImGuizmo::OPERATION operation)
{
    return operation == ImGuizmo::SCALE ? kCombinedGizmoOperation : ImGuizmo::SCALE;
}

std::array<float, 3> BuildGizmoSnapValues(const GizmoSettings& settings, GizmoSnapFamily family)
{
    switch (family)
    {
    case GizmoSnapFamily::Rotation:
        return {settings.rotationSnap, 0.0f, 0.0f};
    case GizmoSnapFamily::Scale:
        return {settings.scaleSnap.x, settings.scaleSnap.y, settings.scaleSnap.z};
    case GizmoSnapFamily::None:
    case GizmoSnapFamily::Translation:
    default:
        return {
            settings.translationSnap.x,
            settings.translationSnap.y,
            settings.translationSnap.z};
    }
}

void GizmoDragSnapState::PrepareForManipulate(
    ImGuizmo::OPERATION operation,
    bool gizmoIsUsing,
    bool translationHandleHovered,
    bool rotationHandleHovered)
{
    if (gizmoIsUsing && m_family != GizmoSnapFamily::None)
    {
        return;
    }

    if (operation == ImGuizmo::SCALE)
    {
        m_family = GizmoSnapFamily::Scale;
        return;
    }

    const bool hasRotation = ContainsOperation(operation, ImGuizmo::ROTATE);
    const bool hasTranslation = ContainsOperation(operation, ImGuizmo::TRANSLATE);
    if (hasRotation && !hasTranslation)
    {
        m_family = GizmoSnapFamily::Rotation;
        return;
    }
    if (hasTranslation && translationHandleHovered)
    {
        m_family = GizmoSnapFamily::Translation;
        return;
    }
    if (hasRotation && rotationHandleHovered)
    {
        m_family = GizmoSnapFamily::Rotation;
        return;
    }

    m_family = GizmoSnapFamily::Translation;
}

void GizmoDragSnapState::FinishManipulate(bool gizmoIsUsing)
{
    if (!gizmoIsUsing)
    {
        m_family = GizmoSnapFamily::None;
    }
}
