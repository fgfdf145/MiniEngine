# Pre-Exposed HDR Buffer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Store pre-exposed values (GT7 frame-buffer units, 1.0 = 100 cd/m^2 as displayed) in the HDR target and GB3 so fp16 no longer clips daylight highlights, with no other change to the image.

**Architecture:** The CPU computes `preExposure = ExposureFromEv100(ev) * 2.5` for the frame before recording and puts it in the camera uniform block. Every HDR writer multiplies its final color by it; every reader converts back where it needs physical units (the histogram) or compensates constants (TAA/bloom luma weights); TAA rescales its history by the frame-to-frame ratio; the tone mapper takes the values as they are.

**Tech Stack:** C++20, Vulkan 1.3, GLSL compiled by glslc, GLM, CMake/Ninja, the repo's plain `Require`-style test executables under `tests/`.

**Spec:** `docs/superpowers/specs/2026-09-24-pre-exposed-hdr-design.md`

## Global Constraints

- HDR target unit: 1.0 = 100 cd/m^2 as displayed; `kFrameBufferUnitsPerExposed = 2.5`.
- Pre-exposure uses this frame's EV (after `UpdateAutoExposure`), never last frame's.
- Shading stays in physical cd/m^2 and fp32; only the value written to a target is multiplied, once, after aerial perspective.
- Image must match the previous build within run-to-run noise (TAA: ~1.5% of pixels, max 11-19; no TAA: ~0.7% at 1 LSB) in Sponza, deferred with and without TAA, and forward-only.
- The user's ~20 uncommitted files stay uncommitted. `renderer.cpp` / `renderer.h` carry user hunks (DrawFrame ~line 409, RecreateSwapchain ~line 1150, header ~line 121): stage the whole file, then remove the user's hunks from the index with `git apply --cached -R` of the saved user patch.
- `renderer.cpp` starts with a UTF-8 BOM; keep it.
- Allman braces, 4-space indent, comment style of the surrounding code; run clang-format from `~/.local/share/miniengine-tools/bin` on touched C++ files.

## Review Focus

1. **Exposure changes with TAA on** (auto exposure adapting, slider drag): history must be rescaled, else a one-frame flash and trails. Pinned by the EV-jump capture in Task 4.
2. **History invalid frames** (first frame, resize, TAA toggled): `historyScale` must be 1 and the stale previous pre-exposure never used. Pinned by `TaaHistoryScale` tests in Task 1.
3. **Very bright inputs** (sun disk 1e9 cd/m^2, HDRI texels at +inf): still clamped to 65504 after pre-exposure, never inf/NaN in the target. Pinned by the sun-fits test in Task 1 and the clamp kept in `sky.frag` in Task 3.
4. **Emissive round trip** (GB3 written pre-exposed, lighting divides back): emissive must not be exposed twice. Pinned by the Sponza no-change capture and the emissive-sphere capture in Task 4.
5. **Flat background** (`EnvironmentMode::None`) must look identical at every EV and must not be metered. Pinned by the tonemap round-trip test in Task 1 and the default-scene capture in Task 4.

---

### Task 1: Pre-exposure units, helpers and tests

**Files:**
- Create: `shaders/vulkan/pre_exposure.glsl`
- Modify: `engine/renderer/exposure.h`, `engine/renderer/exposure.cpp`
- Modify: `shaders/vulkan/gt7_tonemap.glsl` (tail: engine glue)
- Modify: `shaders/vulkan/tonemap.frag` (call sites only, behavior kept: see Step 5)
- Test: `tests/exposure_tests.cpp`, `tests/tonemap_tests.cpp`

**Interfaces:**
- Produces (C++, `namespace me`, `exposure.h`):
  - `inline constexpr float kFrameBufferUnitsPerExposed = 2.5f;`
  - `inline constexpr glm::vec3 kViewportBackgroundFrameBuffer{0.223550f, 0.272803f, 0.421025f};` (replaces `kViewportBackgroundExposed`)
  - `float PreExposureFromEv100(float ev100);`
  - `float TaaHistoryScale(bool historyValid, float currentPreExposure, float historyPreExposure);`
- Produces (GLSL, `pre_exposure.glsl`): `const float kFrameBufferUnitsPerExposed = 2.5f; const float kExposedPerFrameBufferUnit = 0.4f;`
- Produces (GLSL, `gt7_tonemap.glsl`): `vec3 TonemapFrameBufferRec709(vec3 frameBufferRec709)` (replaces `TonemapExposedRec709`; no internal scale).

