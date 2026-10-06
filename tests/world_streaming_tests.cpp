#include <engine/editor/services/world_streaming_service.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
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

// A cell with high detail and LOD, a LOD-only cell (always shows its LOD), and a far-only LOD (GTA III's
// whole-island models: shown only from beyond the radius, nothing within it).
void TestTargets()
{
    StreamedCell both;
    both.highDetailPath = "hd/a.gltf";
    both.lodPath = "lod/a.gltf";
    StreamedCell lodOnly;
    lodOnly.lodPath = "lod/b.gltf";
    StreamedCell farOnly;
    farOnly.lodPath = "lod/island.gltf";
    farOnly.farOnly = true;

    for (StreamedCell* cell : {&both, &lodOnly, &farOnly})
    {
        cell->wantHighDetail = false;
    }
    Require(WorldStreamingService::TargetOf(both) == StreamedCell::Shown::Lod, "far away, a cell shows its LOD");
    Require(WorldStreamingService::TargetOf(lodOnly) == StreamedCell::Shown::Lod, "a LOD-only cell shows it far away");
    Require(WorldStreamingService::TargetOf(farOnly) == StreamedCell::Shown::Lod, "a far-only LOD shows from far away");

    for (StreamedCell* cell : {&both, &lodOnly, &farOnly})
    {
        cell->wantHighDetail = true;
    }
    Require(WorldStreamingService::TargetOf(both) == StreamedCell::Shown::HighDetail, "near, a cell shows its high detail");
    Require(WorldStreamingService::TargetOf(lodOnly) == StreamedCell::Shown::Lod, "a LOD-only cell keeps its LOD up close");
    Require(WorldStreamingService::TargetOf(farOnly) == StreamedCell::Shown::None, "a far-only LOD is gone up close");
}

void TestManifestFarOnly()
{
    const std::filesystem::path folder = std::filesystem::temp_directory_path() / "miniengine_world_streaming_tests";
    std::filesystem::create_directories(folder);
    const std::filesystem::path manifest = folder / "world.stream.yaml";
    {
        std::ofstream file(manifest);
        file << "version: 1\n"
                "cell_size: 500\n"
                "cells:\n"
                "  - name: city_0_0\n"
                "    hd: hd/city_0_0.gltf\n"
                "    lod: lod/city_0_0.gltf\n"
                "    bounds_min: [0, 0, 0]\n"
                "    bounds_max: [500, 50, 500]\n"
                "  - name: city_island\n"
                "    lod: lod/city_island.gltf\n"
                "    far_only: true\n"
                "    bounds_min: [-100, 0, -100]\n"
                "    bounds_max: [900, 80, 900]\n";
    }
    SceneStreamingWorld world;
    world.manifest = manifest.generic_string();
    world.loadRadius = 3500.0f;
    world.unloadRadius = 3700.0f;
    const std::vector<StreamedCell> cells = WorldStreamingService::ReadCells(world);
    std::filesystem::remove_all(folder);

    Require(cells.size() == 2, "two cells");
    Require(!cells[0].farOnly && cells[1].farOnly, "far_only is read, and false when absent");
    Require(cells[1].highDetailPath.empty() && !cells[1].lodPath.empty(), "the island cell has only a LOD");
    Require(cells[1].loadRadius == 3500.0f && cells[1].unloadRadius == 3700.0f, "cells take the world's radii");
}

// A high-detail-only world: every cell shows its high detail however far away it is, and nothing shows a
// LOD (a LOD-only or far-only cell shows nothing).
void TestHighDetailOnly()
{
    const std::filesystem::path folder = std::filesystem::temp_directory_path() / "miniengine_world_streaming_hd_only_tests";
    std::filesystem::create_directories(folder);
    const std::filesystem::path manifest = folder / "world.stream.yaml";
    {
        std::ofstream file(manifest);
        file << "version: 1\n"
                "cell_size: 500\n"
                "cells:\n"
                "  - name: city_0_0\n"
                "    hd: hd/city_0_0.gltf\n"
                "    lod: lod/city_0_0.gltf\n"
                "    bounds_min: [0, 0, 0]\n"
                "    bounds_max: [500, 50, 500]\n"
                "  - name: city_lod_only\n"
                "    lod: lod/city_lod_only.gltf\n"
                "    bounds_min: [500, 0, 0]\n"
                "    bounds_max: [1000, 50, 500]\n"
                "  - name: city_island\n"
                "    lod: lod/city_island.gltf\n"
                "    far_only: true\n"
                "    bounds_min: [-100, 0, -100]\n"
                "    bounds_max: [900, 80, 900]\n";
    }
    SceneStreamingWorld world;
    world.manifest = manifest.generic_string();
    world.highDetailOnly = true;
    std::vector<StreamedCell> cells = WorldStreamingService::ReadCells(world);
    std::filesystem::remove_all(folder);

    Require(cells.size() == 3, "three cells");
    for (StreamedCell& cell : cells)
    {
        Require(cell.lodPath.empty(), "a high-detail-only world keeps no LOD model");
        const float farAway = WorldStreamingService::HorizontalDistance(glm::vec3(1.0e6f, 0.0f, 1.0e6f), cell.boundsMin, cell.boundsMax);
        Require(farAway <= cell.loadRadius, "a high-detail-only cell wants its high detail from any distance");
        cell.wantHighDetail = true;
    }
    Require(WorldStreamingService::TargetOf(cells[0]) == StreamedCell::Shown::HighDetail, "a cell shows its high detail");
    Require(WorldStreamingService::TargetOf(cells[1]) == StreamedCell::Shown::None, "a LOD-only cell shows nothing");
    Require(WorldStreamingService::TargetOf(cells[2]) == StreamedCell::Shown::None, "a far-only cell shows nothing");
}

