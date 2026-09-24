# Specular AA Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Geometric specular anti-aliasing for the base and clearcoat roughness, switchable, off = today's image.

**Architecture:** A pure `FilterRoughnessForSpecularAA` (tested) and its GLSL twin in `specular_aa.glsl`, called from `gbuffer.frag` and `triangle.frag`; settings reach the shaders through a vec4 appended to the camera block.

**Tech Stack:** C++20, GLSL 450, CTest executables.

**Spec:** `docs/superpowers/specs/2026-09-24-specular-aa-design.md`

## Global Constraints

- `sigma^2 = 0.15`, threshold `0.2`, `alpha = roughness^2`, result clamped to [0, 1] in alpha^2.
- Camera block: `vec4 specularAntiAliasing` (x enabled, y variance, z threshold) appended after `viewProjNoJitter`.
- Default on; off skips the filter.

## Review Focus

- Flat surfaces must be bit-identical with the filter on (zero derivatives).
- Derivatives across triangle edges and helper lanes: bounded by the threshold.
- The coat must use the geometric normal's derivatives, not the normal-mapped one's.
- The deferred path must write the filtered roughness to GB2 (8-bit), not filter again in lighting.

---

### Task 1: Pure filter
- [ ] Failing tests `tests/specular_aa_tests.cpp`; `specular_aa.{h,cpp}`; commit `feat(renderer): specular AA roughness filter`.

### Task 2: Shaders and switch
- [ ] Baselines; camera block vec4; `specular_aa.glsl`; `gbuffer.frag`, `triangle.frag`; setting and checkbox.
- [ ] Off within noise; coat spheres on/off with TAA; commit `feat(renderer): specular anti-aliasing`.

### Task 3: Docs
- [ ] README bullet, spec amendments; commit `docs: specular AA`.
