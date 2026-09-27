#include <engine/renderer/ddgi_volume.h>

#include <array>
#include <cmath>
#include <iostream>
#include <random>
#include <stdexcept>
#include <span>
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

std::vector<DdgiLevel> Levels(const glm::vec3& camera, uint32_t count)
{
    std::vector<DdgiLevel> levels;
    for (uint32_t level = 0; level < count; ++level)
    {
        levels.push_back(ComputeDdgiLevel(camera, std::ldexp(1.0f, static_cast<int>(level))));
    }
    return levels;
}

// The grid keeps the camera in its middle cells and moves by whole cells.
void LevelsCentreOnTheCamera()
{
    const DdgiLevel level = ComputeDdgiLevel(glm::vec3(0.5f, 0.5f, 0.5f), 1.0f);
    Require(level.origin == glm::ivec3(-12, -4, -12), "the camera's cell minus half the grid");

    std::mt19937 rng(5u);
    std::uniform_real_distribution<float> place(-500.0f, 500.0f);
    for (int trial = 0; trial < 1000; ++trial)
    {
        const glm::vec3 camera(place(rng), place(rng) * 0.1f, place(rng));
        for (float spacing : {1.0f, 2.0f, 4.0f, 8.0f})
        {
            const DdgiLevel grid = ComputeDdgiLevel(camera, spacing);
            const glm::vec3 low = glm::vec3(grid.origin) * spacing;
            const glm::vec3 high = glm::vec3(grid.origin + kDdgiGridSize - 1) * spacing;
            const glm::vec3 margin = glm::vec3(kDdgiGridSize / 2 - 1) * spacing;
            Require(glm::all(glm::greaterThanEqual(camera, low + margin)) && glm::all(glm::lessThanEqual(camera, high - margin + spacing)),
                    "the camera stays in the grid's middle cells");
        }
    }
}

void SlotsRoundTrip()
{
    std::mt19937 rng(9u);
    std::uniform_int_distribution<int> coordinate(-100000, 100000);
    for (int trial = 0; trial < 2000; ++trial)
    {
        const glm::ivec3 origin(coordinate(rng), coordinate(rng), coordinate(rng));
        const glm::ivec3 offset(trial % kDdgiGridSize.x, (trial / 7) % kDdgiGridSize.y, (trial / 3) % kDdgiGridSize.z);
        const glm::ivec3 coord = origin + offset;
        const glm::ivec3 slot = DdgiStorageSlot(coord);
        Require(glm::all(glm::greaterThanEqual(slot, glm::ivec3(0))) && glm::all(glm::lessThan(slot, kDdgiGridSize)), "slots lie in the grid");
        Require(DdgiSlotCoordinate(slot, origin) == coord, "a slot maps back to the coordinate it holds");
        Require(DdgiSlotFromIndex(DdgiSlotIndex(slot)) == slot, "slot indices round trip");
    }
    // Every coordinate of one grid lands in a different slot.
    std::vector<uint8_t> seen(kDdgiProbesPerLevel, 0u);
    const glm::ivec3 origin(-37, 5, 1001);
    for (int y = 0; y < kDdgiGridSize.y; ++y)
    {
        for (int z = 0; z < kDdgiGridSize.z; ++z)
        {
            for (int x = 0; x < kDdgiGridSize.x; ++x)
            {
                const uint32_t index = DdgiSlotIndex(DdgiStorageSlot(origin + glm::ivec3(x, y, z)));
                Require(index < kDdgiProbesPerLevel && seen[index] == 0u, "one grid fills every slot once");
                seen[index] = 1u;
            }
        }
    }
}

