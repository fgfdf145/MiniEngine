#pragma once

#include <engine/scene/material_graph.h>

#include <glm/glm.hpp>

#include <TaskScheduler.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace me
{

// A texture as the preview samples it: RGBA8, at most kMaterialPreviewMaxTextureSize on a side, with
// its mip chain. Immutable once loaded, shared by the renders in flight.
struct MaterialPreviewTexture
{
    struct Level
    {
        int width = 0;
        int height = 0;
        std::vector<uint8_t> rgba;
    };
    std::vector<Level> levels;

    int Width() const
    {
        return levels.empty() ? 0 : levels.front().width;
    }
    int Height() const
    {
        return levels.empty() ? 0 : levels.front().height;
    }

    // The texel values at uv, filtered as the sampler says: bilinear (or nearest) in a level,
    // between levels at lod (log2 of level-0 texels per footprint) as its mip filter says, wrapped
    // as it says. With srgb the colour channels are decoded before filtering, as a GPU decodes an
    // _SRGB format; alpha stays linear. dither in [0, 1) picks one of the two levels a linear mip
    // filter blends, the lower with probability 1 - fraction: over a pixel's samples that averages
    // to the blend at half the cost. Negative blends the two.
    glm::vec4 Sample(glm::vec2 uv, float lod, const TextureSampler& sampler, bool srgb, float dither = -1.0f) const;
};

// Decodes RGBA8 (the levels of an image, rows top-down) into the preview's form: halved until it
// fits kMaterialPreviewMaxTextureSize, then the mip chain by 2x2 box filtering.
inline constexpr int kMaterialPreviewMaxTextureSize = 1024;
MaterialPreviewTexture BuildMaterialPreviewTexture(int width, int height, const uint8_t* rgba);

// The sRGB transfer function's decode, exactly, for 8-bit values.
float MaterialPreviewSrgbToLinear(uint8_t value);

// The preview's textures by path: loaded in the background on the task system (low priority, many
// at once), kept until the cache is cleared. Used from one thread (the editor's main thread); the
// renders take the shared pointers it hands out.
class MaterialPreviewTextureCache
{
  public:
    MaterialPreviewTextureCache() = default;
    MaterialPreviewTextureCache(const MaterialPreviewTextureCache&) = delete;
    MaterialPreviewTextureCache& operator=(const MaterialPreviewTextureCache&) = delete;
    // Waits for the loads in flight.
    ~MaterialPreviewTextureCache();

    // The texture, or nullptr while it loads, when it failed to load, or before it was requested.
    std::shared_ptr<const MaterialPreviewTexture> Find(const std::string& path) const;
    // Starts loading the paths not loaded or loading yet. Empty paths are ignored.
    void Request(const std::vector<std::string>& paths);
    // Takes in the textures that finished loading and starts the next batch. True when any arrived.
    bool Poll();
    // Loads everything requested, waiting (tests).
    void WaitForAll();
    size_t PendingCount() const;
    size_t LoadedCount() const
    {
        return m_textures.size();
    }
    // Drops every texture (a new model). Waits for the loads in flight.
    void Clear();

  private:
    struct Batch : enki::ITaskSet
    {
        std::vector<std::string> paths;
        std::vector<std::shared_ptr<const MaterialPreviewTexture>> results;
        void ExecuteRange(enki::TaskSetPartition range, uint32_t threadNumber) override;
    };
    void StartBatch();

    std::unordered_map<std::string, std::shared_ptr<const MaterialPreviewTexture>> m_textures;
    // Paths that failed to load: not asked for again.
    std::unordered_set<std::string> m_failed;
    std::unordered_set<std::string> m_inFlight;
    std::vector<std::string> m_queued;
    std::unique_ptr<Batch> m_batch;
};
}
