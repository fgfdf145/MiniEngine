# Volume Scatter and GPU Instancing

## Goal

The two glTF extensions still missing that the Khronos Sample Viewer implements and a sample model
exercises: `KHR_materials_volume_scatter` (subsurface scattering in a volume; draft; `ScatteringSkull`)
and `EXT_mesh_gpu_instancing` (one mesh at many transforms; ratified vendor extension;
`SimpleInstancing`). Accepted against the viewer with `tools/khronos_reference`, as the material phases
were. See `2026-09-25-complete-brdf-program.md` (volume scatter is the Khronos-standard half of phase 5,
SSS) and `2026-09-26-gltf-loading-completeness-design.md`.

## What the Specifications and the Viewer Do

Read from `glTF-Sample-Renderer` at cc27919 (2026-09-22), the renderer the viewer release runs.

### KHR_materials_volume_scatter

- Draft (glTF e17468d). Members `multiscatterColor` (vec3, default 0) and `scatterAnisotropy` (-1..1,
  default 0). Used with `KHR_materials_volume`; the spec recommends `KHR_materials_diffuse_transmission`
  for dense media. The single-scatter albedo is
  `ss = 1 - (4.09712 + 4.20863 m - sqrt(9.59217 + 41.6808 m + 17.7126 m^2))^2` for multi-scatter `m`.
- The viewer's implementation is a screen-space Burley diffusion (Blender's):
  1. **Pre-pass** (`scatter.frag`) draws only the scatter materials (volume and volume scatter both
     present) into their own RGBA16F target with their own depth buffer, each primitive's alpha a
     draw id. Its colour is the diffuse light entering the surface, times `diffuseTransmissionFactor`:
     `(1 - F) * dtColor * ss * (E(n) + E(-n) * atten * (1 - ss))` for the environment (E the diffuse
     irradiance, F the dielectric IBL Fresnel `getIBLGGXFresnel`), and per punctual light
     `(1 - F(v.h)) * I / pi * dtColor * ss * (n.l front + |n.l| * atten * (1 - ss) back)` (the back
     Fresnel about the mirrored light, as diffuse transmission does). `atten` is the volume
     attenuation through the thickness times the node's mean scale. No ambient occlusion, no shadows
     (the viewer has none), sheen scaling applied.
  2. **Main pass** (`pbr.frag`): diffuse transmission as before, but the light through the back is
     scaled by `1 - ss` (IBL and punctual). After the lights it adds
     `SSS * dtColor * (1 - metallic) * (1 - coat * coatFresnel) * (1 - iridescence) * (1 - transmission)`.
  3. **SSS** (`getSubsurfaceScattering`): the scatter radius is `attenuationDistance * multiscatterColor`
     (per channel); its largest channel in pixels (from the depth and the projection) is the disk the
     55 precomputed Burley samples (golden angle, radius by inverse CDF with Newton iterations, `1/pdf`)
     spread over. Samples of another draw id are skipped; each weight is the Burley profile at the
     view-space distance between the two surface points, times `1/pdf`; the result is the weighted mean
     of the pre-pass colour. A disk of at most one pixel returns the centre sample.
  4. `scatterAnisotropy` is read and not used.
- **The sample model disagrees with the draft spec**: `ScatteringSkull.gltf` writes
  `multiscatterColorFactor` (the name the successor proposal `KHR_materials_scatter`, glTF PR 2579, uses),
  the viewer reads only `multiscatterColor`, so the viewer renders the skull with multi-scatter 0
  (`ss = 0`, no scattering: plain diffuse transmission).

### EXT_mesh_gpu_instancing

- Node attributes `TRANSLATION` (vec3 float), `ROTATION` (vec4 float or normalized byte/short) and
  `SCALE` (vec3 float), all optional, of one count. `World = NodeWorld * T * R * S * vertex`. The
  non-instanced mesh is not drawn.
- The viewer draws the instances with GPU instancing; its framing (`getSceneExtents`) ignores them
  and boxes the mesh at the node's transform only.

## Decisions

1. **Instancing by expansion at import.** The loader bakes every instance into its own submesh
   (`NodeWorld * instance` into the vertices, `.../instance_<i>` appended to the name), as it bakes
   node transforms today. The renderer already uses `gl_InstanceIndex` as the draw slot, so hardware
   instancing would be a renderer project of its own; expansion is exact and needs nothing downstream.
   Memory grows with the instance count; the import logs the count. The viewer framing box keeps the
   node's transform (the viewer's rule). Rotation accepts the normalized integer types; bad counts or
   types are rejected with an error (a model whose instances cannot be read would draw wrong).
2. **Scatter data.** `GpuMaterialData` gains `volumeScatter` (rgb multi-scatter colour, a anisotropy):
   15 x vec4, 240 bytes. Scatter is on when the material has the extension, `KHR_materials_volume` and a
   diffuse transmission factor above 0 (without diffuse transmission the viewer's pre-pass is zero and
   nothing shows); `volumeScale.w` = 1 marks it (it was unused). Import reads `multiscatterColor`, and
   `multiscatterColorFactor` when that is absent (the sample model; logged). Sidecar
   (`multiscatter_color`, `scatter_anisotropy`, `volume_scatter`) and the material panel follow the
   other extensions.
3. **Pre-pass.** A `VulkanScatterPass` (scene pass, both orders, before `Forward`) draws the scatter
   items with the forward pipelines' shader under a specialization constant (`kScatterPrepass`), into
   `VulkanScatterImages`: an RGBA16F colour and a D32 depth at the scene's extent, owned by the renderer,
   resting in shader-read layouts, recreated on a resize (set 0 bindings 19 and 20 are rewritten then).
   Colour is pre-exposed, alpha the draw slot + 1. It does nothing on a frame without scatter items.
   Unlike the viewer it includes the shadows (the reference view has none) and the scene's area lights
   are skipped (the viewer has none). The pre-pass colour is the same formula as the viewer's.
4. **Main pass.** `triangle.frag` scales the diffuse transmission colour by `1 - ss` and adds the SSS
   term with the viewer's weights. The Burley samples are computed in the shader
   (`volume_scatter_common.glsl`, compiled into a C++ test too, checked against the viewer's numbers).
   One deliberate difference: the disk is round in pixels (the viewer scales both axes by the width's
   texel size, an ellipse on a non-square canvas).
5. **Comparison.** `SimpleInstancing` joins the models. `ScatteringSkull` is compared as a local copy
   with the key renamed to the draft's `multiscatterColor` (served to the viewer by `capture_server.py`),
   so both renderers scatter; the original model is captured too, where the viewer shows no scattering
   and the engine does (documented, expected).

## Automated Verification

- Instancing: T/R/S applied in order after the node's transform, missing attributes default, normalized
  rotations, the viewer box ignores instances, mismatched counts are rejected, the extension is
  implemented (a required one loads).
- Volume scatter import: both keys, defaults, the gate on volume and diffuse transmission, sidecar round
  trip, legacy sidecar.
- `volume_scatter_common.glsl`: `MultiToSingleScatter` at 0, 1 and a colour; the Burley sample radii,
  `1/pdf` and minimum radius against the viewer's `computeScatterSamples`; the profile.

## Manual Acceptance (by image)

`SimpleInstancing`, `ScatteringSkull` (spec key) and the original against the viewer; no change on the
other compared scenes (they have no scatter materials or instancing).