// Everything stale is updated first, finest level first; a scroll makes exactly the new slab stale.
void SchedulerUpdatesStaleProbesFirst()
{
    DdgiProbeScheduler scheduler;
    const glm::vec3 camera(10.3f, 1.2f, -4.7f);
    std::vector<DdgiLevel> levels = Levels(camera, 4);
    std::vector<uint32_t> first = scheduler.Schedule(levels, 2048);
    Require(first.size() == 2048, "the budget is used");
    for (uint32_t packed : first)
    {
        Require((packed >> 24) == 0u, "stale probes of the finest level come first");
    }
    Require(scheduler.StaleCount(0) == kDdgiProbesPerLevel - 2048, "scheduled probes are no longer stale");

    uint32_t frames = 1;
    while (scheduler.StaleCount(0) + scheduler.StaleCount(1) + scheduler.StaleCount(2) + scheduler.StaleCount(3) > 0)
    {
        scheduler.Schedule(levels, 2048);
        Require(++frames < 20, "the whole volume fills in");
    }
    Require(frames == (4 * kDdgiProbesPerLevel + 2047) / 2048, "at the budget's pace");

    // One level-0 cell along +x: the slab the grid enters is stale, nothing else.
    levels = Levels(camera + glm::vec3(1.0f, 0.0f, 0.0f), 4);
    std::vector<uint32_t> scheduledAfterScroll = scheduler.Schedule(levels, 0);
    Require(scheduledAfterScroll.empty(), "a zero budget schedules nothing");
    Require(scheduler.StaleCount(0) == static_cast<uint32_t>(kDdgiGridSize.y * kDdgiGridSize.z), "one slab of level 0 is stale");
    Require(scheduler.StaleCount(1) == 0 || scheduler.StaleCount(1) == static_cast<uint32_t>(kDdgiGridSize.y * kDdgiGridSize.z),
            "a coarser level moves by a whole slab or not at all");
    const std::vector<uint32_t> refill = scheduler.Schedule(levels, 1024);
    for (size_t index = 0; index < static_cast<size_t>(kDdgiGridSize.y * kDdgiGridSize.z); ++index)
    {
        Require((refill[index] >> 24) == 0u, "the new slab is updated first");
    }
    Require(scheduler.StaleCount(0) == 0, "and is then no longer stale");
}

// With nothing stale, each level is updated twice as often as the next coarser one.
void RoundRobinFavoursFineLevels()
{
    DdgiProbeScheduler scheduler;
    const std::vector<DdgiLevel> levels = Levels(glm::vec3(0.0f), 4);
    for (int frame = 0; frame < 20; ++frame)
    {
        scheduler.Schedule(levels, 2048);
    }
    std::array<uint32_t, 4> counts{};
    for (int frame = 0; frame < 150; ++frame)
    {
        const std::vector<uint32_t> scheduled = scheduler.Schedule(levels, 1500);
        Require(scheduled.size() >= 1497 && scheduled.size() <= 1500, "round robin spends the budget");
        for (uint32_t packed : scheduled)
        {
            ++counts[packed >> 24];
        }
    }
    for (int level = 0; level + 1 < 4; ++level)
    {
        const float ratio = static_cast<float>(counts[level]) / static_cast<float>(counts[level + 1]);
        Require(std::abs(ratio - 2.0f) < 0.05f, "level " + std::to_string(level) + " is updated twice as often as the next");
    }
}

void RotationsAreRotations()
{
    glm::mat3 previous(1.0f);
    for (uint32_t frame = 0; frame < 200; ++frame)
    {
        const glm::mat3 rotation = DdgiRayRotation(frame);
        const glm::mat3 identity = glm::transpose(rotation) * rotation;
        for (int column = 0; column < 3; ++column)
        {
            for (int row = 0; row < 3; ++row)
            {
                Require(std::abs(identity[column][row] - (column == row ? 1.0f : 0.0f)) < 1e-4f, "orthonormal");
            }
        }
        Require(std::abs(glm::determinant(rotation) - 1.0f) < 1e-4f, "not a reflection");
        Require(rotation != previous, "a new rotation every frame");
        previous = rotation;
    }
}