- [ ] **Step 1: Write failing tests** in `tests/exposure_tests.cpp` (add near the other exposure tests, register each in `main`):

```cpp
namespace pre_exposure_shader
{
using namespace glm;
#include <shaders/vulkan/pre_exposure.glsl>
}

void PreExposureIsExposureInFrameBufferUnits()
{
    for (float ev = kMinExposureEv100; ev <= kMaxExposureEv100; ev += 0.5f)
    {
        Require(
            NearlyEqual(PreExposureFromEv100(ev), ExposureFromEv100(ev) * kFrameBufferUnitsPerExposed),
            "the pre-exposure must be the exposure times the frame-buffer scale");
    }
    Require(
        pre_exposure_shader::kFrameBufferUnitsPerExposed == kFrameBufferUnitsPerExposed,
        "pre_exposure.glsl and exposure.h must agree on the frame-buffer scale");
    Require(
        NearlyEqual(pre_exposure_shader::kExposedPerFrameBufferUnit * kFrameBufferUnitsPerExposed, 1.0f),
        "the two GLSL constants must be reciprocals");
}

void SunFitsInFp16InDaylight()
{
    // The sun disk is about 1e9 cd/m^2. Pre-exposed at a sunlit EV it must be representable, which
    // it never was as raw radiance.
    for (const float ev : {15.0f, 15.6f, 18.0f})
    {
        Require(1e9f * PreExposureFromEv100(ev) < 65504.0f, "a pre-exposed sun must fit in fp16 at daylight EV");
    }
}

void TaaHistoryScaleFollowsThePreExposure()
{
    Require(NearlyEqual(TaaHistoryScale(true, 2.0f, 1.0f), 2.0f), "history is scaled by current / previous");
    Require(NearlyEqual(TaaHistoryScale(true, 1.0f, 4.0f), 0.25f), "history is scaled down when the exposure falls");
    Require(TaaHistoryScale(false, 2.0f, 1.0f) == 1.0f, "invalid history is not scaled");
    Require(TaaHistoryScale(true, 2.0f, 0.0f) == 1.0f, "a history written without a pre-exposure is not scaled");
}
```

Replace `BackgroundClearStaysInsideFp16` (it guarded the old `background / exposure` clear) by deleting it and its `main` call.

In `tests/tonemap_tests.cpp`, switch every `TonemapExposedRec709(x)` to `TonemapFrameBufferRec709(x * kFrameBufferUnitsPerExposed)` so each test keeps its meaning (4.0 exposed, `1 / 9.6` exposed, ...), and change `BackgroundConstantMatchesTheOperator` to:

```cpp
void BackgroundConstantMatchesTheOperator()
{
    const glm::vec3 displayed = shader::TonemapFrameBufferRec709(kViewportBackgroundFrameBuffer);
    Require(
        MaxAbsDifference(displayed, kViewportBackgroundDisplayLinear) <= 1e-4f,
        "kViewportBackgroundFrameBuffer must tone map to the viewport background");
}
```

Update the `MidGrayStaysWhereReinhardPutIt` comment: `kFrameBufferUnitsPerExposed` (not `kExposedToGt7FrameBuffer`) was chosen so that point displays as under Reinhard.

- [ ] **Step 2: Build the two tests and confirm they fail to compile** (missing symbols).

Run: `PATH=~/.local/share/miniengine-tools/lib/python3.9/site-packages/cmake/data/bin:$PATH cmake --build out/build/macos-debug --target miniengine_exposure_tests miniengine_tonemap_tests`
Expected: errors naming `PreExposureFromEv100`, `pre_exposure.glsl`, `TonemapFrameBufferRec709`, `kViewportBackgroundFrameBuffer`.

- [ ] **Step 3: Implement.**

`shaders/vulkan/pre_exposure.glsl`:

```glsl
// The HDR target's unit, shared by the shaders that convert to or from it and by
// engine/renderer/exposure.h, which tests/exposure_tests.cpp checks against this file. Like
// gt7_tonemap.glsl it is written in the subset GLSL and C++/GLM both accept.
//
// The HDR target and GB3 hold pre-exposed values in GT7's frame-buffer unit, where 1.0 is
// 100 cd/m^2 as displayed: physical radiance times ExposureFromEv100 (1.0 = sensor saturation)
// times this scale, which places saturation at GT7's 250 cd/m^2 SDR paper white.
const float kFrameBufferUnitsPerExposed = 2.5f;
const float kExposedPerFrameBufferUnit = 0.4f;
```

