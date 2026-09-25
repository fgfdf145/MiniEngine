# Local-Light Shadows

## Goal

Only the brightest directional light casts shadows. Every point, spot and area light shines through
walls and columns: in Sponza a lamp behind a pillar lights the floor on the far side of it. Give the
local lights shadow maps, using the light storage buffer the clustered lighting introduced for
exactly this ("a light in a storage buffer can carry a shadow map index").

The user accepts this by the rendered image.

## Current State

- `VulkanShadowPass` renders four 2048^2 cascades of the one directional caster into a 2D array
  depth image, shared by every frame in flight and ordered by its render pass dependencies. The
  shader samples it through set 0 binding 1 with a `LESS_OR_EQUAL` comparison sampler, 3x3 bilinear
  taps, and a normal offset of 1.5 texels.
- `ShadeSurface` multiplies only the caster's contribution (and its coat and sheen) by the shadow.
  Local lights come from the cluster grid (or the brute-force loop) and are never shadowed.
- `GpuLightData::areaRightAxis.w` is unused and always 0.
- `ShadowDrawItem` carries each caster's world bounding sphere; Blend materials never cast.

## Decisions

1. **One shadow atlas.** A 4096 x 4096 depth image (D32, 64 MiB; D16 fallback as the cascades) cut
   into an 8 x 8 grid of 512 x 512 tiles, 64 tiles. One image shared by every frame in flight, laid
   out and synchronised exactly as the cascade map is (its own render pass, cleared from UNDEFINED,
   left SHADER_READ_ONLY). Set 0 binding 13. Fixed tile size: per-light resolution by screen size is
   a later refinement.
