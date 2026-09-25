# Texture Transforms, Second UV Set and Unlit Materials

## Goal

Close three gaps in how glTF materials are read, found while reviewing what the BRDF program
still lacks: every texture is sampled with the first UV set, untransformed, and unlit materials are
lit. With these, a glTF material using any of them shows as its author made it.

- `KHR_texture_transform`: offset, rotation, scale and a UV set override on any texture.
- `texCoord`: a texture may use `TEXCOORD_1`.
- `KHR_materials_unlit`: the base colour, with no lighting.

## Decisions

1. **Transforms per texture slot.** `TextureTransform { offset, rotation, scale, texCoord }`, one per
   material texture slot (the set 1 binding order, 23 slots; the blend graph's layer B and mask stay
   identity). The metallic and roughness slots share the glTF metallic-roughness texture's
   transform. glTF's matrix is `T * R * S`, `uv' = offset + R(rotation) * (scale * uv)` with
   `R = [[cos, sin], [-sin, cos]]`.
2. **On the GPU** a storage buffer at set 0 binding 17 holds, per draw slot and texture slot, two
   vec4s: `(a, b, tx, set)` and `(c, d, ty, 0)`. `GpuMaterialData.shadingModel.y` is 1 when the
   material has any non-identity transform or any texture on the second set; with 0 the shaders take
   the plain UV and never read the buffer, so ordinary materials sample exactly as before.
3. **The second UV set**: `Vertex` gains `texCoord1` (the glTF's `TEXCOORD_1`, zero when absent),
   vertex attribute 5, passed to the fragment shaders. Tangents stay derived from the first set.
4. **Shadows** run the alpha test through the base colour texture's transform: the shadow pass's push
   constants gain its two rows (144 bytes). Vulkan guarantees 128; device selection requires 144, as
   it requires eight colour attachments (MoltenVK offers 4096).
5. **Unlit** (`MaterialPbrSurfaceSettings::unlit`, sidecar `unlit`, material panel checkbox): a new
   shading flag, 64. The surface emits its base colour (factor, texture, vertex colour) as if lit to
   paper white: `baseColor * kFrameBufferUnitsPerExposed` written straight into the HDR target at
   every exposure, like the viewport background, so white shows as the display's paper white and the
   colours come out close to themselves through the tone curve. No lights, ambient, AO, emissive or
   aerial perspective touch it; it still casts shadows and Blend still blends it.
6. **Import** reads `texCoord` from every textureInfo, `KHR_texture_transform` from the textureInfo's
   extensions (its own `texCoord` overrides), `TEXCOORD_1`, and `KHR_materials_unlit`. The sidecar
   stores non-identity transforms under `texture_transforms`, keyed by slot name.

## Automated Verification

- The transform matrix: identity; each of offset, rotation and scale alone; their composition in
  glTF's order; a rotation of 90 degrees maps (1, 0) as the specification's matrix does.
- Import: `texCoord` 1, a transform with and without its own `texCoord`, metallic-roughness sharing,
  `TEXCOORD_1` read, unlit; sidecar round trip; legacy sidecar without the keys.
- `static_assert`s on `Vertex` and the shadow push constants.

## Manual Acceptance (by image)

1. A checker sphere with scale 4, rotation 45 degrees and an offset shows the transformed checker;
   the same texture on the second UV set shows that set's layout.
2. An unlit sphere shows its flat colour under the sun and in the dark alike.
3. Sponza: within the run-to-run noise of the previous build.
