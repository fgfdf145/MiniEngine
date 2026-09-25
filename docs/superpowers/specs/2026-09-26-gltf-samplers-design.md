# glTF Samplers

## Goal

Sample every material texture the way its glTF sampler says: wrap modes (`REPEAT`, `CLAMP_TO_EDGE`,
`MIRRORED_REPEAT`) per axis, and the magnification and minification filters. Found by the Khronos
reference comparison (`TextureTransformTest` repeats where the model asks for `CLAMP_TO_EDGE`); core
glTF, not an extension.

## Current State

- The importer never reads `textures[i].sampler`. Every `VulkanTexture` creates its own sampler:
  linear, linear mipmaps, 16x anisotropy, repeat on both axes.
- A texture file is loaded once and shared by every material that names it, so its sampler cannot
  carry one material's settings.

## Decisions

1. **Per slot, like texture transforms.** `TextureSampler { wrapS, wrapT, magFilter, minFilter,
   mipFilter }` with engine enums (`TextureWrap` Repeat / ClampToEdge / MirroredRepeat,
   `TextureFilter` Linear / Nearest, `TextureMipFilter` Linear / Nearest / None), one per material
   texture slot (`MaterialTextureSamplers`); the metallic and roughness slots share the
   metallic-roughness texture's. The default is today's sampler: repeat, linear, linear mipmaps.
2. **From glTF** (`TextureSamplerFromGltf`): wrap 10497 / 33071 / 33648; mag 9728 / 9729; min
   9728 (nearest, no mipmaps), 9729 (linear, no mipmaps), 9984 to 9987 (the four mipmapped
   combinations). A missing sampler, or an unset or unknown filter, keeps the engine's default for
   that field: glTF leaves undefined filtering to the implementation.
3. **On the GPU** a `VulkanSamplerCache` owned by the renderer returns one `VkSampler` per distinct
   setting, created on first use and kept for the renderer's lifetime (a handful at most). Material
   descriptors pair each texture's image view with the cache's sampler for that slot's setting; the
   textures' own samplers stay for everything else (the HDRI, defaults outside materials).
   - No mipmaps (`mipFilter` None) is `maxLod` 0.25 with nearest mipmap mode, Vulkan's way of
     sampling the base level only.
   - Anisotropic filtering stays on only where both filters are linear and mipmaps are used; nearest
     filtering asks for texel-exact results that anisotropy would blur.
   - The default setting creates exactly today's sampler (`maxLod` without a limit instead of the
     texture's mip count, which is the same thing).
4. **Sidecar**: `texture_samplers`, keyed by slot name like `texture_transforms`, holding only
   non-default settings (`wrap_s`, `wrap_t`: `repeat`, `clamp_to_edge`, `mirrored_repeat`;
   `mag_filter`, `min_filter`: `linear`, `nearest`; `mip_filter`: `linear`, `nearest`, `none`).
   Absent in older sidecars: every slot keeps the default.
5. Shadows' alpha test binds the same material descriptors, so it samples the base colour with the
   same settings.

## Automated Verification

- `TextureSamplerFromGltf`: each wrap mode, each of the six minification filters, both
  magnification filters, unset (-1) and unknown values keep the default.
- Import: a clamped, nearest-filtered base colour and a mirrored normal map on one material; a
  texture without a sampler keeps the default; metallic and roughness share.
- Sidecar round trip; an older sidecar without the key keeps the default.
- The Vulkan settings a `TextureSampler` maps to (address modes, filters, mipmap mode, `maxLod`,
  anisotropy) as a pure function, including that the default equals today's sampler.

## Manual Acceptance (by image)

1. `TextureTransformTest` against the Sample Viewer: the arrows no longer repeat outside the texture;
   its mean difference drops toward the noise floor.
2. The rest of the Khronos comparison and Sponza: within the run-to-run noise of the previous build
   (their textures use the default or repeat samplers).