2. **Tiles per light.** A spot light takes one tile: a perspective frustum along its direction with
   a field of view of twice its outer angle plus a guard band. A point light takes six, one per cube
   face. An area light takes six too, rendered from its centre: its emission is one-sided, but the
   faces behind it simply shade nothing, and a centre-point shadow is the usual approximation (the
   PCF softens it; the penumbra does not widen with the light's size). A spot whose outer angle is
   above 60 degrees is shadowed as a point, since one frustum wider than 120 degrees wastes most of
   its texels.
3. **Guard band.** Each cube face's frustum is widened so the 90 degree face it serves ends two
   texels inside the tile: `tan(fov / 2) = (tile / 2) / (tile / 2 - 2)`. The 3x3 bilinear taps reach
   at most two texels past the lookup, so a lookup never reads outside its tile, and faces meet
   without seams. The shader also clamps the lookup to its tile rect, so no tap can read a
   neighbour's depth whatever the numbers.
4. **Which lights get tiles.** The shadowed lights are the selected local lights with the new
   `LightComponent::castShadows` (default on), whose range sphere intersects the camera frustum, in
   the selection's order (illuminance at the camera). Tiles are handed out greedily until the 64
   run out; a light that does not fit is not shadowed, and the renderer logs the count when it
   changes, as it does for dropped lights. Up to ten point lights, or 64 spots, are shadowed.
5. **Depth.** Standard zero-to-one perspective, near plane 0.05 m, far plane the light's range, the
   same `LESS` depth test and slope-scaled rasterisation bias as the cascades.
6. **Bias.** Normal offset of 1.5 texels as the cascades, where a texel's world size grows with the
   distance to the light: `texel = distance * 2 tan(fov / 2) / 512`.
7. **Per-tile data.** A storage buffer at set 0 binding 14 (12 holds the materials):
   `LocalShadowTile { mat4 viewProjection; vec4 atlasRect; vec4 params; }` (atlas rect =
   offset.xy, scale.xy in UV; params.x = `2 tan(fov / 2) / tileSize`). The light carries its first
   tile in `areaRightAxis.w` as `tile + 1`, so 0 keeps meaning "no shadow" and a scene without
   shadowed lights uploads what it uploads today.
8. **Face selection** is the major axis of `worldPos - lightPos`, in the order +X, -X, +Y, -Y, +Z,
   -Z, in a shared GLSL file (`local_shadow_common.glsl`) that the C++ test compiles, as
   `ssr_common.glsl` is. The C++ side builds the six face matrices in that same order.
9. **Caster culling.** Per tile, a caster is drawn when its bounding sphere intersects the tile's
   frustum (six planes extracted from the view-projection, Gribb-Hartmann) — which also rejects
   casters beyond the light's range, since the far plane is the range.
10. **Shading.** In both local loops of `ShadeSurface` (clustered and brute force) a light with a
    tile and a non-zero contribution multiplies its contribution, coat and sheen by
    `EvaluateLocalShadow(light, worldPosition, geoNormal)`, as the directional caster does. The
    forward pass shades through the same function and gets it for free.
11. **Controls.** Graphics Debug gets "Local light shadows" (on by default); off, no tile is
    handed out and the image is the previous build's. The light panel gets a "Cast Shadows"
    checkbox for point, spot and area lights, serialised as `cast_shadows` (absent reads as true).

## Data Flow

```
SelectSceneLights ─► selected local lights ─► PlanLocalShadows (pure: frustum test, tile budget)
                                                 │ tiles (viewProj, rect, texel scale)
                                                 │ first tile per selected light
                                                 ▼
    GpuLightData.areaRightAxis.w  ◄──────────────┤
    VulkanUniformBuffer::Update: binding 14      ◄┤
    VulkanLocalShadowPass::Record (atlas)        ◄┘  (before the scene passes, after the cascades)
                                                 ▼
    ShadeSurface: local loop ─► EvaluateLocalShadow (face, rect clamp, 3x3 PCF)
```

### `local_shadows.h` (pure, `engine/renderer/`)

```cpp
inline constexpr uint32_t kLocalShadowAtlasSize = 4096;
inline constexpr uint32_t kLocalShadowTileSize = 512;
inline constexpr uint32_t kLocalShadowTileCount = 64;

struct LocalShadowLight
{
    LightType type;
    glm::vec3 position;
    glm::vec3 direction;      // spot lights
    float range;
    float outerAngleRadians;  // spot lights
    bool castShadows;
};

struct LocalShadowTile
{
    glm::mat4 viewProjection;
    glm::vec4 atlasRect;      // uv offset xy, uv scale zw
    float texelScale;         // 2 tan(fov / 2) / tile size
};

struct LocalShadowPlan
{
    std::vector<LocalShadowTile> tiles;
    std::vector<int32_t> firstTile; // per input light, -1 when not shadowed
    uint32_t droppedCount = 0;      // shadow casting lights in view that did not fit
};

LocalShadowPlan PlanLocalShadows(std::span<const LocalShadowLight> lights, const glm::mat4& cameraViewProjection);
bool FrustumIntersectsSphere(const glm::mat4& viewProjection, const glm::vec3& center, float radius);
```

## Automated Verification

`tests/local_shadows_tests.cpp`:

- **Cube faces cover the sphere.** For random directions, the face `SelectCubeFace` (the shared GLSL)
  picks projects the point inside its tile rect with at least two texels to spare.
- **Faces agree with the C++ matrices**: face i's view-projection maps a point on its axis to the
  tile centre, depth in (0, 1).
- **Spot frustum** contains the whole outer cone out to the range; spots above 60 degrees get six
  tiles.
- **Budget:** lights are served in order, a light that needs more tiles than remain gets none and is
  counted, `castShadows = false` and out-of-view lights take no tiles.
- **Tiles do not overlap** and lie inside the atlas.
- **FrustumIntersectsSphere** accepts spheres touching the frustum and rejects ones wholly outside
  each plane.

## Manual Acceptance (by image)

1. Sponza with local lights: a point light between the columns throws column shadows on the floor
   and walls; a spot light's cone is cut by the geometry in front of it.
2. No acne on lit surfaces, no light leaking at the feet of columns, no seams between cube faces.
3. "Local light shadows" off: within the run-to-run noise of the previous build.
4. Frame time reported with and without, with numbers.

## Out of Scope

- Caching static shadow maps across frames, per-light tile resolution, soft shadows sized by the
  area light, contact shadows, translucent shadows from Blend materials, shadow atlas debug view.
