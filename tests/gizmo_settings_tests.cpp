#include <gizmo_settings.h>
#include <editor_world.h>

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
void Require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

bool NearlyEqual(float lhs, float rhs)
{
    return std::abs(lhs - rhs) < 0.0001f;
}

ImGuizmo::OPERATION LoadStoredOperation(const std::string& storedValue)
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / ("miniengine_gizmo_" + storedValue + ".yaml");
    {
        std::ofstream file(path);
        file
            << "scene:\n"
            << "  version: 3\n"
            << "  selected_entity: 0\n"
            << "entities: []\n"
            << "lights: []\n"
            << "editor:\n"
            << "  gizmo:\n"
            << "    operation: " << storedValue << "\n";
    }

    const SerializedSceneData loaded = LoadEditorSceneDataFromFile(path.string());
    std::error_code removeError;
    std::filesystem::remove(path, removeError);
    return loaded.gizmo.operation;
}

std::string SaveStoredOperation(ImGuizmo::OPERATION operation, const char* suffix)
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / (std::string("miniengine_gizmo_save_") + suffix + ".yaml");
    SerializedSceneData scene{};
    scene.gizmo.operation = operation;
    SaveEditorSceneDataToFile(scene, path.string());

    std::ifstream file(path);
    const std::string yaml((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::error_code removeError;
    std::filesystem::remove(path, removeError);
    return yaml;
}
}

int main()
{
    try
    {
        const int combinedBits = static_cast<int>(kCombinedGizmoOperation);
        Require(
            (combinedBits & static_cast<int>(ImGuizmo::TRANSLATE)) ==
                static_cast<int>(ImGuizmo::TRANSLATE),
            "combined operation is missing translation handles"
        );
        Require(
            (combinedBits & static_cast<int>(ImGuizmo::ROTATE)) ==
                static_cast<int>(ImGuizmo::ROTATE),
            "combined operation is missing rotation handles"
        );
        Require(
            (combinedBits & static_cast<int>(ImGuizmo::SCALE)) == 0,
            "combined operation unexpectedly contains scale handles"
        );

        GizmoSettings settings{};
        Require(settings.operation == kCombinedGizmoOperation, "combined mode is not the default");
        Require(
            ToggleGizmoOperation(kCombinedGizmoOperation) == ImGuizmo::SCALE,
            "R toggle did not enter scale"
        );
        Require(
            ToggleGizmoOperation(ImGuizmo::SCALE) == kCombinedGizmoOperation,
            "R toggle did not return to combined"
        );

        GizmoDragSnapState dragState;
        dragState.PrepareForManipulate(kCombinedGizmoOperation, false, true);
        Require(dragState.Family() == GizmoSnapFamily::Rotation, "rotation hover did not select rotation snap");
        dragState.FinishManipulate(true);
        dragState.PrepareForManipulate(kCombinedGizmoOperation, true, false);
        Require(dragState.Family() == GizmoSnapFamily::Rotation, "snap family changed during rotation drag");
        dragState.FinishManipulate(false);
        Require(dragState.Family() == GizmoSnapFamily::None, "snap family did not clear after drag");

        dragState.PrepareForManipulate(kCombinedGizmoOperation, false, false);
        Require(dragState.Family() == GizmoSnapFamily::Translation, "axis or plane did not select translation snap");
        dragState.FinishManipulate(false);
        dragState.PrepareForManipulate(ImGuizmo::SCALE, false, false);
        Require(dragState.Family() == GizmoSnapFamily::Scale, "scale mode did not select scale snap");

        settings.translationSnap = { 1.0f, 2.0f, 3.0f };
        settings.rotationSnap = 15.0f;
        settings.scaleSnap = { 0.1f, 0.2f, 0.3f };
        const std::array<float, 3> translation = BuildGizmoSnapValues(settings, GizmoSnapFamily::Translation);
        const std::array<float, 3> rotation = BuildGizmoSnapValues(settings, GizmoSnapFamily::Rotation);
        const std::array<float, 3> scale = BuildGizmoSnapValues(settings, GizmoSnapFamily::Scale);
        Require(
            NearlyEqual(translation[0], 1.0f) &&
                NearlyEqual(translation[1], 2.0f) &&
                NearlyEqual(translation[2], 3.0f),
            "translation snap values changed"
        );
        Require(
            NearlyEqual(rotation[0], 15.0f) &&
                NearlyEqual(rotation[1], 0.0f) &&
                NearlyEqual(rotation[2], 0.0f),
            "rotation snap values changed"
        );
        Require(
            NearlyEqual(scale[0], 0.1f) &&
                NearlyEqual(scale[1], 0.2f) &&
                NearlyEqual(scale[2], 0.3f),
            "scale snap values changed"
        );
        Require(
            LoadStoredOperation("combined") == kCombinedGizmoOperation,
            "combined yaml did not load as combined"
        );
        Require(
            LoadStoredOperation("translate") == kCombinedGizmoOperation,
            "legacy translate yaml did not upgrade to combined"
        );
        Require(
            LoadStoredOperation("rotate") == kCombinedGizmoOperation,
            "legacy rotate yaml did not upgrade to combined"
        );
        Require(
            LoadStoredOperation("scale") == ImGuizmo::SCALE,
            "scale yaml did not remain scale"
        );
        Require(
            LoadStoredOperation("unexpected") == kCombinedGizmoOperation,
            "unknown yaml did not use the combined fallback"
        );
        Require(
            SaveStoredOperation(kCombinedGizmoOperation, "combined").find("operation: combined") != std::string::npos,
            "combined mode did not serialize canonically"
        );
        Require(
            SaveStoredOperation(ImGuizmo::SCALE, "scale").find("operation: scale") != std::string::npos,
            "scale mode did not serialize canonically"
        );

        std::cout << "gizmo settings tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "gizmo settings tests failed: " << error.what() << '\n';
        return 1;
    }
}
