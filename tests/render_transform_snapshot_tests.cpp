#include <engine/logic/editor_world.h>
#include <engine/renderer/render_transform_snapshot.h>
#include <engine/renderer/renderer_world.h>

#include <glm/ext/matrix_transform.hpp>

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// main() stays in the global namespace; everything it drives lives in me::.
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

entt::entity Place(IEditorWorld& world, const char* name, glm::vec3 translation)
{
    SerializedEntityData data{};
    data.tagName = name;
    data.transform.translation = translation;
    return world.CreateEntity(data);
}

std::vector<CpuRenderSubmesh> Submeshes(entt::entity entity, size_t count)
{
    std::vector<CpuRenderSubmesh> submeshes(count);
    for (CpuRenderSubmesh& submesh : submeshes)
    {
        submesh.entity = entity;
    }
    return submeshes;
}

// An unchanged list shares one snapshot; a change makes a new one and leaves the old as it was.
void SnapshotsAreSharedUntilTheListChanges()
{
    std::unique_ptr<IEditorWorld> world = CreateEditorWorld();
    RendererWorld renderWorld;
    renderWorld.SetSceneWorld(*world);
    const entt::entity a = Place(*world, "A", glm::vec3(1.0f, 0.0f, 0.0f));
    const entt::entity b = Place(*world, "B", glm::vec3(2.0f, 0.0f, 0.0f));
    renderWorld.SetRenderSubmeshes(Submeshes(a, 2));

    const std::shared_ptr<const CpuRenderSubmeshList> first = renderWorld.SnapshotRenderSubmeshes();
    Require(first == renderWorld.SnapshotRenderSubmeshes(), "an unchanged list must share its snapshot");
    Require(first->size() == 2, "the snapshot holds the list");

    renderWorld.ReplaceEntityRenderSubmeshes(b, Submeshes(b, 3));
    const std::shared_ptr<const CpuRenderSubmeshList> second = renderWorld.SnapshotRenderSubmeshes();
    Require(second != first, "a changed list must make a new snapshot");
    Require(first->size() == 2, "the old snapshot must stay as it was");
    Require(second->size() == 5, "the new snapshot holds the change");
    // The submeshes both lists name are the same objects.
    Require((*first)[0] == (*second)[0], "a submesh both snapshots hold must be shared, not copied");

    renderWorld.RemoveEntityRenderSubmeshes(a);
    Require(renderWorld.SnapshotRenderSubmeshes()->size() == 3, "removal makes a new snapshot");
    Require(!renderWorld.RemoveEntityRenderSubmeshes(a), "removing nothing is no change");
}

// The snapshot's matrices are the world's at capture, with the submesh local transforms on top.
void TransformsMatchTheWorldAtCapture()
{
    std::unique_ptr<IEditorWorld> world = CreateEditorWorld();
    RendererWorld renderWorld;
    renderWorld.SetSceneWorld(*world);
    const entt::entity car = Place(*world, "Car", glm::vec3(5.0f, 0.0f, -3.0f));
    const entt::entity house = Place(*world, "House", glm::vec3(-7.0f, 1.0f, 2.0f));
    std::vector<CpuRenderSubmesh> submeshes = Submeshes(car, 3);
    std::vector<CpuRenderSubmesh> houseSubmeshes = Submeshes(house, 1);
    submeshes.insert(submeshes.end(), houseSubmeshes.begin(), houseSubmeshes.end());
    renderWorld.SetRenderSubmeshes(std::move(submeshes));
    const glm::mat4 wheel = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.3f, 1.2f));
    renderWorld.SetSubmeshLocalTransforms(car, {glm::mat4(1.0f), wheel});
    world->FlushDirtyTransforms();

    RenderTransformSnapshot snapshot;
    snapshot.Capture(renderWorld, *renderWorld.SnapshotRenderSubmeshes());
    Require(snapshot.GetEntityCount() == 2, "two entities");
    for (const entt::entity entity : {car, house})
    {
        for (uint32_t ordinal = 0; ordinal < 3; ++ordinal)
        {
            Require(
                snapshot.GetSubmeshModelMatrix(entity, ordinal) == renderWorld.GetSubmeshModelMatrix(entity, ordinal),
                "submesh " + std::to_string(ordinal) + " must match the world");
        }
    }

    // The world moves on; the snapshot keeps the frame it was taken for.
    const glm::mat4 captured = snapshot.GetSubmeshModelMatrix(car, 1);
    world->EditTransform(car).translation = glm::vec3(100.0f, 0.0f, 0.0f);
    world->FlushDirtyTransforms();
    Require(snapshot.GetSubmeshModelMatrix(car, 1) == captured, "the snapshot must not follow the world");
    snapshot.Capture(renderWorld, *renderWorld.SnapshotRenderSubmeshes());
    Require(snapshot.GetSubmeshModelMatrix(car, 1) == renderWorld.GetSubmeshModelMatrix(car, 1), "a new capture follows it");
}

// A deleted entity, and one the list no longer names, are not in the snapshot.
void RemovedEntitiesAreLeftOut()
{
    std::unique_ptr<IEditorWorld> world = CreateEditorWorld();
    RendererWorld renderWorld;
    renderWorld.SetSceneWorld(*world);
    const entt::entity kept = Place(*world, "Kept", glm::vec3(0.0f));
    const entt::entity deleted = Place(*world, "Deleted", glm::vec3(1.0f));
    const entt::entity unlisted = Place(*world, "Unlisted", glm::vec3(2.0f));
    std::vector<CpuRenderSubmesh> submeshes = Submeshes(kept, 1);
    submeshes.push_back(Submeshes(deleted, 1)[0]);
    renderWorld.SetRenderSubmeshes(std::move(submeshes));
    world->DestroyEntity(deleted);

    RenderTransformSnapshot snapshot;
    snapshot.Capture(renderWorld, *renderWorld.SnapshotRenderSubmeshes());
    Require(snapshot.Contains(kept), "a listed entity is in the snapshot");
    Require(!snapshot.Contains(deleted), "a deleted entity must not be in the snapshot");
    Require(!snapshot.Contains(unlisted), "an entity the list does not name must not be in the snapshot");
    Require(!snapshot.Contains(entt::null), "null is never in the snapshot");
    Require(snapshot.GetSubmeshModelMatrix(deleted, 0) == glm::mat4(1.0f), "a missing entity draws at the identity");

    // A recycled index with a new version is another entity.
    const entt::entity reused = Place(*world, "Reused", glm::vec3(3.0f));
    if (entt::to_entity(reused) == entt::to_entity(deleted))
    {
        Require(!snapshot.Contains(reused), "a recycled index must not match the old capture");
    }

    renderWorld.ClearRenderSubmeshes();
    snapshot.Capture(renderWorld, *renderWorld.SnapshotRenderSubmeshes());
    Require(!snapshot.Contains(kept) && snapshot.GetEntityCount() == 0, "a new capture forgets the last");
}
}

int main()
{
    try
    {
        SnapshotsAreSharedUntilTheListChanges();
        TransformsMatchTheWorldAtCapture();
        RemovedEntitiesAreLeftOut();
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << "render transform snapshot tests passed\n";
    return 0;
}