constexpr uint64_t kMiB = uint64_t{1} << 20;

// Ten cells in high detail at 100, 200 ... 1000 m, 100 MB each by the world's 1000 MB.
GpuMemoryReport ReportWithHeadroom(int64_t headroomMiB)
{
    GpuMemoryReport report;
    report.serial = 1;
    report.budget = 6000 * kMiB;
    report.reserve = 1000 * kMiB;
    report.worldBytes = 1000 * kMiB;
    report.usage = static_cast<uint64_t>(5000 - headroomMiB) * kMiB;
    return report;
}

std::vector<float> Distances(float first, float step, int count)
{
    std::vector<float> distances;
    for (int index = 0; index < count; ++index)
    {
        distances.push_back(first + step * static_cast<float>(index));
    }
    return distances;
}

void TestBudgetRadius()
{
    const std::vector<float> shown = Distances(100.0f, 100.0f, 10);
    const std::vector<float> farther = Distances(1100.0f, 100.0f, 10);
    const auto next = [&](float radius, int64_t headroomMiB, bool outOfMemory, bool mayGrow)
    {
        return WorldStreamingService::NextBudgetRadius(radius, ReportWithHeadroom(headroomMiB), outOfMemory, mayGrow, shown, farther);
    };

    Require(
        WorldStreamingService::NextBudgetRadius(1000.0f, GpuMemoryReport{}, false, true, shown, farther) == 1000.0f,
        "nothing changes before the first measurement");

    // 100 MB over: 100 + 192 MB of margin is three cells, so the cells at 800, 900 and 1000 m go, and the
    // radius ends below 800 m by more than the hysteresis that would keep the one at 800 m.
    const float shrunk = next(1000.0f, -100, false, true);
    Require(shrunk < 800.0f - WorldStreamingService::kBudgetRadiusHysteresis && shrunk > 700.0f, "over budget, the farthest cells go");

    Require(next(1000.0f, -100000, false, true) == WorldStreamingService::kMinBudgetRadius, "the radius never goes below its minimum");

    // An upload that ran out of memory takes at least an eighth of the cells (one of ten) even with room.
    const float afterFailure = next(1000.0f, 300, true, true);
    Require(afterFailure < 1000.0f - WorldStreamingService::kBudgetRadiusHysteresis && afterFailure > 900.0f, "a failed upload sheds a cell");

    // Between 0 and 384 MB free nothing moves; the band keeps a wrong cell cost from oscillating.
    Require(next(1000.0f, 0, false, true) == 1000.0f, "no shrink at exactly the budget");
    Require(next(1000.0f, 300, false, true) == 1000.0f, "no growth with less than the grow headroom");

    // 1000 MB free: (1000 - 192) / 100 / 2 = 4 cells more, the ones at 1100 ... 1400 m.
    Require(next(1000.0f, 1000, false, true) == 1400.0f, "with room the radius takes in the next cells, half the room at a time");
    Require(next(1000.0f, 1000, false, false) == 1000.0f, "growth waits for the last change to be drawn");

    // The radius already sends the cells beyond 525 m away (five of them, 500 MB), so 300 MB over is
    // not over once they are gone: no second shrink before the first shows in the measurement.
    Require(next(500.0f, -300, false, true) == 500.0f, "cells on their way out count as freed");
    // Likewise cells within the radius that have not loaded yet count as used: 600 MB free, but the
    // radius of 1300 m brings in three more cells (300 MB), which leaves less than the grow headroom.
    Require(next(1300.0f, 600, false, true) == 1300.0f, "cells on their way in count as used");
}
}

int main()
{
    try
    {
        TestTargets();
        TestManifestFarOnly();
        TestHighDetailOnly();
        TestBudgetRadius();
    }
    catch (const std::exception& error)
    {
        std::cerr << "world_streaming_tests failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "world_streaming_tests passed\n";
    return 0;
}
