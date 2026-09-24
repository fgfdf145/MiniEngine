# Clearcoat Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Shade `KHR_materials_clearcoat` (factor and roughness) as a second GGX lobe over the base, with coatless surfaces unchanged.

**Architecture:** The factors ride the existing material path (glTF → `ModelMaterialData`/`MaterialPbrSurfaceSettings` → sidecar/editor → `GpuMaterialData`). `ShadingModel::Clearcoat` marks coated draws; the geometry pass writes the factors to a new custom-data target GB5, and `ShadeSurface` adds the coat lobe for both orders.

**Tech Stack:** C++20, Vulkan, GLSL 450, tinygltf, yaml-cpp, CTest executables.

**Spec:** `docs/superpowers/specs/2026-09-24-clearcoat-design.md`

## Global Constraints

- Factors in [0, 1], default 0 / 0; YAML keys `clearcoat_factor`, `clearcoat_roughness_factor` under `pbr:`.
- `ShadingModel::Clearcoat = 1` iff clearcoat factor > 0.
- `GpuMaterialData` 96 bytes, `clearcoatFactors` at offset 80.
- GB5 = `RenderTargetId::GBufferCustom`, `R8G8B8A8_UNORM`, geometry colour location 5, set 2 binding 7.
- Coatless pixels: identical code path; captures within run-to-run noise.

## Review Focus

- Old sidecars without the keys load as 0 / 0 (no coat), not garbage.
- Clearcoat roughness 0 must not blow up GGX: floored at 0.04 like the base.
- A coated Mask or Blend material: forward (Blend) and deferred (Mask) both coat.
- The shadow caster's shadow must darken the coat highlight too.
- Emissive under a coat is dimmed by the coat's Fresnel, and not dimmed without a coat.

---

### Task 1: Clearcoat factors through import, sidecar and editor

- [ ] Failing tests in `tests/gltf_material_extensions_tests.cpp`: import (0.8/0.2, clamping, absent, with textures) and sidecar round trip + legacy sidecar.
- [ ] Fields in `MaterialPbrSurfaceSettings` and `ModelMaterialData`; conversions in `model_loader.cpp`; `ApplyImportedMaterialInfo`; `SerializeMaterialDefinition`/`LoadMaterialDefinition` (clamped); graph node pbr serialise/parse; `DrawMaterialPbrControls` sliders; preview hash; processor panel text; `BuildMaterialData` import with texture warning.
- [ ] Tests pass; commit `feat(asset): clearcoat material factors`.

### Task 2: Clearcoat shading

- [ ] Baseline captures (Sponza few, deferred + forward) from the Task 1 build.
- [ ] `GpuMaterialData.clearcoatFactors`, `ShadingModel::Clearcoat`, filled in `scene_renderables.cpp`.
- [ ] GB5 target, geometry attachment, set 2 binding 7, tonemap reads.
- [ ] Shaders: material struct, GB5 write, lighting-pass decode, `ShadeSurface` coat lobe with emissive inside.
- [ ] Build, tests, Sponza captures within noise, sphere scene shows the coat in both orders; commit `feat(renderer): clearcoat shading`.

### Task 3: Debug view and docs

- [ ] `GBufferDebugView::Custom`, tonemap branch, Graphics Debug entry; commit.
- [ ] README bullet, spec amendments; commit `docs: clearcoat`.