`exposure.h`: replace the `kMaxExposureEv100` comment's last sentence ("The top end is also bounded by the HDR target ...") with nothing (drop it); replace the background block with:

```cpp
// The HDR target's unit (shaders/vulkan/pre_exposure.glsl): pre-exposed values, where 1.0 is
// 100 cd/m^2 as displayed, GT7's frame-buffer unit. An exposed value (1.0 = sensor saturation, see
// ExposureFromEv100) times this lands saturation on GT7's 250 cd/m^2 SDR paper white.
inline constexpr float kFrameBufferUnitsPerExposed = 2.5f;

// The editor viewport's background in HDR target units: what the tone mapping pass
// (TonemapFrameBufferRec709 in shaders/vulkan/gt7_tonemap.glsl) turns into the display-linear
// {0.08, 0.1, 0.16}. It stands for no physical light, so it is written as is at every exposure.
// Solved numerically against that operator; tests/tonemap_tests.cpp checks it still round-trips.
inline constexpr glm::vec3 kViewportBackgroundFrameBuffer{0.223550f, 0.272803f, 0.421025f};
inline constexpr glm::vec3 kViewportBackgroundDisplayLinear{0.08f, 0.1f, 0.16f};
```

and after `ExposureFromEv100`:

```cpp
// Physical radiance (cd/m^2) to the HDR target's unit: the scale every writer of the HDR target
// and GB3 applies. ExposureFromEv100(ev100) * kFrameBufferUnitsPerExposed.
float PreExposureFromEv100(float ev100);

// What TAA multiplies its history by: the history was written at historyPreExposure and this frame
// writes at currentPreExposure. 1 when there is no valid history or it carries no pre-exposure.
float TaaHistoryScale(bool historyValid, float currentPreExposure, float historyPreExposure);
```

`exposure.cpp`:

```cpp
float PreExposureFromEv100(float ev100)
{
    return ExposureFromEv100(ev100) * kFrameBufferUnitsPerExposed;
}

float TaaHistoryScale(bool historyValid, float currentPreExposure, float historyPreExposure)
{
    if (!historyValid || !(historyPreExposure > 0.0f))
    {
        return 1.0f;
    }
    return currentPreExposure / historyPreExposure;
}
```

`gt7_tonemap.glsl` tail: delete `kExposedToGt7FrameBuffer` and replace `TonemapExposedRec709` with:

```glsl
// HDR target values (GT7 frame-buffer units, linear Rec.709; see pre_exposure.glsl) in,
// display-referred linear Rec.709 in [0, 1] out.
vec3 TonemapFrameBufferRec709(vec3 frameBufferRec709)
{
    vec3 rec2020 = max(frameBufferRec709, vec3(0.0f)) * kRec709ToRec2020;
    vec3 mapped = Gt7ApplyToneMapping(Gt7InitializeAsSdr(), rec2020);
    return clamp(mapped * kRec2020ToRec709, vec3(0.0f), vec3(1.0f));
}
```

- [ ] **Step 4: Keep `tonemap.frag` compiling with unchanged output** until Task 3: include `pre_exposure.glsl` and replace each `TonemapExposedRec709(v)` by `TonemapFrameBufferRec709(v * kFrameBufferUnitsPerExposed)`. Grep for any other user of the removed names:

Run: `grep -rn "TonemapExposedRec709\|kExposedToGt7FrameBuffer\|kViewportBackgroundExposed" engine shaders tests app`
Expected after edits: only `GetBackgroundRadiance` in `scene_pass.h` uses `kViewportBackgroundExposed`; change it to `return kViewportBackgroundFrameBuffer / kFrameBufferUnitsPerExposed / exposure;` for now (same value as before; Task 3 removes the function).

- [ ] **Step 5: Build everything and run all tests.**

Run: `cmake --build out/build/macos-debug && ctest --test-dir out/build/macos-debug --output-on-failure`
Expected: build succeeds (shaders included), all tests pass.

- [ ] **Step 6: Commit.**

```bash
git add shaders/vulkan/pre_exposure.glsl shaders/vulkan/gt7_tonemap.glsl shaders/vulkan/tonemap.frag engine/renderer/exposure.h engine/renderer/exposure.cpp engine/renderer/vulkan/scene_pass.h tests/exposure_tests.cpp tests/tonemap_tests.cpp
git commit -m "feat(renderer): pre-exposure units and helpers"
```

---

### Task 2: Pre-exposure in the camera uniform block

