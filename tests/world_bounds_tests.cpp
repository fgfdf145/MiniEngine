#include <engine/logic/editor_world.h>
#include <engine/logic/world_bounds.h>

#include <glm/glm.hpp>

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

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

bool Near(const glm::vec3& actual, const glm::vec3& expected)
{
    return glm::all(glm::lessThan(glm::abs(actual - expected), glm::vec3(1e-4f)));
}

std::string Text(const glm::vec3& value)
{
    return "(" + std::to_string(value.x) + ", " + std::to_string(value.y) + ", " + std::to_string(value.z) + ")";
}

// A model placed away from the origin frames where it is, not where its local bounds are.
void FollowsTranslationAndScale()
{
    std::unique_ptr<IEditorWorld> world = CreateEditorWorld();
    SerializedEntityData data{};
    data.tagName = "Placed";
    data.transform.translation = glm::vec3(10.0f, 2.0f, -5.0f);
    data.transform.scale = glm::vec3(2.0f);
    const entt::entity entity = world->CreateEntity(data);

    const ModelBoundsComponent& local = world->GetModelBounds(entity);
    glm::vec3 minBounds{};
    glm::vec3 maxBounds{};
    Require(ComputeWorldModelBounds(*world, entity, minBounds, maxBounds), "an entity with bounds has world bounds");
    const glm::vec3 expectedMin = data.transform.translation + local.minBounds * 2.0f;
    const glm::vec3 expectedMax = data.transform.translation + local.maxBounds * 2.0f;
    Require(Near(minBounds, expectedMin), "world min is " + Text(minBounds) + ", expected " + Text(expectedMin));
    Require(Near(maxBounds, expectedMax), "world max is " + Text(maxBounds) + ", expected " + Text(expectedMax));
}

// A quarter turn about Y swaps the x and z extents of the box.
void FollowsRotation()
{
    std::unique_ptr<IEditorWorld> world = CreateEditorWorld();
    SerializedEntityData data{};
    data.tagName = "Turned";
    data.transform.rotationDegrees = glm::vec3(0.0f, 90.0f, 0.0f);
    data.transform.scale = glm::vec3(1.0f, 1.0f, 3.0f);
    const entt::entity entity = world->CreateEntity(data);

    const ModelBoundsComponent& local = world->GetModelBounds(entity);
    glm::vec3 minBounds{};
    glm::vec3 maxBounds{};
    Require(ComputeWorldModelBounds(*world, entity, minBounds, maxBounds), "an entity with bounds has world bounds");
    const glm::vec3 localExtent = local.maxBounds - local.minBounds;
    const glm::vec3 worldExtent = maxBounds - minBounds;
    Require(std::fabs(worldExtent.x - localExtent.z * 3.0f) < 1e-4f, "rotated x extent is the scaled z extent");
    Require(std::fabs(worldExtent.z - localExtent.x) < 1e-4f, "rotated z extent is the x extent");
}
}

int main()
{
    try
    {
        FollowsTranslationAndScale();
        FollowsRotation();
    }
    catch (const std::exception& error)
    {
        std::cerr << "world bounds tests failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "world bounds tests passed\n";
    return 0;
}
