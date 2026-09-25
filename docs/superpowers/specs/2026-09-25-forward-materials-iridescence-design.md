# Forward-Shaded Materials and Iridescence (Phase 3 of the Complete BRDF Program)

## Goal

Give the rare, expensive material features a path that does not need G-buffer space, and put the
first of them on it: `KHR_materials_iridescence`, thin-film interference (soap bubbles, oil films,
anodised metal, colour-shift paint). See `2026-09-25-complete-brdf-program.md`.

## Current State

- Opaque and Mask draws go through the geometry pass and the deferred lighting pass; Blend draws are
  shaded by the forward pass (`triangle.frag`, the same `ShadeSurface`) over the lighting result.
- The G-buffer uses all eight colour attachments the device offers.

## Decisions

1. **Forward-shaded opaque draws keep their G-buffer entry.** A material with a forward-only
   feature gets `kShadingFlagForward` (32). The geometry pass still draws it, so it writes depth,
   motion vectors, normals and roughness: TAA, AO, SSR tracing from other surfaces, and shadows all
   work as for any opaque surface. The lighting pass skips pixels with the flag. The forward pass
   draws these items after the lighting pass and before the sky and Blend items, with a
   `LESS_OR_EQUAL` depth test, so they land on exactly the pixels the geometry pass gave them. The
   same vertex shader and inputs produce the same depth.
2. **Draw order** becomes deferred Opaque/Mask, then forward-shaded Opaque/Mask, then Blend back to
   front (`BuildMaterialDrawOrder`), with `forwardShadedDrawItemBegin` beside `blendDrawItemBegin`.
3. **What forward-shaded surfaces lose:** screen-space reflections, as Blend surfaces do (the forward
   pipelines do not bind the G-buffer set). Their specular occlusion still applies.
4. **Iridescence** (Belcour and Barla 2017, as `KHR_materials_iridescence` specifies it): a thin film
   of IOR `iridescenceIor` (1.3) and thickness `mix(min, max, thicknessTexture.g)` nanometres (100
   to 400) over the base. Its Fresnel is the two-beam Airy sum with the spectral integral replaced
   by the paper's Gaussian fit of the CIE matching functions in Fourier form, evaluated once per
   pixel at N.V, as the Khronos sample viewer does. The base's Fresnel, everywhere it is used
   (direct, LTC, split-sum ambient, the diffuse weight), becomes `mix(Schlick, iridescence F,
   iridescenceFactor * iridescenceTexture.r)`. It rides in `SpecularParams`, so no shading
   signature changes. The coat is unaffected.
5. **Data**: `GpuMaterialData` gains `iridescenceFactors` (factor, IOR, thickness min, max), 160
   bytes; the material set gains bindings 21 (iridescence, R) and 22 (thickness, G). Import,
   sidecar (`iridescence_factor`, `iridescence_ior`, `iridescence_thickness_minimum`,
   `iridescence_thickness_maximum`, and the two texture paths) and the material panel follow the
   other extensions.

## Automated Verification

- `iridescence_common.glsl` compiled into a C++ test: a film of zero thickness reflects the base's
  Schlick Fresnel; the result stays in [0, 1]; the colour changes with thickness (different hues at
  200, 300 and 400 nm); total internal reflection returns 1.
- `BuildMaterialDrawOrder` puts forward-shaded items between the deferred ones and Blend, keeps
  Blend back to front.
- Import, sidecar and defaults of the extension.

## Manual Acceptance (by image)

1. Spheres of rising film thickness show the soap-bubble hues shifting across the sphere with the
   view angle; factor 0 is the plain sphere.
2. A forward-shaded sphere moving under TAA does not smear (motion vectors kept); it casts and
   receives shadows.
3. Sponza: within the run-to-run noise of the previous build.
