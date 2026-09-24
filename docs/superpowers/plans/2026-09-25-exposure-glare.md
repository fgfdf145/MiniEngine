# Exposure-Driven Glare Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Replace bloom's fixed 4% mix with Airy-diffraction glare whose F-number follows the exposure.

**Architecture:** A pure `glare.h/.cpp` computes the F-number and per-level rgb energies; the bloom pass pushes them per dispatch; `bloom.comp` weights each level in the upsample and composites energy-conserving.

**Spec:** `docs/superpowers/specs/2026-09-24-exposure-glare-design.md`

## Global Constraints

- Shutter 1/125 s, N clamped to [1.4, 22], sensor height 24 mm, wavelengths 612/549/465 nm.
- Band radii `R_k = 2^(k+1)` px; the last level takes `K / R_last`; energy within 2 px stays.
- Bloom off must render as the previous build (the composite is not dispatched then, as today).
- User's uncommitted files stay out of commits (`git apply --cached -R` of the saved user patch for `renderer.cpp`/`.h`).

## Review Focus

1. Viewport so small the chain has one level: that level carries all of `K / 2`.
2. EV outside [-2, 18] (manual slider extremes): N stays clamped, no NaN.
3. Strength 0: image equals bloom off.

### Task 1: Glare model (TDD)

**Files:** Create `engine/renderer/glare.h`, `engine/renderer/glare.cpp`, `tests/glare_tests.cpp`; modify `engine/renderer/CMakeLists.txt`, `tests/CMakeLists.txt`.

**Produces:** `float GlareFNumberFromEv100(float ev100)`; `std::vector<glm::vec3> ComputeGlareBands(float fNumber, uint32_t viewportHeight, size_t levelCount, float strength)`; constants `kGlareShutterSeconds`, `kGlareMinFNumber`, `kGlareMaxFNumber`, `kGlareSensorHeightMicrons`, `kGlareWavelengthsMicrons`.

- [ ] Tests: f/20 at EV 15.6 (within 0.5), clamps at EV -2 and 20, monotonic; bands sum to `K/2` per channel; band k = `K / 2^(k+2)` below the last, and the last (`K / R_last`) equals the band before it; R > G > B; strength 0 → zeros; one level → sum `K/2`; resolution independence: energy beyond a fixed angle (sum of bands from the level at 2x radius at 2x height) matches.
- [ ] RED (link error), implement, GREEN, commit `feat(renderer): diffraction glare model`.

### Task 2: Bloom pass and shader

**Files:** `engine/renderer/render_types.h` (`BloomSettings{enabled, strength}`), `engine/editor/ui/editor_misc_panels.cpp`, `engine/renderer/vulkan/scene_pass.h` (`glareFNumber`), `engine/renderer/vulkan/renderer.cpp`, `engine/renderer/vulkan/bloom_pass.cpp/.h`, `shaders/vulkan/bloom.comp`, plus any other `bloom.intensity` user (grep).

- [ ] Push block: `uvec2 destinationExtent; vec2 sourceTexelSize; uint mode; 3 x float pad; vec4 destinationWeight; vec4 sourceWeight;` (64 bytes). Upsample: `result = own * destinationWeight + tent * sourceWeight`, where the first upsample passes `sourceWeight = band_last`, later ones 1, and `destinationWeight = band_i`. With one level no upsample runs, so the composite multiplies level 0 by `sourceWeight = band_0`. Composite: `scene * destinationWeight + level0 * sourceWeight` with `destinationWeight = 1 - total`, `sourceWeight` = 1 (or band_0 for a one-level chain).
- [ ] Renderer: `frame.glareFNumber = GlareFNumberFromEv100(State().camera.exposureEv100)`.
- [ ] UI: Strength drag 0-4, text "Aperture f/%.1f (from exposure)".
- [ ] Build, ctest, commit `feat(renderer): exposure-driven glare`.

### Task 3: Acceptance captures and docs

- [ ] Instrumented new binary; captures: sphere 2e7, Sponza deferred, Sponza bloom off; compare with the pre-exposed build (`new` from the previous plan) for bloom off (expect within noise).
- [ ] README bullet (replace the Bloom bullet's mix description), spec amendments, memory; commit `docs: exposure-driven glare`.
