#include <engine/editor/renderer_shared_state.h>
#include <engine/editor/services/scene_io_service.h>
#include <engine/logic/editor_world.h>

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

// What counts as an unsaved change to the scene (SceneIoService::HasUnsavedChanges), which the editor
// asks about before closing, opening another scene or starting a new one.
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

void EditsAreUnsavedUntilSaved()
{
    RendererSharedState state;
    state.editorWorld = CreateEditorWorld();
    IEditorWorld& world = state.GetEditorWorld();
    SerializedEntityData boxData;
    boxData.tagName = "Box";
    const entt::entity box = world.CreateEntity(boxData);
    SerializedEntityData driverData;
    driverData.tagName = "Driver";
    driverData.driverVehicleUuid = world.GetEntityUuid(box);
    const entt::entity driver = world.CreateEntity(driverData);

    Require(!SceneIoService::HasUnsavedChanges(state), "the scene as first seen is the saved one");

    world.SetSelectedEntity(box);
    world.GetGizmoSettings().useSnap = !world.GetGizmoSettings().useSnap;
    Require(!SceneIoService::HasUnsavedChanges(state), "selecting and the gizmo's settings change nothing to save");

    world.EditTransform(driver).translation = glm::vec3(3.0f, 1.0f, 2.0f);
    Require(!SceneIoService::HasUnsavedChanges(state), "a seated driver's place is its car's, not an edit");

    world.EditTransform(box).translation.x += 1.0f;
    Require(SceneIoService::HasUnsavedChanges(state), "moving an entity is an unsaved change");
    world.EditTransform(box).translation.x -= 1.0f;
    Require(!SceneIoService::HasUnsavedChanges(state), "moved back, nothing is left to save");

    std::vector<SceneDrivePath> paths(1);
    paths[0].name = "lap";
    paths[0].points = {{{1.0, 0.0, 2.0}, 0.0f}, {{10.0, 0.0, 2.0}, 0.0f}};
    world.SetDrivePaths(paths);
    Require(SceneIoService::HasUnsavedChanges(state), "a new drive path is an unsaved change");

    const std::filesystem::path file = std::filesystem::temp_directory_path() / "scene_unsaved_tests.yaml";
    SceneIoService::SaveScene(state, file.string());
    Require(!SceneIoService::HasUnsavedChanges(state), "saved, nothing is left to save");
    Require(world.GetSceneFilePath() == file.string(), "the scene is now that file's");

    // A running clock moves the sun every frame; the hours set by hand on a stopped one are an edit.
    SceneEnvironment environment = world.GetEnvironment();
    environment.timeOfDay.enabled = true;
    environment.timeOfDay.timeScale = 60.0f;
    world.SetEnvironment(environment);
    SceneIoService::MarkSceneSaved(state);
    environment.timeOfDay.hours += 0.5f;
    world.SetEnvironment(environment);
    Require(!SceneIoService::HasUnsavedChanges(state), "a running time of day is not an edit");
    environment.timeOfDay.timeScale = 0.0f;
    world.SetEnvironment(environment);
    Require(SceneIoService::HasUnsavedChanges(state), "stopping the clock is");

    SceneIoService::NewScene(state);
    Require(!SceneIoService::HasUnsavedChanges(state), "a new scene starts with nothing to save");
    SerializedEntityData another;
    another.tagName = "Another";
    world.CreateEntity(another);
    Require(SceneIoService::HasUnsavedChanges(state), "an entity added to it is unsaved");
    std::filesystem::remove(file);
}
}

int main()
{
    try
    {
        EditsAreUnsavedUntilSaved();
    }
    catch (const std::exception& error)
    {
        std::cerr << "scene unsaved tests failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "scene unsaved tests passed\n";
    return 0;
}
