#pragma once

#include <engine/scene/scene_components.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace me
{

// Shadow maps for the local lights (point, spot, area): one depth atlas cut into equal square
// tiles. A spot light takes one tile, a point or area light one per cube face. The shader side is
// local_shadow_common.glsl and EvaluateLocalShadow in pbr_common.glsl.
inline constexpr uint32_t kLocalShadowAtlasSize = 4096;
inline constexpr uint32_t kLocalShadowTileSize = 512;
inline constexpr uint32_t kLocalShadowTilesPerRow = kLocalShadowAtlasSize / kLocalShadowTileSize;
inline constexpr uint32_t kLocalShadowTileCount = kLocalShadowTilesPerRow * kLocalShadowTilesPerRow;
inline constexpr uint32_t kLocalShadowCubeFaceCount = 6;
// Every tile ends this many texels beyond the region its lookups land in, so the 3x3 bilinear
// filter, which reaches two texels past the lookup, never reads outside the tile.
inline constexpr float kLocalShadowGuardTexels = 2.0f;
inline constexpr float kLocalShadowNearPlane = 0.05f;
// A spot whose outer half-angle is wider than this is shadowed as a point: one frustum wider than
// twice this angle spends most of its texels on the edges.
inline constexpr float kLocalShadowMaxSpotHalfAngleRadians = 1.04719755f; // 60 degrees

struct LocalShadowLight
{
    LightType type = LightType::Point;
    glm::vec3 position{0.0f};
    // The direction a spot light shines in. Not read for the other types.
    glm::vec3 direction{0.0f, -1.0f, 0.0f};
    float range = 10.0f;
    // A spot light's outer cone half-angle. Not read for the other types.
    float outerAngleRadians = 0.5f;
    bool castShadows = true;
};

struct LocalShadowTile
{
    // World to the tile's clip space: depth in [0, 1], x and y in [-1, 1] over the whole tile.
    // A clip point maps into the atlas as atlasRect.xy + (xy / w * 0.5 + 0.5) * atlasRect.zw; like
    // the cascades, neither side flips y.
    glm::mat4 viewProjection{1.0f};
    // uv offset in xy, uv size in zw.
    glm::vec4 atlasRect{0.0f};
    // 2 tan(fov / 2) / tile size: the world size of one texel at one metre from the light.
    float texelScale = 0.0f;
    // The tile's texel rectangle in the atlas, for the viewport.
    glm::uvec2 atlasOffsetTexels{0u};
};

struct LocalShadowPlan
{
    std::vector<LocalShadowTile> tiles;
    // Per input light: the index of its first tile, or -1 when it casts no shadow. A cube's faces
    // are six consecutive tiles in the order of SelectCubeFace (+X, -X, +Y, -Y, +Z, -Z).
    std::vector<int32_t> firstTile;
    // Lights that would cast a shadow into the view but found no room in the atlas.
    uint32_t droppedCount = 0;
};

// Hands out the atlas tiles, in the order of the lights (the selection's importance order): a
// light gets its tiles when it casts shadows, is not directional or ambient, its range sphere
// reaches into the camera frustum, and enough tiles remain. A light that does not fit is skipped,
// and later, smaller ones may still fit.
LocalShadowPlan PlanLocalShadows(std::span<const LocalShadowLight> lights, const glm::mat4& cameraViewProjection);

// Whether a world space sphere reaches into the frustum of a zero-to-one depth view-projection.
// Conservative: a sphere near a frustum edge may pass without touching it, but one that touches
// it never fails.
bool FrustumIntersectsSphere(const glm::mat4& viewProjection, const glm::vec3& center, float radius);

// The view-projection of one cube face of a light at position with the given range. face is in
// SelectCubeFace's order.
glm::mat4 BuildLocalShadowCubeFace(const glm::vec3& position, float range, uint32_t face);
}
