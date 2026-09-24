# Material Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move material parameters from the push constant into a per-draw storage buffer, reserve GB2.a for a shading model id, and import `KHR_materials_emissive_strength`, with no pixel change.

**Architecture:** `GpuMaterialData` (80 bytes) replaces `MaterialPushConstants`; `VulkanUniformBuffer` builds one immutable buffer of them per content at set 0 binding 12, indexed by the draw slot `triangle.vert` already receives as `gl_InstanceIndex` and now forwards as a flat varying.

**Tech Stack:** C++20, Vulkan, GLSL 450, tinygltf, CTest executables.

**Spec:** `docs/superpowers/specs/2026-09-24-material-foundation-design.md`

## Global Constraints

- `GpuMaterialData` = 5 x vec4, 80 bytes, std430: baseColorFactor, emissiveFactor + alphaCutoff, surfaceFactors, nodeGraphFactors, shadingModel (uvec4, x = id).
- `ObjectPushConstants` = `mat4 model`, 64 bytes.
- Set 0 binding 12, fragment stage, `readonly buffer MaterialBuffer`.
- `ShadingModel::DefaultLit = 0`; GB2.a = id / 255.
- No pixel change beyond run-to-run noise (max one 8-bit step, < 1% of pixels) in deferred and forward-only orders.

## Review Focus

- A scene with zero submeshes: the material buffer must still be a valid, non-empty binding.
- Materials count and motion slot count disagreeing: refused at construction, never an out-of-bounds GPU read.
- Emissive strength that is negative, non-numeric, or an integer JSON number (tinygltf stores `4` as int): negative/non-numeric ignored, integer accepted.
- Forward-only order: `triangle.frag` must read the same fields as `gbuffer.frag`.
- Mask materials in the shadow pass: alpha test still gets base colour, node graph factors and cutoff.

---

### Task 1: `KHR_materials_emissive_strength` import

**Files:** Modify `engine/asset/gltf_model_loader.cpp` (`BuildMaterialData`); Test: new case in the loader test that already loads generated glTFs (find with `grep -ln LoadModel tests/*.cpp`), or a new `tests/gltf_material_extensions_tests.cpp` registered like `miniengine.light_clusters`.

- [ ] Failing test: generated `.gltf` with one material per case — strength 4 (as `4` and `4.0`), absent, `-1`, `"x"` — expect 4, 4, 1, 1, 1.
- [ ] Implement: read the extension object's `emissiveStrength` (`IsNumber()`, `GetNumberAsDouble()`), accept `>= 0`, else warn and keep 1.
- [ ] Tests pass; commit `feat(asset): import KHR_materials_emissive_strength`.

### Task 2: Material storage buffer and shading model id

**Files:** `engine/renderer/material.h`, `engine/renderer/renderer_world.h`, `engine/editor/services/scene_renderables.cpp`, `engine/renderer/vulkan/{renderer.h,renderer.cpp,uniform_buffer.h,uniform_buffer.cpp,shadow_pass.h,shadow_pass.cpp,pipeline_set.cpp,material_draw.cpp,command.h}`, shaders `triangle.vert`, `triangle.frag`, `gbuffer.frag`, new `material_common.glsl`, `gbuffer_common.glsl`, `engine/renderer/CMakeLists.txt` (shader include list).

- [ ] Capture baselines (deferred and forward-only) with the current build at equal frame counts.
- [ ] Rename `MaterialPushConstants` → `GpuMaterialData`, add `shadingModel`; shrink `ObjectPushConstants`; static_asserts.
- [ ] Uniform buffer: materials span, buffer, binding 12, pool, count check.
- [ ] Shaders: flat draw slot, material buffer reads, GB2.a encode.
- [ ] Build, tests, captures vs baselines within noise; commit `feat(renderer): per-draw material buffer and shading model id`.

### Task 3: Docs

- [ ] README bullet, spec amendments; commit `docs: material foundation`.