**Files:**
- Modify: `engine/renderer/vulkan/uniform_buffer.h` (`CameraUniformData`, static asserts, `Update` declaration)
- Modify: `engine/renderer/vulkan/uniform_buffer.cpp` (`Update`)
- Modify: `shaders/vulkan/scene_common.glsl` (`CameraBuffer`)
- Modify: `engine/renderer/vulkan/renderer.cpp` (the `m_uniformBuffer->Update` call)

**Interfaces:**
- Consumes: `PreExposureFromEv100` (Task 1).
- Produces: `ubo.exposure` in GLSL: x = pre-exposure, y = 1 / pre-exposure, zw = 0. `VulkanUniformBuffer::Update(..., bool specularAntiAliasing, float preExposure)`.

- [ ] **Step 1: Extend the layout checks first.** In `uniform_buffer.h` append after `specularAntiAliasing`:

```cpp
    // x = pre-exposure, physical radiance to HDR target units (see PreExposureFromEv100);
    // y = 1 / pre-exposure; zw unused. Appended last.
    glm::vec4 exposure{1.0f, 1.0f, 0.0f, 0.0f};
```

and update the size assertion to `... + 64 + 16 + 16`, adding:

```cpp
static_assert(
    offsetof(CameraUniformData, exposure) ==
        kCameraBlockHeaderBytes + kShadowCascadeCount * 64 + 3 * 16 + 64 + 64 + 18 * 16 + 64 + 16,
    "exposure must follow specularAntiAliasing with no padding");
```

- [ ] **Step 2: Mirror in `scene_common.glsl`** after `specularAntiAliasing`:

```glsl
    // x = pre-exposure (physical radiance to HDR target units, see pre_exposure.glsl), y = its
    // inverse. Every writer of the HDR target and GB3 multiplies its final value by x.
    vec4 exposure;
```

- [ ] **Step 3: Fill it.** Add `float preExposure` as the last `Update` parameter (header and cpp); in the body: `data.exposure = glm::vec4(preExposure, 1.0f / preExposure, 0.0f, 0.0f);`. In `renderer.cpp`, before the `m_uniformBuffer->Update(` call:

```cpp
    // This frame's EV, already adapted by UpdateAutoExposure, so every writer and reader of the
    // HDR target agrees on one pre-exposure.
    const float preExposure = PreExposureFromEv100(State().camera.exposureEv100);
```

and pass `preExposure` as the last argument.

- [ ] **Step 4: Build and run tests.** Run: `cmake --build out/build/macos-debug && ctest --test-dir out/build/macos-debug --output-on-failure`. Expected: pass (static asserts hold, no shader reads the field yet).

