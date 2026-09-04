#include <engine/logic/editor_world.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

// main() stays in the global namespace; everything it drives lives in me::.
using namespace me;

namespace
{
void Require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}
}

int main()
{
    try
    {
        std::unique_ptr<IEditorWorld> world = CreateEditorWorld();

        SerializedEntityData first{};
        first.entityUuid = "11111111-1111-4111-8111-111111111111";
        first.tagName = "First";
        const entt::entity firstEntity = world->CreateEntity(first);

        SerializedEntityData duplicate{};
        duplicate.entityUuid = first.entityUuid;
        duplicate.tagName = "Duplicate";
        const entt::entity duplicateEntity = world->CreateEntity(duplicate);

        Require(world->GetEntityUuid(firstEntity) == first.entityUuid, "first entity did not retain requested uuid");
        Require(!world->GetEntityUuid(duplicateEntity).empty(), "duplicate entity did not receive a uuid");
        Require(
            world->GetEntityUuid(duplicateEntity) != world->GetEntityUuid(firstEntity),
            "duplicate scene uuids were not arbitrated");

        SerializedLightData light{};
        light.entityUuid = "22222222-2222-4222-8222-222222222222";
        light.tagName = "Selected Light";
        const entt::entity lightEntity = world->CreateLightEntity(light);
        world->SetSelectedEntity(lightEntity);

        const SerializedSceneData captured = world->CaptureSceneData();
        Require(captured.selectedEntityUuid == light.entityUuid, "light selection was not captured by uuid");

        const std::filesystem::path scenePath =
            std::filesystem::temp_directory_path() / "miniengine_scene_identity_test.yaml";
        SaveEditorSceneDataToFile(captured, scenePath.string());

        std::ifstream sceneFile(scenePath);
        const std::string yaml((std::istreambuf_iterator<char>(sceneFile)), std::istreambuf_iterator<char>());
        Require(yaml.find("version: 3") != std::string::npos, "scene yaml version was not upgraded to v3");
        Require(yaml.find("selected_entity_uuid") != std::string::npos, "selected uuid missing from scene yaml");
        Require(yaml.find("entity_uuid") != std::string::npos, "entity uuid missing from scene yaml");

        const SerializedSceneData loaded = LoadEditorSceneDataFromFile(scenePath.string());
        std::unique_ptr<IEditorWorld> restoredWorld = CreateEditorWorld();
        restoredWorld->ApplySceneData(loaded);
        const SerializedSceneData restored = restoredWorld->CaptureSceneData();
        Require(restored.selectedEntityUuid == light.entityUuid, "light selection did not survive yaml round-trip");
        Require(restored.entities.size() == 2u && restored.lights.size() == 1u, "scene entity counts changed on round-trip");
        Require(restored.entities[0].entityUuid == first.entityUuid, "model uuid changed on round-trip");
        Require(restored.lights[0].entityUuid == light.entityUuid, "light uuid changed on round-trip");

        std::error_code removeError;
        std::filesystem::remove(scenePath, removeError);

        SerializedSceneData legacy{};
        legacy.entities.resize(2);
        legacy.entities[0].tagName = "Legacy A";
        legacy.entities[1].tagName = "Legacy B";
        legacy.selectedEntityIndex = 1;

        std::unique_ptr<IEditorWorld> legacyWorld = CreateEditorWorld();
        legacyWorld->ApplySceneData(legacy);
        const SerializedSceneData upgraded = legacyWorld->CaptureSceneData();
        Require(!upgraded.entities[0].entityUuid.empty(), "legacy first entity did not receive a uuid");
        Require(!upgraded.entities[1].entityUuid.empty(), "legacy second entity did not receive a uuid");
        Require(
            upgraded.entities[0].entityUuid != upgraded.entities[1].entityUuid,
            "legacy entities received duplicate uuids");
        Require(
            upgraded.selectedEntityUuid == upgraded.entities[1].entityUuid,
            "legacy selected model index did not upgrade to selected uuid");

        std::cout << "scene identity tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "scene identity tests failed: " << error.what() << '\n';
        return 1;
    }
}
