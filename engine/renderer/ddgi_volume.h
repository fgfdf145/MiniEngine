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
// GPU updates this frame. shaders/vulkan/ddgi_common.slang does the same arithmetic.

// Probes per level along x, y (up) and z. Every level has as many; the spacing doubles per level.
// 32 across puts the finest level's reach at 14.5 cells from the camera: a room or an arcade smaller
// than the next level's spacing stays dark out to there (24 across reached 10.5, and coarser probes
// standing outside lit an arcade's back wall from 11 m on). 16 up reaches 6.5 cells above and below.
inline constexpr glm::ivec3 kDdgiGridSize{32, 16, 32};
inline constexpr uint32_t kDdgiProbesPerLevel = 32u * 16u * 32u;
// Cells over which a level fades into the next, horizontally and vertically, measured from the
// camera (ddgi_common.slang's DdgiIrradianceAlong): wide enough that walking toward or away from a
// surface changes its light gradually rather than at a line, narrower up and down, where the grid
// is half as deep and a wide band handed a room's ceiling to the coarse levels.
inline constexpr float kDdgiFadeCellsHorizontal = 3.0f;
inline constexpr float kDdgiFadeCellsVertical = 2.0f;
inline constexpr uint32_t kDdgiMaxLevels = 4;
// Rays each updated probe traces per frame: one workgroup of the update pass reads them all.
inline constexpr uint32_t kDdgiRaysPerProbe = 64;
// Octahedral texels per probe, without and with the one-texel border.
inline constexpr uint32_t kDdgiIrradianceTexels = 8;
inline constexpr uint32_t kDdgiVisibilityTexels = 16;
// One probe's record on the GPU (ddgi_common.slang's DdgiProbeState): coordinate and flags, offset,
// statistics.
inline constexpr uint32_t kDdgiProbeStateBytes = 48;

// One level's grid: the world grid coordinate of its minimum corner probe; probe g sits at g * spacing
// (before relocation).
struct DdgiLevel
{
    glm::ivec3 origin{0};
    float spacing = 1.0f;
};

// The grid of this spacing centred on the camera: its origin is the camera's cell minus one less than
// half the grid, so points within half the grid minus one cell of the camera, on every side, lie
// between probes whatever the camera's place in its cell (15 cells across, 7 up and down). It moves
// only when the camera crosses a cell boundary, and then by whole cells; ddgi_common.slang fades each
// level by the distance to the camera, not to the grid's faces, so the moves do not show.
DdgiLevel ComputeDdgiLevel(const glm::vec3& camera, float spacing);

// The storage slot along each axis of the probe at world grid coordinate coord: coord mod the grid
// size, non-negative. A scroll therefore moves no probe's data.
glm::ivec3 DdgiStorageSlot(const glm::ivec3& coord);

// The world grid coordinate the slot holds while the level's grid starts at origin: the one
// coordinate in [origin, origin + size) congruent to the slot.
glm::ivec3 DdgiSlotCoordinate(const glm::ivec3& slot, const glm::ivec3& origin);

// A slot's index within its level, and back: x fastest, then z, then y (ddgi_common.slang).
uint32_t DdgiSlotIndex(const glm::ivec3& slot);
glm::ivec3 DdgiSlotFromIndex(uint32_t index);

// What the GPU's schedule buffer holds per updated probe: level << 24 | slot index.
inline uint32_t PackDdgiProbe(uint32_t level, uint32_t slotIndex)
{
    return (level << 24) | slotIndex;
}

// What ddgi_update.comp reports per updated probe (its feedback buffer): these bits, and the
// coordinate it updated mod 256 per axis in bits 8 to 31 (DdgiFeedbackCoordinate), so a report that
// arrives after a scroll gave the slot another coordinate is recognised as stale.
// CHANGED: its light changed (or the lighting epoch did), and it restarted its average.
// EMPTY: no surface near enough for it to light, for several updates in a row.
// INACTIVE: inside geometry.
inline constexpr uint32_t kDdgiFeedbackChanged = 1u;
inline constexpr uint32_t kDdgiFeedbackEmpty = 2u;
inline constexpr uint32_t kDdgiFeedbackInactive = 4u;
inline uint32_t DdgiFeedbackCoordinate(const glm::ivec3& coord)
{
    const glm::uvec3 bits = glm::uvec3(coord) & 0xffu;
    return (bits.x << 8) | (bits.y << 16) | (bits.z << 24);
}

// Chooses the probes to update each frame. It mirrors which world coordinate each slot holds, which
// the GPU also records per probe (a probe's data is used only where its recorded coordinate matches):
// a slot whose coordinate changed with a scroll, or that was never updated, is stale. Stale probes of
// the finest levels come first; then hot probes, whose light the GPU saw change (ApplyFeedback), and
// their neighbours, at most half the budget; the rest goes round robin, each level twice as often as
// the next coarser one. A cold probe, one the GPU found empty (no surface near enough for it to
// light), takes its round robin turn only once in kDdgiColdRateDivisor passes.
//
// A level whose probes have converged refreshes at 1 / kDdgiSettledShareDivisor of its round robin
// share. It has converged once the updates since it last changed have left less than
// kDdgiSettledResidual of whatever it held before: each update keeps the hysteresis' share of the old
// value, so n updates per probe leave hysteresis^n (counted per probe that takes every turn: cold
// ones count as a fraction). A scroll, Unsettle or Reset starts it over.
class DdgiProbeScheduler
{
  public:
    // levels: this frame's grids, finest first (at most kDdgiMaxLevels). A change in the level count or
    // a spacing starts over with every probe stale. hysteresis: what this frame's updates blend with;
    // 1 (the default) never settles.
    std::vector<uint32_t> Schedule(std::span<const DdgiLevel> levels, uint32_t budget, float hysteresis = 1.0f);