- [ ] **Step 5: Commit** (renderer.cpp with the user's hunks removed from the index):

```bash
git diff engine/renderer/vulkan/renderer.cpp engine/renderer/vulkan/renderer.h > $SCRATCH/user-renderer.patch   # saved once, before Task 2's edit
git add engine/renderer/vulkan/uniform_buffer.h engine/renderer/vulkan/uniform_buffer.cpp shaders/vulkan/scene_common.glsl engine/renderer/vulkan/renderer.cpp
git apply --cached -R $SCRATCH/user-renderer.patch
git diff --cached --stat   # renderer.cpp shows only this task's lines
git commit -m "feat(renderer): pre-exposure in the camera uniform block"
```

---

### Task 3: Switch the HDR target to pre-exposed values

One atomic change: any subset renders wrong.

**Files:**
- Modify shaders: `deferred_lighting.frag`, `triangle.frag`, `sky.frag`, `gbuffer.frag`, `taa_resolve.comp`, `bloom.comp`, `exposure_histogram.comp`, `tonemap.frag`
- Modify C++: `scene_pass.h` (`ScenePassFrameContext`, remove `GetBackgroundRadiance`), `forward_pass.cpp`, `lighting_pass.cpp`, `taa_pass.cpp`, `bloom_pass.cpp`, `exposure_histogram_pass.cpp`, `tonemap_pass.cpp`, `renderer.cpp`, `renderer.h`

**Interfaces:**
- Consumes: `ubo.exposure` (Task 2); `kViewportBackgroundFrameBuffer`, `TaaHistoryScale`, `pre_exposure.glsl` (Task 1).
- Produces: `ScenePassFrameContext::taaHistoryScale` (float, 1 by default) replacing `exposure`; `VulkanRenderer::m_taaHistoryPreExposure` (float, 0 = none).

- [ ] **Step 1: Writers.**
  - `gbuffer.frag`: `outEmissive = vec4(emissiveSample * material.emissiveFactor * ubo.exposure.x, 0.0);` with a comment that GB3 holds HDR-target units so bright emissives do not clip in B10G11R11.
  - `deferred_lighting.frag`: `vec3 emissive = texture(gbufferEmissive, fragTexCoord).rgb * ubo.exposure.y;` (comment: GB3 is pre-exposed; shading runs in physical units); final line `outColor = vec4(ApplyAerialPerspective(color, worldPosition) * ubo.exposure.x, 1.0);`; heat map `outColor = vec4(LightCountHeat(localLights) * kFrameBufferUnitsPerExposed, 1.0);` with `#include "pre_exposure.glsl"`; update the push-constant comments (background is `kViewportBackgroundFrameBuffer`; `debug.y` unused).
  - `triangle.frag`: `color = ApplyAerialPerspective(color, fragWorldPosition) * ubo.exposure.x;` and replace the trailing "writes linear radiance" comment with "writes pre-exposed radiance (see pre_exposure.glsl)".
  - `sky.frag`: `outColor = vec4(min(luminance * ubo.exposure.x, vec3(65504.0)), 1.0);` comment: pre-exposed, the sun disk now fits at daylight EV, the clamp stays for HDRI texels and low EVs. Push-constant comment: `xyz = kViewportBackgroundFrameBuffer`.
- [ ] **Step 2: Background constant.** Delete `GetBackgroundRadiance` from `scene_pass.h`; in `forward_pass.cpp` (clear and `RecordSky`) and `lighting_pass.cpp` use `kViewportBackgroundFrameBuffer`, with the comment that it stands for no physical light and is written as is at every exposure. In `lighting_pass.cpp` set `constants.debug = glm::vec4(heatMap ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);` and fix its struct comment (`y unused`).
- [ ] **Step 3: TAA.** `taa_pass.cpp`: rename `TaaPushConstants::exposure` to `historyScale` (comment: history is multiplied by it, current / previous pre-exposure); `constants.historyScale = frame.taaHistoryScale;`. `taa_resolve.comp`: rename the member likewise; `#include "pre_exposure.glsl"`; after `vec3 history = SampleHistoryCatmullRom(previousUv);` add `history *= taa.historyScale;` with a comment; weights become `Luma(x) * kExposedPerFrameBufferUnit`; update the header comment's "exposed luma".
- [ ] **Step 4: Bloom.** `bloom_pass.cpp`: rename `exposure` to `unused` and drop the assignment. `bloom.comp`: add `#extension GL_GOOGLE_include_directive : require` and `#include "pre_exposure.glsl"`; rename the member; `KarisWeight` uses `* kExposedPerFrameBufferUnit` (comment: the same exposed luma as before the unit change).
- [ ] **Step 5: Histogram.** `HistogramPushConstants::unused` becomes `float invPreExposure = 1.0f;` (C++ and GLSL), set from `1.0f / frame.preExposure`. Add `float preExposure = 1.0f;` to `ScenePassFrameContext` for it. In the shader: `vec3 radiance = min(texelFetch(hdrTexture, pixel, 0).rgb, vec3(65504.0)) * constants.invPreExposure;` and reword the background comment (the flat background is a constant in target units, not radiance).
- [ ] **Step 6: Tone mapping.** `tonemap.frag`: shaded view `color = TonemapFrameBufferRec709(min(texture(hdrTexture, fragTexCoord).rgb, vec3(65504.0)));`; emissive view `TonemapFrameBufferRec709(min(texture(gbufferEmissive, ...).rgb, vec3(65504.0)))`; heat view `min(texture(hdrTexture, ...).rgb * kExposedPerFrameBufferUnit, vec3(1.0))`; drop the `exposure` member (rename to `unused` float, keep layout) and fix comments. `tonemap_pass.cpp`: same rename, drop the assignment.
- [ ] **Step 7: Frame context and renderer.** In `ScenePassFrameContext` replace `exposure` with:

```cpp
    // Physical radiance to HDR target units, the same value as the camera block's exposure.x.
    float preExposure = 1.0f;
    // What TAA multiplies its history by (see TaaHistoryScale): 1 without valid history.
    float taaHistoryScale = 1.0f;
```

In `renderer.cpp` replace `frame.exposure = State().camera.GetExposure();` by `frame.preExposure = preExposure;`, and after `frame.taaHistory = m_taaHistory.Advance(taaEnabled);`:

```cpp
    frame.taaHistoryScale = TaaHistoryScale(frame.taaHistory.valid, preExposure, m_taaHistoryPreExposure);
    // The history this frame writes carries this frame's pre-exposure.
    m_taaHistoryPreExposure = preExposure;
```

In `renderer.h` next to `m_taaHistory`: `float m_taaHistoryPreExposure = 0.0f; // the pre-exposure the TAA history was written with`.

Run: `grep -rn "frame.exposure\|GetBackgroundRadiance\|\.exposure\b" engine/renderer/vulkan shaders/vulkan`
Expected: only `ubo.exposure` uses in shaders.

- [ ] **Step 8: Build, run tests, format.** `cmake --build out/build/macos-debug && ctest --test-dir out/build/macos-debug --output-on-failure`; clang-format the touched C++ files; rebuild.
- [ ] **Step 9: Commit** (renderer.cpp / renderer.h with the user's hunks reverse-applied from the index as in Task 2):

```bash
git add shaders/vulkan/*.frag shaders/vulkan/*.comp engine/renderer/vulkan/scene_pass.h engine/renderer/vulkan/forward_pass.cpp engine/renderer/vulkan/lighting_pass.cpp engine/renderer/vulkan/taa_pass.cpp engine/renderer/vulkan/bloom_pass.cpp engine/renderer/vulkan/exposure_histogram_pass.cpp engine/renderer/vulkan/tonemap_pass.cpp engine/renderer/vulkan/renderer.cpp engine/renderer/vulkan/renderer.h
git apply --cached -R $SCRATCH/user-renderer.patch
git commit -m "feat(renderer): pre-exposed HDR target"
```

---

### Task 4: Acceptance captures

Reuse the previous session's tooling (copy into this session's scratchpad): `instrument.py` (temporary getenv block after `ApplyUiActions`, never committed), `pngdiff.py`, `few.yaml` (Sponza, five lights), `hot/` (emissive spheres 2e5 and 2e7 cd/m^2, daylight), the retrying `run()` from `run_bloom2.sh` (valid run: exit 0, exactly two resizes, last 667x541).

- [ ] **Step 1: Baseline binary (done before Task 1 touches code).** With the instrumentation applied to the working tree at `3b8a6b9` (plus the user's uncommitted files, as every earlier baseline), build and copy `out/build/macos-debug/app/miniengine_app` and its shader output directory to the scratchpad as `base` / `shaders_base`, then restore `renderer.cpp` from the saved copy. Capture base runs now (Step 3's base half).
- [ ] **Step 2: New binary** after Task 3: apply the same instrumentation plus two switches, `ME_EVJUMP` (auto exposure off; EV 5 until frame 3490, then 7, via a static frame counter) and `ME_NOSCALE` (forces `frame.taaHistoryScale = 1`); build, copy as `new` / `shaders_new`, restore `renderer.cpp`.
- [ ] **Step 3: Captures** (3500 frames Sponza `few.yaml`, 400 frames spheres): base twice and new once in deferred; base and new with `ME_NOTAA=1` and with `ME_FORWARD=1`; spheres base and new; new with `ME_EVJUMP=1`, with `ME_EVJUMP=1 ME_NOSCALE=1`, and a steady manual EV 7 run (`ME_EVJUMP=1` with the jump at frame 0 via `ME_EVSTEADY=1`).
- [ ] **Step 4: Compare** with `pngdiff.py`. Expected: new vs base within noise for the three Sponza configurations (noise = base vs base); spheres: base halos equal, new halos differ (larger around 2e7); EV jump vs steady within TAA noise, and the `ME_NOSCALE` jump clearly worse (shows the rescale is what keeps it clean).
- [ ] **Step 5: Record figures** for the spec's Amendments section.

### Task 5: Docs

**Files:** `README.md` (section 7 bullet; rewrite the bloom bullet's known-limit sentence), `docs/superpowers/specs/2026-09-24-pre-exposed-hdr-design.md` (Amendments During Implementation with Task 4 figures), memory `rendering-roadmap.md` (mark done).

- [ ] **Step 1:** README bullet in the style of its neighbors: what the HDR target and GB3 hold, where the pre-exposure comes from, TAA history rescale, the capture results, link to the spec. In the bloom bullet replace the known-limit sentence with one saying the pre-exposed target lifted it.
- [ ] **Step 2:** Spec amendments with the numbers.
- [ ] **Step 3:** Commit `docs: pre-exposed HDR`.
