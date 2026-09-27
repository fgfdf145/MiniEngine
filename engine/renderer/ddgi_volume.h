#pragma once

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace me
{

// The CPU half of the cascaded DDGI probe volume (docs/design/2026-09-27-ddgi-design.md): where each
// level's grid sits around the camera, which storage slot each probe lives in, and which probes the
// GPU updates this frame. shaders/vulkan/ddgi_common.glsl does the same arithmetic.

// Probes per level along x, y (up) and z. Every level has as many; the spacing doubles per level.
inline constexpr glm::ivec3 kDdgiGridSize{24, 8, 24};
inline constexpr uint32_t kDdgiProbesPerLevel = 24u * 8u * 24u;
inline constexpr uint32_t kDdgiMaxLevels = 4;
// Rays each updated probe traces per frame: one workgroup of the update pass reads them all.
inline constexpr uint32_t kDdgiRaysPerProbe = 64;
// Octahedral texels per probe, without and with the one-texel border.
inline constexpr uint32_t kDdgiIrradianceTexels = 8;
inline constexpr uint32_t kDdgiVisibilityTexels = 16;

// One level's grid: the world grid coordinate of its minimum corner probe; probe g sits at g * spacing
// (before relocation).
struct DdgiLevel
{
    glm::ivec3 origin{0};
    float spacing = 1.0f;
};

// The grid of this spacing centred on the camera: the camera's cell, minus half the grid. It moves
// only when the camera crosses a cell boundary, and then by whole cells.
DdgiLevel ComputeDdgiLevel(const glm::vec3& camera, float spacing);

// The storage slot along each axis of the probe at world grid coordinate coord: coord mod the grid
// size, non-negative. A scroll therefore moves no probe's data.
glm::ivec3 DdgiStorageSlot(const glm::ivec3& coord);

// The world grid coordinate the slot holds while the level's grid starts at origin: the one
// coordinate in [origin, origin + size) congruent to the slot.
glm::ivec3 DdgiSlotCoordinate(const glm::ivec3& slot, const glm::ivec3& origin);

// A slot's index within its level, and back: x fastest, then z, then y (ddgi_common.glsl).
uint32_t DdgiSlotIndex(const glm::ivec3& slot);
glm::ivec3 DdgiSlotFromIndex(uint32_t index);

// What the GPU's schedule buffer holds per updated probe: level << 24 | slot index.
inline uint32_t PackDdgiProbe(uint32_t level, uint32_t slotIndex)
{
    return (level << 24) | slotIndex;
}

// Chooses the probes to update each frame. It mirrors which world coordinate each slot holds, which
// the GPU also records per probe (a probe's data is used only where its recorded coordinate matches):
// a slot whose coordinate changed with a scroll, or that was never updated, is stale. Stale probes of
// the finest levels come first; the rest of the budget goes round robin, each level twice as often as
// the next coarser one.
class DdgiProbeScheduler
{
  public:
    // levels: this frame's grids, finest first (at most kDdgiMaxLevels). A change in the level count or
    // a spacing starts over with every probe stale.
    std::vector<uint32_t> Schedule(std::span<const DdgiLevel> levels, uint32_t budget);

    // Every probe stale again, as after new content.
    void Reset();

    // How many probes of this level hold stale data for the grids of the last Schedule (for tests and
    // the editor).
    uint32_t StaleCount(uint32_t level) const;

  private:
    struct LevelState
    {
        float spacing = 0.0f;
        // The grid's origin at the last Schedule.
        glm::ivec3 origin{0};
        // Per slot index: the coordinate it holds, and whether it holds anything.
        std::vector<glm::ivec3> held;
        std::vector<uint8_t> valid;
        uint32_t cursor = 0;
        // Fractional share of the round-robin budget carried to the next frame.
        float credit = 0.0f;
    };
    std::vector<LevelState> m_levels;
};

// Frames an instance stays out of the probe rays after its matrix last changed.
inline constexpr uint32_t kDdgiMovingInstanceFrames = 30;

// Which ray scene instances the probe rays skip because they move (the DDGI design's moving
// instances): probes blend over many frames, so a car traced where it passes would leave a dark
// trail on the road behind it. An instance whose matrix changed in the last kDdgiMovingInstanceFrames
// frames is skipped; once it stands still it is traced again. It still receives GI either way.
class DdgiMovingInstances
{
  public:
    // models: this frame's matrix per instance. A different count than last frame is new content,
    // and every instance starts out still. The result, one flag per instance (non-zero: skip), stays
    // valid until the next call.
    std::span<const uint8_t> Update(std::span<const glm::mat4> models);

  private:
    std::vector<glm::mat4> m_previous;
    // Frames since each instance's matrix last changed, saturating at kDdgiMovingInstanceFrames.
    std::vector<uint32_t> m_stillFrames;
    std::vector<uint8_t> m_skipped;
};

// A rotation for this frame's probe ray directions: the spherical Fibonacci set turned randomly, so
// the directions cover the sphere over frames.
glm::mat3 DdgiRayRotation(uint32_t frameIndex);
}
