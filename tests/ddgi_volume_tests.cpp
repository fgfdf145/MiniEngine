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
    Require(level.origin == -(kDdgiGridSize / 2 - 1), "the camera's cell minus one less than half the grid");

    std::mt19937 rng(5u);
    std::uniform_real_distribution<float> place(-500.0f, 500.0f);
    for (int trial = 0; trial < 1000; ++trial)
    {
        const glm::vec3 camera(place(rng), place(rng) * 0.1f, place(rng));
        for (float spacing : {1.0f, 2.0f, 4.0f, 8.0f})
        {
            const DdgiLevel grid = ComputeDdgiLevel(camera, spacing);
            // What ddgi_common.glsl's fade assumes: every point within half the grid minus one cell of
            // the camera lies between the grid's first and last probes.
            const glm::vec3 low = glm::vec3(grid.origin) * spacing;
            const glm::vec3 high = glm::vec3(grid.origin + kDdgiGridSize - 1) * spacing;
            const glm::vec3 reach = glm::vec3(kDdgiGridSize / 2 - 1) * spacing;
            Require(glm::all(glm::greaterThanEqual(camera - reach, low)) && glm::all(glm::lessThanEqual(camera + reach, high)),
                    "the grid reaches half its size minus one cell around the camera");
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
        Require(++frames < static_cast<int>(4 * kDdgiProbesPerLevel / 2048 + 2), "the whole volume fills in");
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

// A level that has converged refreshes a quarter as often; a scroll or Unsettle brings it back.
void ConvergedLevelsRefreshLess()
{
    DdgiProbeScheduler scheduler;
    const glm::vec3 camera(0.0f);
    std::vector<DdgiLevel> levels = Levels(camera, 4);
    // Never settles at the default hysteresis of 1.
    for (int frame = 0; frame < 400; ++frame)
    {
        scheduler.Schedule(levels, 2048);
    }
    Require(!scheduler.Settled(0), "without a hysteresis nothing settles");

    // At 0.97 a probe needs log(0.01) / log(0.97), about 151 updates. Level 0 gets 8/15 of the budget,
    // so it settles after about 151 * probes / (2048 * 8 / 15) frames, and level 3, with 1/15, long
    // after.
    const int expected = static_cast<int>(151.2 * kDdgiProbesPerLevel / (2048.0 * 8.0 / 15.0));
    int frames = 0;
    while (!scheduler.Settled(0))
    {
        scheduler.Schedule(levels, 2048, 0.97f);
        Require(++frames < expected * 115 / 100, "the finest level settles");
    }
    Require(frames > expected * 85 / 100, "but not before its probes had their updates");
    Require(!scheduler.Settled(3), "the coarsest level, updated least, has not settled yet");

    std::array<uint32_t, 4> counts{};
    for (uint32_t packed : scheduler.Schedule(levels, 2048, 0.97f))
    {
        ++counts[packed >> 24];
    }
    Require(counts[0] < 300 && counts[1] > 500, "a settled level gets a quarter of its share");

    scheduler.Unsettle();
    Require(!scheduler.Settled(0), "Unsettle starts every level over");

    for (int frame = 0; frame < expected * 115 / 100; ++frame)
    {
        scheduler.Schedule(levels, 2048, 0.97f);
    }
    Require(scheduler.Settled(0), "settled again");
    levels = Levels(camera + glm::vec3(1.0f, 0.0f, 0.0f), 4);
    scheduler.Schedule(levels, 2048, 0.97f);
    Require(!scheduler.Settled(0), "a scroll brings new probes, which start the level over");
}

void LightingWatchReportsChanges()
{
    DdgiLightingWatch watch;
    std::vector<glm::vec4> lighting = {glm::vec4(0.0f, -1.0f, 0.0f, 0.0f)};
    watch.Update(lighting);
    watch.Update(lighting);
    Require(!watch.Changed(), "still lighting is no change");
    lighting[0].x = 0.5f;
    watch.Update(lighting);
    Require(watch.Changed(), "a turned light is");
    watch.Update(lighting);
    Require(!watch.Changed(), "for the frame it turned only");
}

// With nothing stale, each level is updated twice as often as the next coarser one.
void RoundRobinFavoursFineLevels()
{
    DdgiProbeScheduler scheduler;
    const std::vector<DdgiLevel> levels = Levels(glm::vec3(0.0f), 4);
    // Long enough to fill every level, so nothing is stale.
    for (int frame = 0; frame < static_cast<int>(4 * kDdgiProbesPerLevel / 2048 + 2); ++frame)
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

// Each change in the lighting starts a new epoch, which the probes compare with the one they
// recorded; drift within the tolerance does not.
void LightingEpochCountsChanges()
{
    DdgiLightingWatch watch;
    std::vector<glm::vec4> lighting = {glm::vec4(0.3f, -0.9f, 0.2f, 0.0f), glm::vec4(1.0f, 0.95f, 0.9f, 120000.0f)};
    Require(!watch.Update(lighting) && watch.Epoch() == 0u, "the first frame has nothing to compare with");
    Require(!watch.Update(lighting) && watch.Epoch() == 0u, "steady lighting keeps the epoch");

    lighting[0].x += 0.01f;
    Require(watch.Update(lighting) && watch.Epoch() == 1u, "a turning sun starts a new epoch");
    Require(!watch.Update(lighting) && watch.Epoch() == 1u, "which then holds");

    // Tiny drift (the sun's transmittance as the camera climbs a little) is not a change.
    lighting[1].w *= 1.0f + 1e-5f;
    Require(!watch.Update(lighting), "drift within the tolerance");

    // A sun the time of day moves a little each frame adds up to a change.
    int frames = 0;
    while (!watch.Update(lighting) && frames < 1000)
    {
        lighting[0].z += 1e-4f;
        ++frames;
    }
    Require(watch.Epoch() == 2u && frames > 10 && frames < 100, "slow drift starts an epoch once it adds up");

    // A light added or removed changes the lighting too.
    lighting.push_back(glm::vec4(1.0f));
    Require(watch.Update(lighting) && watch.Epoch() == 3u, "a new light");

    for (int change = 0; change < 254; ++change)
    {
        lighting[0].y += 0.01f;
        watch.Update(lighting);
    }
    Require(watch.Epoch() == 1u, "the epoch wraps at 256, as the probes record it");
}

// Fills every level so nothing is stale, and returns the last frame's schedule.
std::vector<uint32_t> Fill(DdgiProbeScheduler& scheduler, std::span<const DdgiLevel> levels)
{
    std::vector<uint32_t> scheduled;
    for (int frame = 0; frame < static_cast<int>(levels.size() * kDdgiProbesPerLevel / 2048 + 2); ++frame)
    {
        scheduled = scheduler.Schedule(levels, 2048);
    }
    return scheduled;
}

uint32_t Report(const DdgiLevel& level, uint32_t slotIndex, uint32_t bits)
{
    return bits | DdgiFeedbackCoordinate(DdgiSlotCoordinate(DdgiSlotFromIndex(slotIndex), level.origin));
}

// A probe whose light changed is updated again within the next frames, ahead of the round robin, and
// so are its neighbours; the change unsettles its level.
void ChangedProbesComeBackSoon()
{
    DdgiProbeScheduler scheduler;
    const std::vector<DdgiLevel> levels = Levels(glm::vec3(0.0f), 4);
    Fill(scheduler, levels);

    const glm::ivec3 slot(5, 6, 7);
    const uint32_t index = DdgiSlotIndex(slot);
    const uint32_t neighbour = DdgiSlotIndex(slot + glm::ivec3(0, 1, 0));
    const uint32_t far = DdgiSlotIndex(glm::ivec3(20, 6, 7));
    const std::vector<uint32_t> scheduled = {PackDdgiProbe(2, index)};
    scheduler.ApplyFeedback(scheduled, std::vector<uint32_t>{Report(levels[2], index, kDdgiFeedbackChanged)});
    Require(scheduler.Hot(2, index) && scheduler.Hot(2, neighbour) && !scheduler.Hot(2, far), "the probe and its neighbours are hot");

    uint32_t probeUpdates = 0;
    uint32_t neighbourUpdates = 0;
    for (int frame = 0; frame < static_cast<int>(kDdgiHotUpdates); ++frame)
    {
        for (const uint32_t packed : scheduler.Schedule(levels, 2048))
        {
            probeUpdates += packed == PackDdgiProbe(2, index) ? 1u : 0u;
            neighbourUpdates += packed == PackDdgiProbe(2, neighbour) ? 1u : 0u;
        }
    }
    Require(probeUpdates == kDdgiHotUpdates, "the changed probe is updated every frame while hot");
    Require(neighbourUpdates >= kDdgiNeighbourHotUpdates, "its neighbours too, for fewer frames");
    Require(!scheduler.Hot(2, index) && !scheduler.Hot(2, neighbour), "then they cool down");

    // A report for a coordinate the slot no longer holds (a scroll happened since) is ignored.
    DdgiProbeScheduler other;
    Fill(other, levels);
    const DdgiLevel moved = ComputeDdgiLevel(glm::vec3(300.0f, 0.0f, 0.0f), 4.0f);
    other.ApplyFeedback(scheduled, std::vector<uint32_t>{Report(moved, index, kDdgiFeedbackChanged)});
    Require(!other.Hot(2, index), "a stale report");
}

// Empty and buried probes take a share of the round robin's turns; the budget goes to the others.
void ColdProbesUpdateLess()
{
    DdgiProbeScheduler scheduler;
    const std::vector<DdgiLevel> levels = Levels(glm::vec3(0.0f), 1);
    Fill(scheduler, levels);
    // The upper three quarters of the level are empty air.
    std::vector<uint32_t> scheduled;
    std::vector<uint32_t> feedback;
    for (uint32_t index = 0; index < kDdgiProbesPerLevel; ++index)
    {
        const bool empty = DdgiSlotFromIndex(index).y >= kDdgiGridSize.y / 4;
        scheduled.push_back(PackDdgiProbe(0, index));
        feedback.push_back(Report(levels[0], index, empty ? kDdgiFeedbackEmpty : 0u));
    }
    scheduler.ApplyFeedback(scheduled, feedback);
    Require(scheduler.Cold(0, DdgiSlotIndex(glm::ivec3(0, kDdgiGridSize.y - 1, 0))) && !scheduler.Cold(0, 0), "empty probes are cold");
    {
        DdgiProbeScheduler buried;
        Fill(buried, levels);
        buried.ApplyFeedback(std::vector<uint32_t>{PackDdgiProbe(0, 0)}, std::vector<uint32_t>{Report(levels[0], 0, kDdgiFeedbackInactive)});
        Require(!buried.Cold(0, 0), "a probe inside geometry keeps its turns");
    }

    std::vector<uint32_t> counts(kDdgiProbesPerLevel, 0u);
    constexpr int kFrames = 400;
    for (int frame = 0; frame < kFrames; ++frame)
    {
        for (const uint32_t packed : scheduler.Schedule(levels, 1024))
        {
            ++counts[packed & 0xffffffu];
        }
    }
    double warm = 0.0;
    double cold = 0.0;
    for (uint32_t index = 0; index < kDdgiProbesPerLevel; ++index)
    {
        (DdgiSlotFromIndex(index).y >= kDdgiGridSize.y / 4 ? cold : warm) += counts[index];
    }
    warm /= kDdgiProbesPerLevel / 4;
    cold /= kDdgiProbesPerLevel * 3 / 4;
    Require(std::abs(warm / cold - kDdgiColdRateDivisor) < 1.0, "a cold probe updates kDdgiColdRateDivisor times less often");
    // 1024 probes a frame over 4096 warm probes and 12288 cold ones at a fraction of the rate.
    const double perFrame = 1024.0 / (4096.0 + 12288.0 / kDdgiColdRateDivisor);
    Require(std::abs(warm / kFrames - perFrame) < 0.02, "the budget the cold probes leave goes to the rest");

    scheduler.GeometryChanged();
    Require(!scheduler.Cold(0, DdgiSlotIndex(glm::ivec3(0, kDdgiGridSize.y - 1, 0))), "new geometry: every probe is looked at again");
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
        LightingEpochCountsChanges();
        ConvergedLevelsRefreshLess();
        LightingWatchReportsChanges();
        ChangedProbesComeBackSoon();
        ColdProbesUpdateLess();
    }
    catch (const std::exception& error)
    {
        std::cerr << "ddgi volume tests failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "ddgi volume tests passed\n";
    return 0;
}