// An instance whose matrix changed is left out of the probe rays until it has stood still for
// kDdgiMovingInstanceFrames frames; one that never moves is always traced.
void MovingInstancesAreSkippedUntilTheySettle()
{
    DdgiMovingInstances moving;
    std::vector<glm::mat4> models(3, glm::mat4(1.0f));
    std::span<const uint8_t> skipped = moving.Update(models);
    Require(skipped.size() == 3 && skipped[0] == 0 && skipped[1] == 0 && skipped[2] == 0, "new instances are traced");

    models[1][3] = glm::vec4(0.1f, 0.0f, 0.0f, 1.0f);
    skipped = moving.Update(models);
    Require(skipped[0] == 0 && skipped[1] != 0 && skipped[2] == 0, "the one that moved is skipped");
    for (uint32_t frame = 1; frame < kDdgiMovingInstanceFrames; ++frame)
    {
        skipped = moving.Update(models);
        Require(skipped[1] != 0, "and stays skipped while it may still move");
    }
    skipped = moving.Update(models);
    Require(skipped[1] == 0, "until it has stood still long enough");

    // A car driving: a new matrix every frame keeps it out.
    for (uint32_t frame = 0; frame < 3 * kDdgiMovingInstanceFrames; ++frame)
    {
        models[2][3] = glm::vec4(0.0f, 0.0f, -0.5f * static_cast<float>(frame + 1), 1.0f);
        skipped = moving.Update(models);
        Require(skipped[2] != 0 && skipped[0] == 0, "a moving instance is skipped every frame");
    }

    // New content (a different instance count) starts over with everything traced.
    models.push_back(glm::mat4(2.0f));
    skipped = moving.Update(models);
    Require(skipped.size() == 4 && skipped[2] == 0 && skipped[3] == 0, "new content is traced");
}

// The probes keep their settled hysteresis while the lighting holds, and drop to the fast one for a
// second after it changes, the sun's direction or colour or the sky.
void HysteresisDropsWhenTheLightingChanges()
{
    DdgiAdaptiveHysteresis adaptive;
    std::vector<glm::vec4> lighting = {glm::vec4(0.3f, -0.9f, 0.2f, 0.0f), glm::vec4(1.0f, 0.95f, 0.9f, 120000.0f)};
    Require(adaptive.Update(lighting, 1.0f / 60.0f, 0.97f) == 0.97f, "the first frame has nothing to compare with");
    Require(adaptive.Update(lighting, 1.0f / 60.0f, 0.97f) == 0.97f, "steady lighting keeps the setting");

    lighting[0].x += 0.01f;
    Require(adaptive.Update(lighting, 1.0f / 60.0f, 0.97f) == kDdgiFastHysteresis, "a turning sun speeds the probes up");
    float elapsed = 0.0f;
    while (elapsed + 0.1f < kDdgiFastSeconds)
    {
        Require(adaptive.Update(lighting, 0.1f, 0.97f) == kDdgiFastHysteresis, "for a second after the change");
        elapsed += 0.1f;
    }
    Require(adaptive.Update(lighting, 0.2f, 0.97f) == 0.97f, "then settles again");

    // Tiny drift (the sun's transmittance as the camera climbs a little) is not a change.
    lighting[1].w *= 1.0f + 1e-5f;
    Require(adaptive.Update(lighting, 1.0f / 60.0f, 0.97f) == 0.97f, "drift within the tolerance");

    // The setting wins when it is already faster.
    lighting[1].y = 0.5f;
    Require(adaptive.Update(lighting, 1.0f / 60.0f, 0.5f) == 0.5f, "never slower than the setting");

    // A light added or removed changes the lighting too.
    lighting.push_back(glm::vec4(1.0f));
    Require(adaptive.Update(lighting, 1.0f / 60.0f, 0.97f) == kDdgiFastHysteresis, "a new light");
}
}

int main()
{
    try
    {
        LevelsCentreOnTheCamera();
        SlotsRoundTrip();
        SchedulerUpdatesStaleProbesFirst();
        RoundRobinFavoursFineLevels();
        RotationsAreRotations();
        MovingInstancesAreSkippedUntilTheySettle();
        HysteresisDropsWhenTheLightingChanges();
    }
    catch (const std::exception& error)
    {
        std::cerr << "ddgi volume tests failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "ddgi volume tests passed\n";
    return 0;
}
