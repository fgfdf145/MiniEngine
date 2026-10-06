#include <engine/editor/services/world_streaming_service.h>

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
}

int main()
{
    try
    {
        TestTargets();
        TestManifestFarOnly();
    }
    catch (const std::exception& error)
    {
        std::cerr << "world_streaming_tests failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "world_streaming_tests passed\n";
    return 0;
}
