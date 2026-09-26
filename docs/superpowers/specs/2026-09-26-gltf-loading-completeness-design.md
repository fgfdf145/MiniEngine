# glTF Loading Completeness

## Goal

Open the glTF files exporters and the Khronos sample repository actually ship, not only the plain
ones: required extensions are checked, a model's own lights shine, quantized, Draco- and
meshopt-compressed geometry loads, and KTX2 (Basis Universal) textures load. Animation, skinning and
morph targets stay out of scope (the user's call, 2026-09-26: materials and models first).

Acceptance models from glTF-Sample-Assets: `glTF-Quantized` (Avocado, Duck, Lantern), `glTF-Draco`
(Avocado, BoomBox, Lantern), `glTF-Meshopt` and `glTF-Meshopt-EXT` (DragonAttenuation,
MeshoptCubeTest), `glTF-KTX-BasisU` (FlightHelmet, StainedGlassLamp, AnisotropyBarnLamp), and for
lights `DirectionalLight`, `PointLightIntensityTest`, `LightsPunctualLamp` against the Sample Viewer.

## Decisions

1. **`extensionsRequired` is checked.** The loader knows the extensions it implements. A required
   one it does not implement fails the import with an error naming it (instead of a model drawn
   wrong); a used, not required one it does not implement is logged as a warning.
2. **Quantized geometry** (`KHR_mesh_quantization`): positions, normals, tangents and UVs stored as
   (normalized) bytes and shorts. The accessor reader already converts every component type; this
   adds the extension to the known list and a test that decodes each allowed type. UV dequantization
   comes through `KHR_texture_transform`, already imported.
3. **Lights are part of the model** (`KHR_lights_punctual`): `LoadedModelData::lights` holds each
   light with its node's world transform (position, direction), type, colour, intensity, range and
   cone. They are not scene entities, so saving and reloading a scene never duplicates them: the
   renderer adds them to the scene's lights through the model entity's transform, while
   `ModelComponent::useModelLights` (default on, saved as `use_model_lights`, a checkbox in the model
   panel) turns them off. Units follow the engine's: glTF's candela becomes lumens (a point light
   `4 pi cd`, a spot `2 pi (1 - cos outer) cd`, which the shader divides back out), directional lux
   stays lux. An undefined range (infinite in glTF) becomes the distance where the light falls to
   1e-3 lux, `sqrt(cd / 1e-3)`, capped at 1000 m, so the clusters keep a finite sphere. glTF's
   default spot angles (inner 0, outer pi/4) apply. The comparison tool stops writing the model's
   lights into its scenes: the engine imports them now.
4. **Meshopt** (`EXT_meshopt_compression` and `KHR_meshopt_compression`, meshoptimizer from
   vcpkg): before anything reads an accessor, each compressed buffer view is decoded (attributes,
   triangles or indices mode, then the octahedral, quaternion, exponential or colour filter) into a
   new buffer and the view is pointed at it. The fallback buffer such files carry has no data and is
   never read.
5. **Draco** (`KHR_draco_mesh_compression`, draco from vcpkg): a primitive with the extension is
   decoded from its buffer view; the decoded attributes, by the extension's attribute ids, and the
   faces replace the accessor reads, with the accessors' own component type and normalization
   deciding how the decoded values dequantize.
6. **KTX2 textures** (`KHR_texture_basisu`, libktx from vcpkg): a texture's source may sit in the
   extension. `.ktx2` images are loaded by the texture loader, Basis Universal (ETC1S or UASTC)
   transcoded to RGBA8, then take the existing path: mips rebuilt, BC7 or BC5, cached. The second
   compression costs a little quality against transcoding straight to BC7, for one path through the
   cache. Non-Basis KTX2 files in an 8-bit RGBA format load directly; other formats fail with a
   message.

## Risks

- tinygltf rejects a buffer without `uri` in a `.gltf` (the meshopt fallback), and in a `.glb` reads it
  from the BIN chunk, which is shorter. Resolved: the JSON (the `.glb`'s JSON chunk) is patched before
  tinygltf parses it, each fallback buffer given one byte of inline data.
- vcpkg's ktx port does not build for x86 Windows. There vcpkg.json leaves it out and KTX2 textures
  fail to load with a message.
- New vcpkg dependencies (draco, meshoptimizer, ktx) lengthen a clean configure.

## Automated Verification

- The required-extension check: a required unknown extension fails with its name; a used one warns;
  the implemented ones pass.
- Quantized accessors: normalized and unnormalized bytes and shorts decode to the values the
  specification gives.
- Lights: types, units (candela to lumens), default range, cone defaults and the node transform's
  position and direction; `use_model_lights` round trip in a scene.
- Meshopt: a buffer view encoded with meshoptimizer's own encoder decodes back to the source bytes.
- Draco: a mesh encoded with draco's encoder decodes to the same positions and indices.
- KTX2: a file written by libktx loads with the original pixels.

## Manual Acceptance (by image)

1. Each compressed or quantized variant renders like its plain glTF (same scene, mean difference
   within the run-to-run noise).
2. `DirectionalLight`, `PointLightIntensityTest` and `LightsPunctualLamp` against the Sample Viewer.
3. KTX2 FlightHelmet and StainedGlassLamp against their PNG versions.
