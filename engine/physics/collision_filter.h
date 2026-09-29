#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <span>

namespace me
{

// Ground cover: at least this share of the mesh's triangles are upright (within 30 degrees of
// vertical), and the median upright triangle is shorter than this many metres.
inline constexpr float kGroundCoverMaxTriangleHeight = 0.5f;
inline constexpr float kGroundCoverMinUprightShare = 0.8f;

// Whether a mesh, in world space, is ground cover: grass, weeds and litter drawn as swarms of small
// upright alpha-tested cards (Spa's grass verges are a million triangles of them). A car rolls over
// that; as solid triangles it would catch on the blades, bounce, and stop against the verge. Tall
// cut-outs (fences, trees, bushes) and flat ones (drain gratings) are not ground cover.
bool IsGroundCover(std::span<const glm::vec3> vertices, std::span<const uint32_t> indices);
}