    // Every probe stale again, as after new content.
    void Reset();
    // Every level unconverged again, for lighting that changed or instances that move.
    void Unsettle();
    // Whether a level refreshes at the settled rate (for tests and the editor).
    bool Settled(uint32_t level) const;

    // How many probes of this level hold stale data for the grids of the last Schedule (for tests and
    // the editor).
    uint32_t StaleCount(uint32_t level) const;

    // What the GPU reported for the probes of an earlier Schedule (one feedback value per scheduled
    // probe, kDdgiFeedback* bits). A changed probe becomes hot for kDdgiHotUpdates updates and its six
    // neighbours for kDdgiNeighbourHotUpdates; an empty one becomes cold (not one inside geometry: it
    // needs its updates to come back to life). Reports for slots that have since moved to another
    // coordinate are ignored.
    void ApplyFeedback(std::span<const uint32_t> scheduled, std::span<const uint32_t> feedback);
    // The geometry changed (a new ray scene): what made probes cold may be gone, and the levels
    // converge over. Unlike Reset, no probe goes stale.
    void GeometryChanged();
    // Whether a probe is cold or hot (for tests).
    bool Cold(uint32_t level, uint32_t slotIndex) const;
    bool Hot(uint32_t level, uint32_t slotIndex) const;

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
        // What remains, per probe on average, of the level's content before its last change.
        float residual = 1.0f;
        // Per slot index: non-zero when cold; the updates it still has coming as a hot probe.
        std::vector<uint8_t> cold;
        std::vector<uint8_t> hot;
        uint32_t coldCount = 0;
        // Round robin passes over the level: a cold probe takes the passes where (passes + index) is
        // a multiple of kDdgiColdRateDivisor.
        uint32_t passes = 0;
    };
    void MakeHot(uint32_t level, uint32_t index, uint8_t updates);

    std::vector<LevelState> m_levels;
    // The hot probes (PackDdgiProbe), oldest first.
    std::vector<uint32_t> m_hot;
};

inline constexpr uint8_t kDdgiHotUpdates = 4;
inline constexpr uint8_t kDdgiNeighbourHotUpdates = 2;
inline constexpr uint32_t kDdgiColdRateDivisor = 16;

inline constexpr float kDdgiSettledResidual = 0.01f;
inline constexpr float kDdgiSettledShareDivisor = 4.0f;

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
    // Whether the last Update saw an instance move or the count change: the path tracer's still
    // image starts over then.
    bool MovedThisFrame() const;

  private:
    std::vector<glm::mat4> m_previous;
    bool m_movedThisFrame = false;
    // Frames since each instance's matrix last changed, saturating at kDdgiMovingInstanceFrames.
    std::vector<uint32_t> m_stillFrames;
    std::vector<uint8_t> m_skipped;
};

// Watches the lighting the probes hold (the DDGI design's lighting epoch): the lighting is whatever
// values the caller says describe it (the directional lights' directions and colours, the sky's mode
// and ambient); a difference in any of them beyond a relative kDdgiLightingTolerance from the lighting
// the epoch started with, or in their count, starts a new epoch. Against the epoch's start rather than
// the last frame, so a sun the time of day moves a little every frame starts one every so often.
// Every probe that updates in a later epoch than the one it recorded restarts its average
// (ddgi_update.comp), whenever its turn comes: at 0.97 an update, a moved sun took a hundred updates
// per probe to show in the bounce light, and with the round robin's tens of frames between a probe's
// updates, that was minutes.
class DdgiLightingWatch
{
  public:
    // Whether this call started a new epoch (never the first call).
    bool Update(std::span<const glm::vec4> lighting);
    // Whether the last Update saw the lighting change.
    bool Changed() const;
    // The epoch, counting changes mod 256 (what the probes record).
    uint32_t Epoch() const;

  private:
    // The lighting when the epoch started.
    std::vector<glm::vec4> m_reference;
    bool m_hasReference = false;
    bool m_changed = false;
    uint32_t m_epoch = 0;
};

// About a third of a degree of the sun's direction, or half a percent of its strength.
inline constexpr float kDdgiLightingTolerance = 5e-3f;

// A new lighting epoch whose light level (the directional lights' illuminance plus the ambient's and
// the HDRI's average luminance) is more than this many times the last epoch's, or less than its
// inverse, clears the probes instead of letting them average toward it.
inline constexpr float kDdgiLightingJump = 2.0f;

// Whether a light level moved from previous to current by more than kDdgiLightingJump either way.
bool DdgiLightingJumped(float previous, float current);

// A rotation for this frame's probe ray directions: the spherical Fibonacci set turned randomly, so
// the directions cover the sphere over frames.
glm::mat3 DdgiRayRotation(uint32_t frameIndex);
}
