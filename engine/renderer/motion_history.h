#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace me
{

// Identifies one draw across frames: the entity it belongs to and its position among that entity's
// submeshes. A slot index would not do, because a content reload may reorder the draw list.
struct MotionKey
{
    uint32_t entity = 0;
    uint32_t submeshOrdinal = 0;
};

struct MotionFrame
{
    glm::mat4 previousViewProjection{1.0f};
    // Parallel to the models passed to Advance.
    std::vector<glm::mat4> previousModels;
};

// Remembers last frame's view-projection and each draw's model matrix, for motion vectors. A draw
// with no history, and every draw after Reset, reports its current matrix: zero motion rather than
// a guess.
class MotionHistory
{
  public:
    // Returns last frame's matrices for this frame's draws, then remembers this frame's. Throws
    // std::invalid_argument when keys and models differ in length or a key repeats, since a
    // duplicate would silently give one draw the other's history.
    MotionFrame Advance(
        const glm::mat4& viewProjection,
        std::span<const MotionKey> keys,
        std::span<const glm::mat4> models);

    void Reset();

  private:
    static uint64_t Pack(const MotionKey& key);

    bool m_hasHistory = false;
    glm::mat4 m_viewProjection{1.0f};
    std::unordered_map<uint64_t, glm::mat4> m_models;
};
}
