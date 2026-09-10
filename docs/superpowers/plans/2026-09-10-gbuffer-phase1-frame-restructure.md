# G-Buffer Phase One: Frame Restructure Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Introduce an HDR intermediate target, an explicit pass and barrier abstraction, and a standalone tone mapping pass, leaving the rendered image pixel-identical and the forward path still the only shading path.

**Architecture:** `SceneRenderTargets` owns every offscreen image and `RenderTargetLayoutTracker` owns their layouts; each pass is a small class declaring which targets it reads and writes, and the renderer's frame function becomes a loop over a pass list. The scene renders into an `R16G16B16A16_SFLOAT` target which a tone mapping pass resolves into the sRGB image ImGui samples. `VulkanSceneViewport` is deleted and its responsibilities are split across these units.

**Tech Stack:** C++20, Vulkan 1.3 headers, SDL3, Dear ImGui, GLM, EnTT, CMake + vcpkg, glslc.

**Spec:** [docs/superpowers/specs/2026-09-10-gbuffer-deferred-design.md](../specs/2026-09-10-gbuffer-deferred-design.md)

## Global Constraints

- Language standard is C++20. Formatting is Allman throughout; `scripts/check-format.ps1` is a gate.
- All engine code lives in `namespace me`.
- Target dependency direction may not be reversed. This work lands entirely inside `engine_renderer`, plus one new test target.
- `CMakePresets.json` is the only source of truth for build parameters. Configure with `cmake --preset vs2026-x64` and build with `cmake --build --preset vs2026-x64-debug --parallel`. `CMakePresets.json` defines no `testPresets`, so tests run as `ctest --test-dir out/build/vs2026-x64 -C Debug`. These are the forms README section 6 documents; `ctest --preset ...` does not work in this repo.
- Texture loading applies no vertical flip: UV origin is top-left, row 0 is `v0`. Never introduce a `1 - v` compensation.
- World units are metres, per `engine/scene/world_units.h`.
- `ObjectPushConstants` must stay exactly 128 bytes; its existing `static_assert`s in `engine/renderer/material.h` may not be relaxed.
- Validation layers are enabled in Debug builds. Zero validation messages is a gate on every task that changes recording.
- **All layout changes in the frame are explicit `vkCmdPipelineBarrier` calls.** Every render pass this plan creates declares `initialLayout == finalLayout` for each attachment so that no render pass performs an implicit transition behind `RenderTargetLayoutTracker`'s back. This is a deliberate break from `VulkanSceneViewport`, which relied on attachment transitions and subpass dependencies.

## Deferred to Phase Two

Recorded here so no task reaches for them:

- The five G-buffer targets, `gbuffer.frag`, `deferred_lighting.frag` and the lighting pass.
- `pbr_common.glsl`, `gbuffer_common.glsl`, `#extension GL_GOOGLE_include_directive`, and listing `.glsl` files in shader `DEPENDS`. Phase one has no shader include, so the include machinery lands with its first consumer.
- `invViewProj` on the camera uniform buffer. Only the lighting pass reads it; adding it now would be an untested field.
- Descriptor set 2 (G-buffer inputs).
- `BuildScenePassOrder` and the forward comparison toggle. Phase one has exactly one pass order.
- Depth attachment `storeOp`. Phase one keeps `VK_ATTACHMENT_STORE_OP_DONT_CARE`, matching today; phase two changes it to `STORE` when the lighting pass samples depth.

## File Structure

**Create:**

| File | Responsibility |
| --- | --- |
| `engine/renderer/vulkan/render_target_layout.h` / `.cpp` | `RenderTargetId`, `RenderPassIo`, `TargetTransition`, `RenderTargetLayoutTracker`. Pure logic; calls no Vulkan entry point. |
| `engine/renderer/vulkan/format_support.h` / `.cpp` | `ChooseFormat`: first candidate satisfying required format features, with an injectable query. Pure logic. |
| `engine/renderer/vulkan/scene_render_targets.h` / `.cpp` | Owns every offscreen image, view, memory and the ImGui texture bindings. Two indexing schemes, separate accessors. |
| `engine/renderer/vulkan/scene_pass.h` | `IScenePass` and `ScenePassFrameContext`. |
| `engine/renderer/vulkan/forward_pass.h` / `.cpp` | Render pass and framebuffers for HDR + depth; records the material draw items. |
| `engine/renderer/vulkan/tonemap_pass.h` / `.cpp` | Render pass, framebuffers, pipeline, descriptor sets for the HDR to LDR resolve. |
| `shaders/vulkan/fullscreen.vert` | Full-screen triangle from `gl_VertexIndex`. |
| `shaders/vulkan/tonemap.frag` | Samples HDR, applies Reinhard, writes LDR. |
| `tests/scene_pass_tests.cpp` | Unit tests for the two pure-logic units. |

**Modify:**

| File | Change |
| --- | --- |
| `engine/renderer/CMakeLists.txt` | List-driven shader compilation; source list updated. |
| `engine/renderer/vulkan/command.h` | `kMaxFramesInFlight` promoted to public; add `GetCurrentFrame()`. |
| `engine/renderer/vulkan/uniform_buffer.h` / `.cpp` | Split one 14-binding layout into set 0 (uniform buffer) and set 1 (13 samplers). |
| `engine/renderer/vulkan/pipeline_set.h` / `.cpp` | Accept two descriptor set layouts. |
| `engine/renderer/vulkan/renderer.h` / `.cpp` | Replace `m_sceneViewportLayer` with targets, tracker and the pass list. |
| `shaders/vulkan/triangle.frag` | Texture bindings move to set 1; Reinhard removed in Task 6. |
| `tests/CMakeLists.txt` | Register `miniengine_scene_pass_tests`. |

**Delete:**

| File | Replaced by |
| --- | --- |
| `engine/renderer/vulkan/scene_viewport.h` / `.cpp` | `SceneRenderTargets` (images, views, ImGui bindings) and `VulkanForwardPass` (render pass, framebuffers). |

---

### Task 1: List-driven shader compilation

Pure build refactor. No shader source changes, no behavior change. It exists so Tasks 5 and 6 add a shader by adding one list entry rather than by copying a block.

**Files:**
- Modify: `engine/renderer/CMakeLists.txt:73-95`

**Interfaces:**
- Consumes: nothing.
- Produces: a CMake function `miniengine_add_shaders(<target> <source> ...)` that compiles each listed shader from `${PROJECT_SOURCE_DIR}/shaders/vulkan/` into `${MINIENGINE_SHADER_OUTPUT_DIR}/<name>.spv` and attaches them all to one custom target.

- [ ] **Step 1: Replace the hard-coded shader block**

In `engine/renderer/CMakeLists.txt`, delete the block from `set(MINIENGINE_VERTEX_SHADER ...)` through `add_custom_target(engine_renderer_shaders DEPENDS ...)` and put this in its place:

```cmake
# MINIENGINE_SHADER_OUTPUT_DIR is set in MiniEngineBuildOptions.cmake so that
# engine_core can bake the same value in as the EnginePaths fallback.
#
# Every shader stage the renderer loads is listed here by file name. The compiled name is the
# source name plus ".spv", which is what VulkanShaderModule is given at load time.
set(MINIENGINE_SHADER_SOURCES
    triangle.vert
    triangle.frag
)

function(miniengine_add_shaders shader_target)
    set(spv_outputs "")
    foreach(shader_name IN LISTS ARGN)
        set(shader_source "${PROJECT_SOURCE_DIR}/shaders/vulkan/${shader_name}")
        set(shader_output "${MINIENGINE_SHADER_OUTPUT_DIR}/${shader_name}.spv")
        add_custom_command(
            OUTPUT "${shader_output}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${MINIENGINE_SHADER_OUTPUT_DIR}"
            COMMAND ${Vulkan_GLSLC_EXECUTABLE} "${shader_source}" -o "${shader_output}"
            DEPENDS "${shader_source}"
            COMMENT "Compiling ${shader_name}"
            VERBATIM
        )
        list(APPEND spv_outputs "${shader_output}")
    endforeach()

    add_custom_target(${shader_target} DEPENDS ${spv_outputs})
endfunction()

miniengine_add_shaders(engine_renderer_shaders ${MINIENGINE_SHADER_SOURCES})
```

- [ ] **Step 2: Configure and build**

Run: `cmake --build --preset vs2026-x64-debug --parallel --target engine_renderer_shaders`
Expected: SUCCESS. `triangle.vert.spv` and `triangle.frag.spv` appear in the shader output directory.

- [ ] **Step 3: Verify the compiled names did not change**

Run: `ls out/build/vs2026-x64/shaders/vulkan/` (or wherever `MINIENGINE_SHADER_OUTPUT_DIR` resolves to, which the configure output prints)
Expected: exactly `triangle.vert.spv` and `triangle.frag.spv`. `VulkanPipelineSet` loads both by name from `EnginePaths::ShaderRoot()`, so a renamed output breaks at runtime rather than at build time. Do not use `git stash` to compare against the previous build: this checkout is shared with the user's IDE.

- [ ] **Step 4: Build the whole preset and run the app**

Run: `cmake --build --preset vs2026-x64-debug --parallel`
Expected: SUCCESS.

Run: `out/build/vs2026-x64/app/Debug/miniengine_app.exe --backend vulkan --frames 60`
Expected: exit code 0, no validation messages.

- [ ] **Step 5: Commit**

```bash
git add engine/renderer/CMakeLists.txt
git commit -m "build: compile shaders from a list"
```

---

### Task 2: Layout tracker and format chooser

The two pure-logic units in phase one, both written test-first. Neither calls a Vulkan entry point, so both are genuinely unit testable — and layout tracking is exactly where a multi-pass frame grows silent bugs.

**Files:**
- Create: `engine/renderer/vulkan/render_target_layout.h`, `engine/renderer/vulkan/render_target_layout.cpp`
- Create: `engine/renderer/vulkan/format_support.h`, `engine/renderer/vulkan/format_support.cpp`
- Create: `tests/scene_pass_tests.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: `engine/renderer/CMakeLists.txt` (source list)

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `enum class RenderTargetId : uint32_t { SceneDepth, SceneHdr, SceneLdr, Count }` and `inline constexpr size_t kRenderTargetCount`.
  - `enum class RenderTargetKind { Color, Depth }`, `RenderTargetKind GetRenderTargetKind(RenderTargetId)`.
  - `struct RenderPassIo { std::span<const RenderTargetId> reads; std::span<const RenderTargetId> writes; }`.
  - `struct TargetTransition { RenderTargetId target; VkImageLayout oldLayout; VkImageLayout newLayout; }` with defaulted `operator==`.
  - `class RenderTargetLayoutTracker` with `void Reset()`, `VkImageLayout GetLayout(RenderTargetId) const`, `std::vector<TargetTransition> Transition(const RenderPassIo&)`.
  - `VkImageLayout GetWriteLayout(RenderTargetId)` and `inline constexpr VkImageLayout kReadLayout`.
  - `using FormatFeatureQuery = std::function<VkFormatFeatureFlags(VkFormat)>` and `VkFormat ChooseFormat(std::span<const VkFormat>, VkFormatFeatureFlags, const FormatFeatureQuery&)`.

- [ ] **Step 1: Write the failing tests**

Create `tests/scene_pass_tests.cpp`:

```cpp
#include <engine/renderer/vulkan/format_support.h>
#include <engine/renderer/vulkan/render_target_layout.h>

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// main() stays in the global namespace; everything it drives lives in me::.
using namespace me;

namespace
{
void Require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

RenderPassIo MakeIo(
    std::span<const RenderTargetId> reads,
    std::span<const RenderTargetId> writes)
{
    RenderPassIo io{};
    io.reads = reads;
    io.writes = writes;
    return io;
}

void UndefinedColorWriteBecomesColorAttachment()
{
    RenderTargetLayoutTracker tracker;
    const std::array<RenderTargetId, 1> writes = {RenderTargetId::SceneHdr};

    const std::vector<TargetTransition> transitions = tracker.Transition(MakeIo({}, writes));

    Require(transitions.size() == 1, "a first write must produce one transition");
    Require(transitions[0].target == RenderTargetId::SceneHdr, "wrong target in transition");
    Require(transitions[0].oldLayout == VK_IMAGE_LAYOUT_UNDEFINED, "a fresh target must start undefined");
    Require(
        transitions[0].newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        "a color write must target the color attachment layout");
    Require(
        tracker.GetLayout(RenderTargetId::SceneHdr) == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        "Transition must record the new layout");
}

void DepthWriteBecomesDepthAttachment()
{
    RenderTargetLayoutTracker tracker;
    const std::array<RenderTargetId, 1> writes = {RenderTargetId::SceneDepth};

    const std::vector<TargetTransition> transitions = tracker.Transition(MakeIo({}, writes));

    Require(transitions.size() == 1, "a first depth write must produce one transition");
    Require(
        transitions[0].newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        "a depth write must not use the color attachment layout");
}

void WrittenThenReadBecomesShaderRead()
{
    RenderTargetLayoutTracker tracker;
    const std::array<RenderTargetId, 1> hdr = {RenderTargetId::SceneHdr};
    tracker.Transition(MakeIo({}, hdr));

    const std::vector<TargetTransition> transitions = tracker.Transition(MakeIo(hdr, {}));

    Require(transitions.size() == 1, "a read after a write must produce one transition");
    Require(
        transitions[0].oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        "the old layout must be what the previous pass left behind");
    Require(
        transitions[0].newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        "a read must target the shader read only layout");
}

void AlreadyCorrectLayoutProducesNothing()
{
    RenderTargetLayoutTracker tracker;
    const std::array<RenderTargetId, 1> hdr = {RenderTargetId::SceneHdr};
    tracker.Transition(MakeIo({}, hdr));

    const std::vector<TargetTransition> transitions = tracker.Transition(MakeIo({}, hdr));

    Require(transitions.empty(), "a target already in the required layout must not be transitioned");
}

void RepeatedDeclarationIsIdempotent()
{
    RenderTargetLayoutTracker tracker;
    const std::array<RenderTargetId, 1> reads = {RenderTargetId::SceneHdr};
    const std::array<RenderTargetId, 1> writes = {RenderTargetId::SceneLdr};

    const std::vector<TargetTransition> first = tracker.Transition(MakeIo(reads, writes));
    const std::vector<TargetTransition> second = tracker.Transition(MakeIo(reads, writes));

    Require(first.size() == 2, "the first application must transition both targets");
    Require(second.empty(), "applying the same declaration twice must transition once");
}

void ResetReturnsEveryTargetToUndefined()
{
    RenderTargetLayoutTracker tracker;
    const std::array<RenderTargetId, 1> hdr = {RenderTargetId::SceneHdr};
    tracker.Transition(MakeIo({}, hdr));

    tracker.Reset();

    Require(
        tracker.GetLayout(RenderTargetId::SceneHdr) == VK_IMAGE_LAYOUT_UNDEFINED,
        "Reset must return targets to undefined so recreated images are re-transitioned");
}

void TargetInBothSpansIsRejected()
{
    RenderTargetLayoutTracker tracker;
    const std::array<RenderTargetId, 1> both = {RenderTargetId::SceneHdr};

    bool threw = false;
    try
    {
        tracker.Transition(MakeIo(both, both));
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }

    Require(threw, "a target read and written by one pass must be rejected");
}

void ChooseFormatTakesTheFirstSupportedCandidate()
{
    const std::array<VkFormat, 2> candidates = {
        VK_FORMAT_B10G11R11_UFLOAT_PACK32,
        VK_FORMAT_R16G16B16A16_SFLOAT};

    const VkFormat chosen = ChooseFormat(
        candidates,
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT,
        [](VkFormat format)
        {
            return format == VK_FORMAT_B10G11R11_UFLOAT_PACK32
                       ? static_cast<VkFormatFeatureFlags>(VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)
                       : static_cast<VkFormatFeatureFlags>(0);
        });

    Require(chosen == VK_FORMAT_B10G11R11_UFLOAT_PACK32, "the first supported candidate must win");
}

void ChooseFormatFallsThroughToALaterCandidate()
{
    const std::array<VkFormat, 2> candidates = {
        VK_FORMAT_B10G11R11_UFLOAT_PACK32,
        VK_FORMAT_R16G16B16A16_SFLOAT};

    const VkFormat chosen = ChooseFormat(
        candidates,
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT,
        [](VkFormat format)
        {
            return format == VK_FORMAT_R16G16B16A16_SFLOAT
                       ? static_cast<VkFormatFeatureFlags>(VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)
                       : static_cast<VkFormatFeatureFlags>(0);
        });

    Require(chosen == VK_FORMAT_R16G16B16A16_SFLOAT, "an unsupported first candidate must be skipped");
}

void ChooseFormatRequiresEveryRequestedFeature()
{
    // The depth target must be both a depth attachment and sampleable. A candidate offering only
    // one of the two must be skipped, which is the constraint this design newly imposes.
    const std::array<VkFormat, 2> candidates = {
        VK_FORMAT_D32_SFLOAT,
        VK_FORMAT_D24_UNORM_S8_UINT};
    const VkFormatFeatureFlags required =
        VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;

    const VkFormat chosen = ChooseFormat(
        candidates,
        required,
        [](VkFormat format)
        {
            if (format == VK_FORMAT_D32_SFLOAT)
            {
                return static_cast<VkFormatFeatureFlags>(VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
            }
            return static_cast<VkFormatFeatureFlags>(
                VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
        });

    Require(chosen == VK_FORMAT_D24_UNORM_S8_UINT, "a partially supported candidate must be skipped");
}

void ChooseFormatThrowsWhenNothingQualifies()
{
    const std::array<VkFormat, 1> candidates = {VK_FORMAT_D32_SFLOAT};

    bool threw = false;
    try
    {
        ChooseFormat(
            candidates,
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,
            [](VkFormat)
            { return static_cast<VkFormatFeatureFlags>(0); });
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }

    Require(threw, "no qualifying candidate must throw rather than return an undefined format");
}
}

int main()
{
    try
    {
        UndefinedColorWriteBecomesColorAttachment();
        DepthWriteBecomesDepthAttachment();
        WrittenThenReadBecomesShaderRead();
        AlreadyCorrectLayoutProducesNothing();
        RepeatedDeclarationIsIdempotent();
        ResetReturnsEveryTargetToUndefined();
        TargetInBothSpansIsRejected();
        ChooseFormatTakesTheFirstSupportedCandidate();
        ChooseFormatFallsThroughToALaterCandidate();
        ChooseFormatRequiresEveryRequestedFeature();
        ChooseFormatThrowsWhenNothingQualifies();

        std::cout << "scene pass tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "scene pass tests failed: " << error.what() << '\n';
        return 1;
    }
}
```

- [ ] **Step 2: Register the test target**

Append to `tests/CMakeLists.txt`:

```cmake
# The two units under test call no Vulkan entry point, so their translation units are compiled
# directly into the test rather than linking engine_renderer, which would drag the whole Vulkan
# backend and the ImGui editor into a unit test. Vulkan::Vulkan is here for the headers only.
add_executable(miniengine_scene_pass_tests
    scene_pass_tests.cpp
    ${PROJECT_SOURCE_DIR}/engine/renderer/vulkan/render_target_layout.cpp
    ${PROJECT_SOURCE_DIR}/engine/renderer/vulkan/format_support.cpp
)
miniengine_group_target_sources(miniengine_scene_pass_tests)

target_include_directories(miniengine_scene_pass_tests
    PRIVATE
        "${PROJECT_SOURCE_DIR}"
)

target_link_libraries(miniengine_scene_pass_tests
    PRIVATE
        Vulkan::Vulkan
)

add_test(
    NAME miniengine.scene_pass
    COMMAND miniengine_scene_pass_tests
)

if(WIN32)
    add_custom_command(TARGET miniengine_scene_pass_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:miniengine_scene_pass_tests>
            $<TARGET_FILE_DIR:miniengine_scene_pass_tests>
        COMMAND_EXPAND_LISTS
    )
endif()

set_target_properties(miniengine_scene_pass_tests PROPERTIES FOLDER "Tests")
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `cmake --build --preset vs2026-x64-debug --parallel --target miniengine_scene_pass_tests`
Expected: FAIL — `render_target_layout.h` and `format_support.h` do not exist.

- [ ] **Step 4: Write `render_target_layout.h`**

```cpp
#pragma once

// Deliberately not including "common.h": this header is compiled into a unit test that must not
// pull in SDL, GLM or the editor. Vulkan's own header supplies every type used below.
#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace me
{

// Every offscreen target the scene passes read or write. Phase one uses all three; the G-buffer
// ids are appended here when the deferred passes land, which is why Count is last.
enum class RenderTargetId : uint32_t
{
    SceneDepth,
    SceneHdr,
    SceneLdr,
    Count
};

inline constexpr size_t kRenderTargetCount = static_cast<size_t>(RenderTargetId::Count);

// A depth target transitions to a different attachment layout than a color target, and the
// tracker has to know which is which without ever touching a VkImage.
enum class RenderTargetKind
{
    Color,
    Depth
};

RenderTargetKind GetRenderTargetKind(RenderTargetId target);

// The layout a target must be in to be written by a pass, derived from its kind.
VkImageLayout GetWriteLayout(RenderTargetId target);

// The layout a target must be in to be sampled by a pass.
inline constexpr VkImageLayout kReadLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

// What a pass declares about the targets it touches. Reads resolve to kReadLayout, writes to
// GetWriteLayout. A target may not appear in both spans.
struct RenderPassIo
{
    std::span<const RenderTargetId> reads;
    std::span<const RenderTargetId> writes;
};

struct TargetTransition
{
    RenderTargetId target = RenderTargetId::SceneDepth;
    VkImageLayout oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout newLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    bool operator==(const TargetTransition&) const = default;
};

// Tracks the layout every target is currently in and works out the barriers a pass needs.
//
// This class calls no Vulkan entry point. Transition records the new layouts and returns the
// barriers the caller must issue; recording and bookkeeping are one call so a caller cannot
// update the tracker without issuing the barriers or the reverse.
//
// It is authoritative only because every render pass in the frame declares
// initialLayout == finalLayout for its attachments. A render pass that transitions an attachment
// implicitly would desynchronise this tracker silently.
class RenderTargetLayoutTracker
{
  public:
    RenderTargetLayoutTracker();

    // Returns every target to VK_IMAGE_LAYOUT_UNDEFINED. Call this whenever the images
    // themselves are recreated: a fresh VkImage is undefined regardless of what the destroyed
    // one was in.
    void Reset();

    VkImageLayout GetLayout(RenderTargetId target) const;

    // Throws std::runtime_error when a target appears in both spans of one declaration.
    std::vector<TargetTransition> Transition(const RenderPassIo& io);

  private:
    void RequireDisjoint(const RenderPassIo& io) const;
    void Accumulate(
        std::span<const RenderTargetId> targets,
        VkImageLayout (*resolve)(RenderTargetId),
        std::vector<TargetTransition>& transitions);

    std::array<VkImageLayout, kRenderTargetCount> m_layouts{};
};
}
```

- [ ] **Step 5: Write `render_target_layout.cpp`**

```cpp
#include "render_target_layout.h"

#include <algorithm>
#include <stdexcept>

namespace me
{

namespace
{
VkImageLayout ResolveReadLayout(RenderTargetId)
{
    return kReadLayout;
}

VkImageLayout ResolveWriteLayout(RenderTargetId target)
{
    return GetWriteLayout(target);
}
}

RenderTargetKind GetRenderTargetKind(RenderTargetId target)
{
    return target == RenderTargetId::SceneDepth ? RenderTargetKind::Depth : RenderTargetKind::Color;
}

VkImageLayout GetWriteLayout(RenderTargetId target)
{
    return GetRenderTargetKind(target) == RenderTargetKind::Depth
               ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
               : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
}

RenderTargetLayoutTracker::RenderTargetLayoutTracker()
{
    Reset();
}

void RenderTargetLayoutTracker::Reset()
{
    m_layouts.fill(VK_IMAGE_LAYOUT_UNDEFINED);
}

VkImageLayout RenderTargetLayoutTracker::GetLayout(RenderTargetId target) const
{
    return m_layouts.at(static_cast<size_t>(target));
}

std::vector<TargetTransition> RenderTargetLayoutTracker::Transition(const RenderPassIo& io)
{
    RequireDisjoint(io);

    std::vector<TargetTransition> transitions;
    transitions.reserve(io.reads.size() + io.writes.size());
    Accumulate(io.reads, &ResolveReadLayout, transitions);
    Accumulate(io.writes, &ResolveWriteLayout, transitions);
    return transitions;
}

void RenderTargetLayoutTracker::RequireDisjoint(const RenderPassIo& io) const
{
    for (RenderTargetId read : io.reads)
    {
        if (std::find(io.writes.begin(), io.writes.end(), read) != io.writes.end())
        {
            throw std::runtime_error("A render pass may not both read and write the same target");
        }
    }
}

void RenderTargetLayoutTracker::Accumulate(
    std::span<const RenderTargetId> targets,
    VkImageLayout (*resolve)(RenderTargetId),
    std::vector<TargetTransition>& transitions)
{
    for (const RenderTargetId target : targets)
    {
        const size_t index = static_cast<size_t>(target);
        const VkImageLayout required = resolve(target);
        if (m_layouts.at(index) == required)
        {
            continue;
        }

        transitions.push_back(TargetTransition{target, m_layouts.at(index), required});
        m_layouts.at(index) = required;
    }
}
}
```

Note on `RequireDisjoint`: the spec calls a target appearing in both spans "a programming error [that] asserts in Debug builds". This throws in every configuration instead, so the rule also holds in Release and can be covered by `TargetInBothSpansIsRejected`. An assert would be untestable here and would let a Release build record a contradictory declaration in silence.

- [ ] **Step 6: Write `format_support.h`**

```cpp
#pragma once

// Deliberately not including "common.h"; see render_target_layout.h for why.
#include <vulkan/vulkan.h>

#include <functional>
#include <span>

namespace me
{

// Returns the optimal-tiling format features of one format. Injected so ChooseFormat can be
// tested without a physical device; the production caller wraps
// vkGetPhysicalDeviceFormatProperties.
using FormatFeatureQuery = std::function<VkFormatFeatureFlags(VkFormat)>;

// Returns the first candidate whose optimal-tiling features include every bit in
// requiredFeatures. Throws std::runtime_error when no candidate qualifies rather than returning
// VK_FORMAT_UNDEFINED, so a missing format is a startup failure with a message instead of a
// confusing image creation error later.
VkFormat ChooseFormat(
    std::span<const VkFormat> candidates,
    VkFormatFeatureFlags requiredFeatures,
    const FormatFeatureQuery& query);
}
```

- [ ] **Step 7: Write `format_support.cpp`**

```cpp
#include "format_support.h"

#include <stdexcept>
#include <string>

namespace me
{

VkFormat ChooseFormat(
    std::span<const VkFormat> candidates,
    VkFormatFeatureFlags requiredFeatures,
    const FormatFeatureQuery& query)
{
    if (!query)
    {
        throw std::runtime_error("ChooseFormat requires a format feature query");
    }

    for (const VkFormat candidate : candidates)
    {
        if ((query(candidate) & requiredFeatures) == requiredFeatures)
        {
            return candidate;
        }
    }

    throw std::runtime_error(
        "No candidate format supports the required features (0x" +
        std::to_string(static_cast<uint32_t>(requiredFeatures)) + ")");
}
}
```

- [ ] **Step 8: Add both units to the renderer source list**

In `engine/renderer/CMakeLists.txt`, inside `add_library(engine_renderer ...)`, add in alphabetical position:

```cmake
    vulkan/format_support.cpp
    vulkan/format_support.h
    vulkan/render_target_layout.cpp
    vulkan/render_target_layout.h
```

- [ ] **Step 9: Run the tests to verify they pass**

Run: `cmake --build --preset vs2026-x64-debug --parallel --target miniengine_scene_pass_tests`
Expected: SUCCESS.

Run: `ctest --test-dir out/build/vs2026-x64 -C Debug -R miniengine.scene_pass --output-on-failure`
Expected: PASS, output `scene pass tests passed`.

- [ ] **Step 10: Run the whole suite and the format check**

Run: `ctest --test-dir out/build/vs2026-x64 -C Debug --output-on-failure`
Expected: every test passes.

Run: `scripts/check-format.ps1`
Expected: no findings.

- [ ] **Step 11: Commit**

```bash
git add engine/renderer/vulkan/render_target_layout.h engine/renderer/vulkan/render_target_layout.cpp engine/renderer/vulkan/format_support.h engine/renderer/vulkan/format_support.cpp engine/renderer/CMakeLists.txt tests/scene_pass_tests.cpp tests/CMakeLists.txt
git commit -m "feat(vulkan): add render target layout tracking and format selection"
```

---

### Task 3: Split the descriptor set layout

The riskiest step in phase one, done while the forward path is still the only path so any mistake is immediately visible in the viewport. One 14-binding layout becomes set 0 (the camera uniform buffer) and set 1 (the thirteen material samplers).

`triangle.vert` needs no change: the uniform buffer stays at set 0, binding 0. Only the fragment shader's sampler declarations move.

**Files:**
- Modify: `engine/renderer/vulkan/uniform_buffer.h:76-133`
- Modify: `engine/renderer/vulkan/uniform_buffer.cpp:97-129` and `:131-236`
- Modify: `engine/renderer/vulkan/pipeline_set.h:22-32`, `engine/renderer/vulkan/pipeline_set.cpp:38-88`
- Modify: `engine/renderer/vulkan/renderer.h:104`, `engine/renderer/vulkan/renderer.cpp:323-380`, `:740-770`, `:810-880`
- Modify: `shaders/vulkan/triangle.frag:44-57`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces:
  - `class VulkanFrameDescriptorSetLayout` with `explicit VulkanFrameDescriptorSetLayout(VkDevice)` and `VkDescriptorSetLayout GetHandle() const` — set 0, one binding, `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER`, stages vertex and fragment.
  - `VulkanMaterialDescriptorSetLayout` unchanged in name but now 13 bindings numbered 0 through 12, all `VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER`, stage fragment.
  - `VulkanUniformBuffer` constructor gains a second layout parameter: `(VkPhysicalDevice, VkDevice, uint32_t imageCount, VkDescriptorSetLayout frameSetLayout, VkDescriptorSetLayout materialSetLayout, const std::vector<MaterialTextureBinding>&)`.
  - `VkDescriptorSet VulkanUniformBuffer::GetFrameDescriptorSet(uint32_t imageIndex) const` and the existing `GetDescriptorSet(uint32_t imageIndex, uint32_t materialIndex)` now returning a material-only set.
  - `VulkanPipelineSet` constructor gains a parameter: `(VkDevice, VkPipelineCache, VkRenderPass, VkDescriptorSetLayout frameSetLayout, VkDescriptorSetLayout materialSetLayout)`.

- [ ] **Step 1: Renumber the fragment shader's sampler bindings**

In `shaders/vulkan/triangle.frag`, replace the thirteen sampler declarations (currently `set = 0, binding = 1` through `set = 0, binding = 13`) with:

```glsl
layout(set = 1, binding = 0) uniform sampler2D baseColorTexture;
layout(set = 1, binding = 1) uniform sampler2D normalTexture;
layout(set = 1, binding = 2) uniform sampler2D metallicTexture;
layout(set = 1, binding = 3) uniform sampler2D roughnessTexture;
layout(set = 1, binding = 4) uniform sampler2D occlusionTexture;
layout(set = 1, binding = 5) uniform sampler2D emissiveTexture;
layout(set = 1, binding = 6) uniform sampler2D secondaryBaseColorTexture;
layout(set = 1, binding = 7) uniform sampler2D secondaryNormalTexture;
layout(set = 1, binding = 8) uniform sampler2D secondaryMetallicTexture;
layout(set = 1, binding = 9) uniform sampler2D secondaryRoughnessTexture;
layout(set = 1, binding = 10) uniform sampler2D secondaryOcclusionTexture;
layout(set = 1, binding = 11) uniform sampler2D secondaryEmissiveTexture;
layout(set = 1, binding = 12) uniform sampler2D blendMaskTexture;
```

Leave `layout(set = 0, binding = 0) uniform CameraBuffer` exactly as it is.

- [ ] **Step 2: Add the frame set layout class**

In `engine/renderer/vulkan/uniform_buffer.h`, above `VulkanMaterialDescriptorSetLayout`, add:

```cpp
// Set 0: the per-frame camera uniform buffer. Split out from the material set so that the
// lighting and tone mapping passes — which have no material to bind — can still bind the camera
// data, and so a material reload rebuilds only set 1.
class VulkanFrameDescriptorSetLayout
{
  public:
    explicit VulkanFrameDescriptorSetLayout(VkDevice device);
    ~VulkanFrameDescriptorSetLayout();

    VulkanFrameDescriptorSetLayout(const VulkanFrameDescriptorSetLayout&) = delete;
    VulkanFrameDescriptorSetLayout& operator=(const VulkanFrameDescriptorSetLayout&) = delete;

    VkDescriptorSetLayout GetHandle() const;

  private:
    VkDevice m_device = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_layout = VK_NULL_HANDLE;
};
```

Update the comment above `VulkanMaterialDescriptorSetLayout` to say thirteen combined image samplers rather than one uniform buffer plus thirteen.

- [ ] **Step 3: Implement the frame set layout and shrink the material one**

In `engine/renderer/vulkan/uniform_buffer.cpp`, add:

```cpp
VulkanFrameDescriptorSetLayout::VulkanFrameDescriptorSetLayout(VkDevice device)
    : m_device(device)
{
    VkDescriptorSetLayoutBinding uniformBinding{};
    uniformBinding.binding = 0;
    uniformBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uniformBinding.descriptorCount = 1;
    uniformBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &uniformBinding;

    CheckVulkan(
        vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_layout),
        "Failed to create frame descriptor set layout");
}

VulkanFrameDescriptorSetLayout::~VulkanFrameDescriptorSetLayout()
{
    if (m_layout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(m_device, m_layout, nullptr);
    }
}

VkDescriptorSetLayout VulkanFrameDescriptorSetLayout::GetHandle() const
{
    return m_layout;
}
```

Then rewrite `VulkanMaterialDescriptorSetLayout`'s constructor body to declare thirteen sampler bindings numbered from zero:

```cpp
VulkanMaterialDescriptorSetLayout::VulkanMaterialDescriptorSetLayout(VkDevice device)
    : m_device(device)
{
    std::array<VkDescriptorSetLayoutBinding, 13> bindings{};
    for (uint32_t bindingIndex = 0; bindingIndex < static_cast<uint32_t>(bindings.size()); ++bindingIndex)
    {
        bindings[bindingIndex].binding = bindingIndex;
        bindings[bindingIndex].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[bindingIndex].descriptorCount = 1;
        bindings[bindingIndex].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();

    CheckVulkan(
        vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_layout),
        "Failed to create material descriptor set layout");
}
```

- [ ] **Step 4: Allocate both set kinds in `VulkanUniformBuffer`**

Change the constructor signature in the header and the definition to take `VkDescriptorSetLayout frameSetLayout` before `VkDescriptorSetLayout materialSetLayout`; store both in new members `m_frameSetLayout` and `m_materialSetLayout`, replacing `m_descriptorSetLayout`. Add a `std::vector<VkDescriptorSet> m_frameDescriptorSets;` member and this accessor:

```cpp
VkDescriptorSet VulkanUniformBuffer::GetFrameDescriptorSet(uint32_t imageIndex) const
{
    if (imageIndex >= m_imageCount)
    {
        throw std::runtime_error("Frame descriptor set image index is out of range");
    }

    return m_frameDescriptorSets[imageIndex];
}
```

In `CreateDescriptorPool`, size the pool for both kinds:

```cpp
void VulkanUniformBuffer::CreateDescriptorPool(uint32_t imageCount)
{
    const uint32_t materialSetCount = imageCount * static_cast<uint32_t>(m_materialBindings.size());
    const std::array<VkDescriptorPoolSize, 2> poolSizes = {{{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, imageCount},
                                                            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, materialSetCount * 13}}};

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    poolInfo.maxSets = materialSetCount + imageCount;

    CheckVulkan(vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool), "Failed to create uniform descriptor pool");
}
```

In `CreateDescriptorSets`, allocate `imageCount` sets from `m_frameSetLayout` into `m_frameDescriptorSets` and write binding 0 of each with that image's `VkDescriptorBufferInfo`; allocate the `imageCount * materials` sets from `m_materialSetLayout` as today but write bindings 0 through 12 with the thirteen `VkDescriptorImageInfo`s instead of bindings 1 through 13. The uniform buffer write moves out of the per-material loop entirely — that is the point of the split.

- [ ] **Step 5: Bind two set layouts in the pipeline layout**

In `engine/renderer/vulkan/pipeline_set.cpp`, replace the single-layout pipeline layout creation with:

```cpp
        const std::array<VkDescriptorSetLayout, 2> setLayouts = {frameSetLayout, materialSetLayout};

        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
        pipelineLayoutInfo.pSetLayouts = setLayouts.data();
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;
```

Update the constructor declaration in `pipeline_set.h` to match.

- [ ] **Step 6: Bind both sets at record time**

In `VulkanRenderer::RecordSceneLayer`, bind set 0 once before the draw loop and set 1 per draw item:

```cpp
    const VkDescriptorSet frameDescriptorSet = m_uniformBuffer->GetFrameDescriptorSet(imageIndex);
    vkCmdBindDescriptorSets(
        commandBuffer,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        pipelineLayout,
        0,
        1,
        &frameDescriptorSet,
        0,
        nullptr);
```

and change the existing per-draw bind's `firstSet` argument from `0` to `1`.

- [ ] **Step 7: Wire the new layout through the renderer**

Add `std::unique_ptr<VulkanFrameDescriptorSetLayout> m_frameSetLayout;` to `renderer.h` beside `m_materialSetLayout`. Create it in `CreateDeviceResources` and reset it in `DestroyDeviceResources`, both next to the material layout — it has the same device lifetime and the same reason for existing. Pass `m_frameSetLayout->GetHandle()` as the new argument at all three call sites: `CreateDescriptorResources`, `ApplyRenderContent`, and `EnsureGraphicsPipelines`.

- [ ] **Step 8: Build and run**

Run: `cmake --build --preset vs2026-x64-debug --parallel`
Expected: SUCCESS.

Run: `out/build/vs2026-x64/app/Debug/miniengine_app.exe --backend vulkan --frames 60`
Expected: exit code 0, zero validation messages. A mismatch between the shader's declared sets and the bound sets surfaces here as a validation error, which is why this task is verified by running rather than by a unit test.

- [ ] **Step 9: Confirm the image is unchanged**

Launch `out/build/vs2026-x64/app/Debug/miniengine_app.exe --backend vulkan` on the default scene. The viewport must be indistinguishable from before this task: same lighting, same textures on every material, same background. A set or binding mismatch typically shows as every surface sampling the wrong texture or turning black, so this check is decisive.

- [ ] **Step 10: Run the suite and format check**

Run: `ctest --test-dir out/build/vs2026-x64 -C Debug --output-on-failure`
Run: `scripts/check-format.ps1`
Expected: both clean.

- [ ] **Step 11: Commit**

```bash
git add engine/renderer/vulkan/uniform_buffer.h engine/renderer/vulkan/uniform_buffer.cpp engine/renderer/vulkan/pipeline_set.h engine/renderer/vulkan/pipeline_set.cpp engine/renderer/vulkan/renderer.h engine/renderer/vulkan/renderer.cpp shaders/vulkan/triangle.frag
git commit -m "refactor(vulkan): split the camera uniform buffer into its own descriptor set"
```

---

### Task 4: `SceneRenderTargets`

Owns every offscreen image. The depth and HDR targets get `VulkanCommandContext::kMaxFramesInFlight` copies because they are produced and consumed inside one command buffer; the LDR target keeps one copy per swapchain image because ImGui's texture binding for it is handed out before the command buffer is recorded.

Nothing consumes this class yet — it is wired up in Task 5. This task ends at a compiling, constructible unit so that Task 5's diff is about the frame structure and not about image creation.

**Files:**
- Create: `engine/renderer/vulkan/scene_render_targets.h`, `engine/renderer/vulkan/scene_render_targets.cpp`
- Modify: `engine/renderer/vulkan/command.h:27-60`
- Modify: `engine/renderer/CMakeLists.txt` (source list)

**Interfaces:**
- Consumes: `RenderTargetId`, `GetRenderTargetKind` from Task 2; `ChooseFormat`, `FormatFeatureQuery` from Task 2.
- Produces:
  - `VulkanCommandContext::kMaxFramesInFlight` as a public constant and `uint32_t VulkanCommandContext::GetCurrentFrame() const`.
  - `class SceneRenderTargets` with:
    - `SceneRenderTargets(VkPhysicalDevice, VkDevice, VkFormat ldrFormat, VkExtent2D, uint32_t swapchainImageCount)`
    - `VkFormat GetFormat(RenderTargetId) const`
    - `VkImage GetImage(RenderTargetId, uint32_t index) const`
    - `VkImageView GetView(RenderTargetId, uint32_t index) const`
    - `VkImageAspectFlags GetAspect(RenderTargetId) const`
    - `VkExtent2D GetExtent() const`
    - `uint32_t GetTransientCopyCount() const` and `uint32_t GetLdrCopyCount() const`
    - `uint32_t ResolveIndex(RenderTargetId, uint32_t imageIndex, uint32_t frameSlot) const`
    - `ImTextureID GetLdrTextureId(uint32_t imageIndex) const`
    - `ImTextureID GetHdrTextureId(uint32_t frameSlot) const`
    - `bool MatchesExtent(VkExtent2D) const`
    - `void Rebuild(VkExtent2D, uint32_t swapchainImageCount)` and `void ReleaseImages()`

- [ ] **Step 1: Expose the frame slot from the command context**

In `engine/renderer/vulkan/command.h`, move `static constexpr size_t kMaxFramesInFlight = 2;` from the private section to the top of the public section with this comment, and add the accessor:

```cpp
    // How many frames may be recorded before the oldest must complete. AcquireNextImage waits on
    // this slot's fence before acquiring, so any resource indexed by GetCurrentFrame() is free for
    // reuse once that call returns. SceneRenderTargets sizes its transient targets against this.
    static constexpr size_t kMaxFramesInFlight = 2;

    // The slot the frame being recorded belongs to. Advances in Present, so it is stable for the
    // whole of one AcquireNextImage / Submit / Present cycle.
    uint32_t GetCurrentFrame() const;
```

Implement it in `command.cpp`:

```cpp
uint32_t VulkanCommandContext::GetCurrentFrame() const
{
    return m_currentFrame;
}
```

- [ ] **Step 2: Write `scene_render_targets.h`**

```cpp
#pragma once

#include "common.h"
#include "render_target_layout.h"

#include <imgui.h>

#include <array>
#include <vector>

namespace me
{

// Owns every offscreen image the scene passes use, their views and their memory, plus the ImGui
// texture bindings needed to display them. Render passes and framebuffers deliberately live in
// the passes that use them, not here: a pass knows its own attachment set, and keeping them apart
// is what lets a resize rebuild images without touching a render pass the pipelines were built
// against.
//
// Two indexing schemes coexist, which is deliberate and is why the accessors are separate:
//
//   * SceneDepth and SceneHdr are transient. They are written and read inside one command buffer,
//     so kMaxFramesInFlight copies suffice, indexed by VulkanCommandContext::GetCurrentFrame().
//   * SceneLdr is sampled by ImGui, whose texture binding is handed out before the command buffer
//     is recorded, so it keeps one copy per swapchain image, indexed by the acquired image index.
//
// Passing a frame slot where an image index belongs would silently sample the wrong target, so
// GetImage / GetView assert the index against the count for the requested target's scheme.
class SceneRenderTargets
{
  public:
    SceneRenderTargets(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        VkFormat ldrFormat,
        VkExtent2D extent,
        uint32_t swapchainImageCount);
    ~SceneRenderTargets();

    SceneRenderTargets(const SceneRenderTargets&) = delete;
    SceneRenderTargets& operator=(const SceneRenderTargets&) = delete;

    VkFormat GetFormat(RenderTargetId target) const;
    VkImageAspectFlags GetAspect(RenderTargetId target) const;
    VkImage GetImage(RenderTargetId target, uint32_t index) const;
    VkImageView GetView(RenderTargetId target, uint32_t index) const;

    VkExtent2D GetExtent() const;
    bool MatchesExtent(VkExtent2D extent) const;

    // Copy counts for the two indexing schemes. Transient covers SceneDepth and SceneHdr.
    uint32_t GetTransientCopyCount() const;
    uint32_t GetLdrCopyCount() const;

    // Picks the index appropriate to a target's scheme. Every caller that has both an image index
    // and a frame slot in hand goes through this instead of restating the rule, so the rule lives
    // in exactly one place — the class that decided it.
    uint32_t ResolveIndex(RenderTargetId target, uint32_t imageIndex, uint32_t frameSlot) const;

    ImTextureID GetLdrTextureId(uint32_t imageIndex) const;
    // Only used while the tone mapping pass does not exist yet; see the phase one plan, Task 5.
    ImTextureID GetHdrTextureId(uint32_t frameSlot) const;

    // Both must run with the in-flight frames already waited on, and ReleaseImages must run while
    // ImGui's Vulkan backend is still alive because it removes ImGui texture bindings.
    void ReleaseImages();
    void Rebuild(VkExtent2D extent, uint32_t swapchainImageCount);

  private:
    struct TargetImage
    {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDescriptorSet imguiBinding = VK_NULL_HANDLE;
    };

    struct TargetDescription
    {
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkImageUsageFlags usage = 0;
        VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        bool bindToImGui = false;
        std::vector<TargetImage> images;
    };

    VkFormatFeatureFlags QueryFormatFeatures(VkFormat format) const;
    void SelectFormats(VkFormat ldrFormat);
    void CreateSampler();
    void CreateImages(uint32_t swapchainImageCount);
    void DestroyImages(std::array<TargetDescription, kRenderTargetCount>& targets) const;
    uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;
    void CreateImage(VkFormat format, VkImageUsageFlags usage, TargetImage& target) const;
    VkImageView CreateImageView(VkImage image, VkFormat format, VkImageAspectFlags aspect) const;
    TargetDescription& Describe(RenderTargetId target);
    const TargetDescription& Describe(RenderTargetId target) const;

    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkExtent2D m_extent{};
    VkSampler m_sampler = VK_NULL_HANDLE;
    uint32_t m_swapchainImageCount = 0;
    std::array<TargetDescription, kRenderTargetCount> m_targets{};
};
}
```

- [ ] **Step 3: Write `scene_render_targets.cpp`**

Move `FindMemoryType`, `CreateImage`, `CreateImageView`, `HasStencilComponent` and `ToImTextureId` from `scene_viewport.cpp` verbatim — they are correct and this task is not the place to change them. `CreateImage` loses its `width`/`height` parameters and reads `m_extent` instead, and writes into a `TargetImage` rather than out-parameters.

`SelectFormats` is the new logic and uses Task 2's `ChooseFormat`:

```cpp
void SceneRenderTargets::SelectFormats(VkFormat ldrFormat)
{
    const FormatFeatureQuery query = [this](VkFormat format)
    { return QueryFormatFeatures(format); };

    // The depth target must additionally be sampleable: the deferred lighting pass reconstructs
    // world position from it. That requirement is why this cannot stay VulkanSceneViewport's
    // attachment-only search.
    static constexpr std::array<VkFormat, 3> kDepthCandidates = {
        VK_FORMAT_D32_SFLOAT,
        VK_FORMAT_D32_SFLOAT_S8_UINT,
        VK_FORMAT_D24_UNORM_S8_UINT};
    static constexpr std::array<VkFormat, 1> kHdrCandidates = {
        VK_FORMAT_R16G16B16A16_SFLOAT};

    TargetDescription& depth = Describe(RenderTargetId::SceneDepth);
    depth.format = ChooseFormat(
        kDepthCandidates,
        VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,
        query);
    depth.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    depth.aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (HasStencilComponent(depth.format))
    {
        depth.aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
    }
    depth.bindToImGui = false;

    TargetDescription& hdr = Describe(RenderTargetId::SceneHdr);
    hdr.format = ChooseFormat(
        kHdrCandidates,
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,
        query);
    hdr.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    hdr.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    hdr.bindToImGui = true;

    TargetDescription& ldr = Describe(RenderTargetId::SceneLdr);
    ldr.format = ldrFormat;
    ldr.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ldr.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    ldr.bindToImGui = true;
}
```

`QueryFormatFeatures` wraps the Vulkan call:

```cpp
VkFormatFeatureFlags SceneRenderTargets::QueryFormatFeatures(VkFormat format) const
{
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(m_physicalDevice, format, &properties);
    return properties.optimalTilingFeatures;
}
```

`CreateImages` sizes each target by its scheme:

```cpp
void SceneRenderTargets::CreateImages(uint32_t swapchainImageCount)
{
    m_swapchainImageCount = swapchainImageCount;

    for (size_t index = 0; index < m_targets.size(); ++index)
    {
        const RenderTargetId target = static_cast<RenderTargetId>(index);
        TargetDescription& description = m_targets[index];
        const uint32_t copyCount =
            target == RenderTargetId::SceneLdr ? swapchainImageCount : GetTransientCopyCount();

        description.images.assign(copyCount, TargetImage{});
        for (TargetImage& image : description.images)
        {
            CreateImage(description.format, description.usage, image);
            image.view = CreateImageView(image.image, description.format, description.aspect);
            if (description.bindToImGui)
            {
                image.imguiBinding = ImGui_ImplVulkan_AddTexture(
                    m_sampler,
                    image.view,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
        }
    }
}
```

The translation unit needs `#include "command.h"` for `kMaxFramesInFlight`, `#include "format_support.h"` for `ChooseFormat`, and `#include "../imgui/imgui_impl_vulkan.h"` for the ImGui texture bindings, matching what `scene_viewport.cpp` included.

`GetTransientCopyCount` returns `static_cast<uint32_t>(VulkanCommandContext::kMaxFramesInFlight)`; `GetLdrCopyCount` returns `m_swapchainImageCount`. `ResolveIndex` returns `imageIndex` for `RenderTargetId::SceneLdr` and `frameSlot` for every other target. `GetImage` and `GetView` call `description.images.at(index)`, so a wrong index throws rather than silently reading a neighbouring frame's target. `CreateSampler` is `scene_viewport.cpp`'s verbatim. `ReleaseImages` mirrors `ReleaseFrames`: remove the ImGui binding, then destroy view, image and memory, then clear. `MatchesExtent` and the constructor's extent clamping are `scene_viewport.cpp`'s verbatim.

`Rebuild` is all-or-nothing, which `ReleaseImages`-then-`CreateImages` would not be — a failed allocation partway through would leave the renderer with no targets at all and nothing to fall back on. Build into locals first and only commit once every image exists:

```cpp
void SceneRenderTargets::Rebuild(VkExtent2D extent, uint32_t swapchainImageCount)
{
    const VkExtent2D clamped = {std::max(extent.width, 1u), std::max(extent.height, 1u)};

    // Snapshot what is live, build the replacement into the members, and put the snapshot back
    // if anything throws. A failure therefore leaves the previous, still-valid set in place.
    std::array<TargetDescription, kRenderTargetCount> previous = std::move(m_targets);
    const VkExtent2D previousExtent = m_extent;
    const uint32_t previousImageCount = m_swapchainImageCount;

    m_targets = previous;
    for (TargetDescription& description : m_targets)
    {
        description.images.clear();
    }
    m_extent = clamped;

    try
    {
        CreateImages(swapchainImageCount);
    }
    catch (...)
    {
        DestroyImages(m_targets);
        m_targets = std::move(previous);
        m_extent = previousExtent;
        m_swapchainImageCount = previousImageCount;
        throw;
    }

    DestroyImages(previous);
}
```

This needs `ReleaseImages` split into a public `ReleaseImages()` that destroys `m_targets` and clears them, and a private `void DestroyImages(std::array<TargetDescription, kRenderTargetCount>& targets) const` holding the actual destruction loop, so both the commit and the rollback path can use it. Declare `DestroyImages` alongside the other private helpers.

- [ ] **Step 4: Add the unit to the renderer source list**

In `engine/renderer/CMakeLists.txt`, add to `add_library(engine_renderer ...)`:

```cmake
    vulkan/scene_render_targets.cpp
    vulkan/scene_render_targets.h
```

- [ ] **Step 5: Build**

Run: `cmake --build --preset vs2026-x64-debug --parallel`
Expected: SUCCESS. Nothing constructs `SceneRenderTargets` yet, so the app's behavior is unchanged.

- [ ] **Step 6: Run the suite and format check**

Run: `ctest --test-dir out/build/vs2026-x64 -C Debug --output-on-failure`
Run: `out/build/vs2026-x64/app/Debug/miniengine_app.exe --backend vulkan --frames 60`
Run: `scripts/check-format.ps1`
Expected: all clean, image unchanged.

- [ ] **Step 7: Commit**

```bash
git add engine/renderer/vulkan/scene_render_targets.h engine/renderer/vulkan/scene_render_targets.cpp engine/renderer/vulkan/command.h engine/renderer/vulkan/command.cpp engine/renderer/CMakeLists.txt
git commit -m "feat(vulkan): add scene render target ownership"
```

---

### Task 5: Pass interface and the forward pass

The scene starts rendering into the HDR target through an `IScenePass`, and `VulkanSceneViewport` is deleted. Reinhard stays in `triangle.frag` for this task, so the HDR target holds already-tone-mapped values in `[0, 1]` and ImGui samples it directly.

That intermediate state is genuinely image-equivalent, not approximately: today the viewport attachment is `_SRGB`, so the hardware encodes on write and decodes on ImGui's read, round-tripping to the same linear value ImGui would read from a linear 16-bit float target holding the same number. Keeping the operator here for one task is what makes this task verifiable on its own.

**Files:**
- Create: `engine/renderer/vulkan/scene_pass.h`, `engine/renderer/vulkan/forward_pass.h`, `engine/renderer/vulkan/forward_pass.cpp`
- Delete: `engine/renderer/vulkan/scene_viewport.h`, `engine/renderer/vulkan/scene_viewport.cpp`
- Modify: `engine/renderer/vulkan/renderer.h`, `engine/renderer/vulkan/renderer.cpp`
- Modify: `engine/renderer/CMakeLists.txt` (source list)

**Interfaces:**
- Consumes: `SceneRenderTargets`, `VulkanCommandContext::GetCurrentFrame()` from Task 4; `RenderTargetLayoutTracker`, `RenderPassIo`, `TargetTransition` from Task 2; `VulkanPipelineSet`'s five-argument constructor from Task 3.
- Produces:
  - `struct ScenePassFrameContext { uint32_t imageIndex; uint32_t frameSlot; VkExtent2D extent; std::span<const VulkanDrawItem> drawItems; const VulkanPipelineSet* pipelines; VkDescriptorSet frameDescriptorSet; }`
  - `class IScenePass` with `virtual RenderPassIo Io() const`, `virtual void Record(VkCommandBuffer, const SceneRenderTargets&, const ScenePassFrameContext&) const`, `virtual void OnTargetsRebuilt(const SceneRenderTargets&)`.
  - `class VulkanForwardPass : public IScenePass` with `VulkanForwardPass(VkDevice, const SceneRenderTargets&)` and `VkRenderPass GetRenderPass() const`.
  - `void VulkanRenderer::RecordTransitions(VkCommandBuffer, const RenderPassIo&, const ScenePassFrameContext&)` and `void VulkanRenderer::RecordScenePasses(VkCommandBuffer, const ScenePassFrameContext&)`. Both non-const: they advance the layout tracker.

- [ ] **Step 1: Write `scene_pass.h`**

```cpp
#pragma once

#include "buffer.h"
#include "common.h"
#include "pipeline_set.h"
#include "render_target_layout.h"
#include "scene_render_targets.h"

#include <span>

namespace me
{

// Everything a pass may need about the frame being recorded. Passes hold no per-frame state of
// their own, so a pass object is reusable across frames and owns only its render pass and
// framebuffers.
struct ScenePassFrameContext
{
    uint32_t imageIndex = 0;
    uint32_t frameSlot = 0;
    VkExtent2D extent{};
    std::span<const VulkanDrawItem> drawItems;
    const VulkanPipelineSet* pipelines = nullptr;
    VkDescriptorSet frameDescriptorSet = VK_NULL_HANDLE;
};

// One pass in the scene frame. Io() is the declaration the layout tracker turns into barriers;
// Record assumes those barriers have already been issued.
//
// Every implementation's render pass must declare initialLayout == finalLayout for each
// attachment, so that the tracker stays the single authority on layouts. See the note in
// render_target_layout.h.
class IScenePass
{
  public:
    virtual ~IScenePass() = default;

    virtual RenderPassIo Io() const = 0;
    virtual void Record(
        VkCommandBuffer commandBuffer,
        const SceneRenderTargets& targets,
        const ScenePassFrameContext& frame) const = 0;

    // Called after SceneRenderTargets::Rebuild, so the pass can recreate framebuffers against the
    // new views. Formats never change here, so render passes and pipelines survive.
    virtual void OnTargetsRebuilt(const SceneRenderTargets& targets) = 0;
};
}
```

- [ ] **Step 2: Write `forward_pass.h`**

```cpp
#pragma once

#include "scene_pass.h"

#include <vector>

namespace me
{

// The material pass: every draw item, into the HDR color target with depth. This is
// VulkanSceneViewport's old render pass with two changes — the color attachment is the HDR target
// rather than an sRGB image, and both attachments declare initialLayout == finalLayout so the
// pass performs no implicit transition.
//
// Framebuffers are indexed by frame slot because both attachments are transient targets.
class VulkanForwardPass : public IScenePass
{
  public:
    VulkanForwardPass(VkDevice device, const SceneRenderTargets& targets);
    ~VulkanForwardPass() override;

    VulkanForwardPass(const VulkanForwardPass&) = delete;
    VulkanForwardPass& operator=(const VulkanForwardPass&) = delete;

    RenderPassIo Io() const override;
    void Record(
        VkCommandBuffer commandBuffer,
        const SceneRenderTargets& targets,
        const ScenePassFrameContext& frame) const override;
    void OnTargetsRebuilt(const SceneRenderTargets& targets) override;

    // The material pipelines are built against this. It depends only on the attachment formats,
    // so it survives a resize and a swapchain recreate.
    VkRenderPass GetRenderPass() const;

  private:
    void CreateRenderPass(const SceneRenderTargets& targets);
    void CreateFramebuffers(const SceneRenderTargets& targets);
    void DestroyFramebuffers();

    VkDevice m_device = VK_NULL_HANDLE;
    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> m_framebuffers;
};
}
```

- [ ] **Step 3: Write `forward_pass.cpp`**

`Io()` returns the declaration:

```cpp
RenderPassIo VulkanForwardPass::Io() const
{
    static constexpr std::array<RenderTargetId, 2> kWrites = {
        RenderTargetId::SceneHdr,
        RenderTargetId::SceneDepth};

    RenderPassIo io{};
    io.writes = kWrites;
    return io;
}
```

`CreateRenderPass` is `VulkanSceneViewport::CreateRenderPass`'s body with these differences, and no others:

- The color attachment format is `targets.GetFormat(RenderTargetId::SceneHdr)`.
- The color attachment's `initialLayout` and `finalLayout` are both `VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL`.
- The depth attachment's `initialLayout` and `finalLayout` are both `VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL`.
- Both `VkSubpassDependency` entries are deleted. `dependencyCount` becomes `0` and `pDependencies` `nullptr`: the explicit barriers the tracker produces now carry that ordering, and leaving the dependencies in would duplicate it against layouts that no longer change.

`loadOp` stays `CLEAR` for both, `storeOp` stays `STORE` for color and `DONT_CARE` for depth.

`CreateFramebuffers` is `CreateFrameResources`'s framebuffer half, sized by `targets.GetTransientCopyCount()` and attaching `targets.GetView(RenderTargetId::SceneHdr, slot)` and `targets.GetView(RenderTargetId::SceneDepth, slot)`.

`Record` is `VulkanRenderer::RecordSceneLayer`'s body verbatim from Task 3, with `m_sceneViewportLayer->GetRenderPass()` becoming `m_renderPass`, `GetFramebuffer(imageIndex)` becoming `m_framebuffers[frame.frameSlot]`, the extent coming from `frame.extent`, `m_graphicsPipelines` becoming `frame.pipelines`, and the set 0 bind using `frame.frameDescriptorSet`. The clear values, the dynamic viewport and scissor, the pipeline rebind guard, the push constants and the draw call are unchanged.

`OnTargetsRebuilt` calls `DestroyFramebuffers` then `CreateFramebuffers`. The destructor calls `DestroyFramebuffers` then destroys the render pass.

- [ ] **Step 4: Replace the viewport layer in the renderer**

In `renderer.h`: delete the `scene_viewport.h` include and the `m_sceneViewportLayer` member; add includes for `forward_pass.h` and `scene_render_targets.h`; add members

```cpp
    std::unique_ptr<SceneRenderTargets> m_sceneTargets;
    std::unique_ptr<VulkanForwardPass> m_forwardPass;
    RenderTargetLayoutTracker m_layoutTracker;
    std::vector<IScenePass*> m_scenePasses;
```

Replace the `RecordSceneLayer` declaration with

```cpp
    void RecordTransitions(
        VkCommandBuffer commandBuffer,
        const RenderPassIo& io,
        const ScenePassFrameContext& frame);
    void RecordScenePasses(VkCommandBuffer commandBuffer, const ScenePassFrameContext& frame);
```

and rename `SyncSceneViewportLayer` to `SyncSceneTargets`.

- [ ] **Step 5: Write the pass loop**

Add both functions to `renderer.cpp`, and declare both in `renderer.h`. `RecordScenePasses` is the function every later pass is added to; `RecordTransitions` is separate because the ImGui pass also needs a transition and is not an `IScenePass` — it writes the swapchain, not a scene target.

```cpp
void VulkanRenderer::RecordTransitions(
    VkCommandBuffer commandBuffer,
    const RenderPassIo& io,
    const ScenePassFrameContext& frame)
{
    for (const TargetTransition& transition : m_layoutTracker.Transition(io))
    {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = transition.oldLayout;
        barrier.newLayout = transition.newLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = m_sceneTargets->GetImage(
            transition.target,
            m_sceneTargets->ResolveIndex(transition.target, frame.imageIndex, frame.frameSlot));
        barrier.subresourceRange.aspectMask = m_sceneTargets->GetAspect(transition.target);
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcAccessMask = AccessMaskForLayout(transition.oldLayout);
        barrier.dstAccessMask = AccessMaskForLayout(transition.newLayout);

        vkCmdPipelineBarrier(
            commandBuffer,
            StageMaskForLayout(transition.oldLayout),
            StageMaskForLayout(transition.newLayout),
            0,
            0,
            nullptr,
            0,
            nullptr,
            1,
            &barrier);
    }
}

void VulkanRenderer::RecordScenePasses(VkCommandBuffer commandBuffer, const ScenePassFrameContext& frame)
{
    for (IScenePass* pass : m_scenePasses)
    {
        RecordTransitions(commandBuffer, pass->Io(), frame);
        pass->Record(commandBuffer, *m_sceneTargets, frame);
    }
}
```

Add these two helpers in the anonymous namespace at the top of `renderer.cpp`:

```cpp
VkAccessFlags AccessMaskForLayout(VkImageLayout layout)
{
    switch (layout)
    {
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        return VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        return VK_ACCESS_SHADER_READ_BIT;
    default:
        return 0;
    }
}

VkPipelineStageFlags StageMaskForLayout(VkImageLayout layout)
{
    switch (layout)
    {
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        return VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        return VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        return VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    case VK_IMAGE_LAYOUT_UNDEFINED:
        return VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    default:
        return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    }
}
```

- [ ] **Step 6: Rewire creation, teardown and resize**

In `CreateSwapchainResources`, replace the `m_sceneViewportLayer` block with the equivalent for the new units: construct or rebuild `m_sceneTargets` with `m_swapchain->GetImageFormat()` as the LDR format, then construct `m_forwardPass` if it does not exist or call `m_forwardPass->OnTargetsRebuilt(*m_sceneTargets)` if it does, then `m_layoutTracker.Reset()` and populate `m_scenePasses` with `{m_forwardPass.get()}`. Keep the existing conditional that resets `m_graphicsPipelines` when the color format changed, now keyed on the forward pass being recreated.

In `DestroySwapchainResources`, replace `m_sceneViewportLayer->ReleaseFrames()` with `m_sceneTargets->ReleaseImages()` and add `m_layoutTracker.Reset()` — a released image is undefined again.

In `SyncSceneTargets`, replace `m_sceneViewportLayer->Resize(...)` with `m_sceneTargets->Rebuild(...)` followed by `m_forwardPass->OnTargetsRebuilt(*m_sceneTargets)` and `m_layoutTracker.Reset()`.

In `EnsureGraphicsPipelines`, pass `m_forwardPass->GetRenderPass()` instead of `m_sceneViewportLayer->GetRenderPass()`.

In the destructor, reset `m_forwardPass` and `m_sceneTargets` where `m_sceneViewportLayer` was reset, in that order.

- [ ] **Step 7: Rewire `DrawFrame`**

Replace `m_sceneViewportLayer->GetExtent()` with `m_sceneTargets->GetExtent()` at both call sites. Pass `m_sceneTargets->GetHdrTextureId(m_commandContext->GetCurrentFrame())` to `DrawEditorUi` in place of `GetTextureId(imageIndex)`. Build the context and record:

```cpp
    ScenePassFrameContext frame{};
    frame.imageIndex = imageIndex;
    frame.frameSlot = m_commandContext->GetCurrentFrame();
    frame.extent = m_sceneTargets->GetExtent();
    frame.drawItems = drawItems;
    frame.pipelines = m_graphicsPipelines.get();
    frame.frameDescriptorSet = m_uniformBuffer->GetFrameDescriptorSet(imageIndex);

    m_commandContext->RecordCommandBuffer(imageIndex, [&](VkCommandBuffer commandBuffer)
                                          {
                                              RecordScenePasses(commandBuffer, frame);
                                              RecordEditorLayer(commandBuffer, imageIndex);
                                          });
```

The HDR target must be in `SHADER_READ_ONLY_OPTIMAL` when the ImGui pass samples it, and no `IScenePass` reads it yet, so declare that read explicitly inside the recorder, after `RecordScenePasses` and before `RecordEditorLayer`:

```cpp
    // Until the tone mapping pass exists, ImGui samples the HDR target directly. Task 6 replaces
    // this with the tone mapping pass's own read declaration.
    static constexpr std::array<RenderTargetId, 1> kImGuiReads = {RenderTargetId::SceneHdr};
    RenderPassIo imguiIo{};
    imguiIo.reads = kImGuiReads;
    RecordTransitions(commandBuffer, imguiIo, frame);
```

- [ ] **Step 8: Delete the viewport layer**

```bash
git rm engine/renderer/vulkan/scene_viewport.h engine/renderer/vulkan/scene_viewport.cpp
```

Remove both from the `engine_renderer` source list and add `vulkan/forward_pass.cpp`, `vulkan/forward_pass.h`, `vulkan/scene_pass.h`.

- [ ] **Step 9: Build and run**

Run: `cmake --build --preset vs2026-x64-debug --parallel`
Expected: SUCCESS.

Run: `out/build/vs2026-x64/app/Debug/miniengine_app.exe --backend vulkan --frames 60`
Expected: exit code 0, zero validation messages. Missing or wrong barriers surface here as synchronization validation errors, which is the main thing this task can get wrong.

- [ ] **Step 10: Confirm the image is unchanged**

Launch the app on the default scene. The viewport must be indistinguishable from before this task. Drag the viewport panel edge to resize, including down to a very small size and back: no artifacts, no validation errors. Then resize the OS window to force a swapchain recreate and confirm the same.

- [ ] **Step 11: Run the suite and format check**

Run: `ctest --test-dir out/build/vs2026-x64 -C Debug --output-on-failure`
Run: `scripts/check-format.ps1`
Expected: both clean.

- [ ] **Step 12: Commit**

```bash
git add -A engine/renderer shaders
git commit -m "refactor(vulkan): record the scene through a pass list into an HDR target"
```

---

### Task 6: Tone mapping pass

Reinhard leaves `triangle.frag` for a pass of its own, the HDR target starts holding genuinely linear radiance, and ImGui goes back to sampling an sRGB image.

**Files:**
- Create: `shaders/vulkan/fullscreen.vert`, `shaders/vulkan/tonemap.frag`
- Create: `engine/renderer/vulkan/tonemap_pass.h`, `engine/renderer/vulkan/tonemap_pass.cpp`
- Modify: `shaders/vulkan/triangle.frag` (remove the Reinhard operator)
- Modify: `engine/renderer/vulkan/renderer.h`, `engine/renderer/vulkan/renderer.cpp`
- Modify: `engine/renderer/CMakeLists.txt` (shader list and source list)

**Interfaces:**
- Consumes: `IScenePass`, `ScenePassFrameContext` from Task 5; `SceneRenderTargets` from Task 4.
- Produces: `class VulkanTonemapPass : public IScenePass` with `VulkanTonemapPass(VkDevice, VkPipelineCache, const SceneRenderTargets&)`. It takes no `VkPhysicalDevice`: it allocates no device memory, only a descriptor pool and a sampler.

- [ ] **Step 1: Write `fullscreen.vert`**

```glsl
#version 450

// A single triangle covering the whole viewport, generated from the vertex index so the pass
// needs no vertex buffer and no vertex input state. Vertices land at (-1,-1), (3,-1), (-1,3),
// which clips to exactly the [-1,1] square.
layout(location = 0) out vec2 fragTexCoord;

void main()
{
    const vec2 position = vec2(
        (gl_VertexIndex == 1) ? 3.0 : -1.0,
        (gl_VertexIndex == 2) ? 3.0 : -1.0);

    // Texture coordinates with the origin at the top left, matching the project's UV convention
    // (see the Vulkan UV note in README): NDC y = -1 is the image's top row.
    fragTexCoord = position * 0.5 + 0.5;
    gl_Position = vec4(position, 0.0, 1.0);
}
```

- [ ] **Step 2: Write `tonemap.frag`**

```glsl
#version 450

layout(set = 0, binding = 0) uniform sampler2D hdrTexture;

layout(location = 0) in vec2 fragTexCoord;

layout(location = 0) out vec4 outColor;

void main()
{
    vec3 color = texture(hdrTexture, fragTexCoord).rgb;

    // Reinhard, moved verbatim out of triangle.frag. The expression is unchanged so that this
    // pass produces the same values the forward shader used to produce.
    color = color / (color + vec3(1.0));

    // This pass is the sole writer of the LDR target and knows coverage is total, so it writes
    // alpha explicitly rather than relying on the RGB-only color write mask the material
    // pipelines use to keep the attachment's clear alpha intact. ImGui composites the viewport
    // image over the editor, so this alpha must be 1.0.
    outColor = vec4(color, 1.0);
}
```

- [ ] **Step 3: Add both to the shader list**

In `engine/renderer/CMakeLists.txt`:

```cmake
set(MINIENGINE_SHADER_SOURCES
    triangle.vert
    triangle.frag
    fullscreen.vert
    tonemap.frag
)
```

- [ ] **Step 4: Remove Reinhard from `triangle.frag`**

Replace the tail of `main()`:

```glsl
    vec3 color = ambient + directAccum + emissive;

    // Tone mapping happens in the tonemap pass, which is the only consumer of this target. This
    // shader writes linear radiance.
    outColor = vec4(color, albedo.a);
```

Delete the `// Reinhard tonemapping` comment and the `color = color / (color + vec3(1.0));` line.

- [ ] **Step 5: Write `tonemap_pass.h`**

```cpp
#pragma once

#include "scene_pass.h"

#include <vector>

namespace me
{

// Resolves the HDR target into the sRGB image ImGui samples. Owns its render pass, framebuffers,
// pipeline and the descriptor sets holding the HDR sampler.
//
// Its framebuffers are indexed by swapchain image because the LDR target is, while its descriptor
// sets are indexed by frame slot because the HDR target is. That split is the whole reason
// SceneRenderTargets exposes two copy counts.
class VulkanTonemapPass : public IScenePass
{
  public:
    VulkanTonemapPass(
        VkDevice device,
        VkPipelineCache pipelineCache,
        const SceneRenderTargets& targets);
    ~VulkanTonemapPass() override;

    VulkanTonemapPass(const VulkanTonemapPass&) = delete;
    VulkanTonemapPass& operator=(const VulkanTonemapPass&) = delete;

    RenderPassIo Io() const override;
    void Record(
        VkCommandBuffer commandBuffer,
        const SceneRenderTargets& targets,
        const ScenePassFrameContext& frame) const override;
    void OnTargetsRebuilt(const SceneRenderTargets& targets) override;

  private:
    void CreateDescriptorSetLayout();
    void CreateSampler();
    void CreateRenderPass(const SceneRenderTargets& targets);
    void CreatePipeline(VkPipelineCache pipelineCache);
    void CreateDescriptorSets(const SceneRenderTargets& targets);
    void CreateFramebuffers(const SceneRenderTargets& targets);
    void DestroyFramebuffers();

    VkDevice m_device = VK_NULL_HANDLE;
    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> m_descriptorSets;
    std::vector<VkFramebuffer> m_framebuffers;
};
}
```

- [ ] **Step 6: Write `tonemap_pass.cpp`**

`Io()` declares one read and one write:

```cpp
RenderPassIo VulkanTonemapPass::Io() const
{
    static constexpr std::array<RenderTargetId, 1> kReads = {RenderTargetId::SceneHdr};
    static constexpr std::array<RenderTargetId, 1> kWrites = {RenderTargetId::SceneLdr};

    RenderPassIo io{};
    io.reads = kReads;
    io.writes = kWrites;
    return io;
}
```

`CreateDescriptorSetLayout` creates one binding: binding 0, `VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER`, count 1, stage fragment. `CreateSampler` uses `VK_FILTER_NEAREST` with `CLAMP_TO_EDGE` on all axes and `maxLod` 0 — the pass samples one texel per pixel at matching resolution, so linear filtering would only blur.

`CreateRenderPass` declares one color attachment: format `targets.GetFormat(RenderTargetId::SceneLdr)`, `VK_SAMPLE_COUNT_1_BIT`, `loadOp` `VK_ATTACHMENT_LOAD_OP_DONT_CARE` (the full-screen triangle covers every pixel and writes all four channels), `storeOp` `VK_ATTACHMENT_STORE_OP_STORE`, stencil ops `DONT_CARE`, and `initialLayout` and `finalLayout` both `VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL`. One subpass, one color attachment reference in `VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL`, no depth attachment, `dependencyCount` `0`.

`CreatePipeline` builds one graphics pipeline from `fullscreen.vert.spv` and `tonemap.frag.spv`, loaded through `VulkanShaderModule` with `EnginePaths::ShaderRoot()` exactly as `VulkanPipelineSet` does. Its state:

- `pVertexInputState` with all counts zero — the vertex shader reads no attributes.
- Topology `VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST`.
- Viewport and scissor counts 1, both dynamic, set at record time. This is what lets a viewport resize leave the pipeline alone.
- Rasterizer: `VK_POLYGON_MODE_FILL`, `lineWidth` 1, `VK_CULL_MODE_NONE`. The generated triangle has no meaningful winding, so culling it by face would be a coin flip.
- `pDepthStencilState` with depth test, depth write and stencil test all `VK_FALSE`.
- One color blend attachment, `blendEnable` `VK_FALSE`, `colorWriteMask` all four of R, G, B and A — unlike the material pipelines, this pass writes alpha deliberately.
- No push constant range, one set layout: `m_setLayout`.

`CreateDescriptorSets` allocates `targets.GetTransientCopyCount()` sets from a pool sized to match and writes binding 0 of set `slot` with `targets.GetView(RenderTargetId::SceneHdr, slot)`, `m_sampler` and `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL`. It is called again from `OnTargetsRebuilt` because the views change; reset the pool with `vkResetDescriptorPool` first rather than leaking sets.

`CreateFramebuffers` creates `targets.GetLdrCopyCount()` framebuffers, each attaching `targets.GetView(RenderTargetId::SceneLdr, imageIndex)`.

`Record` begins the render pass on `m_framebuffers[frame.imageIndex]`, sets the dynamic viewport and scissor from `frame.extent` the way `VulkanForwardPass` does, binds `m_pipeline` and `m_descriptorSets[frame.frameSlot]` at set 0, then:

```cpp
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);
```

`OnTargetsRebuilt` rebuilds framebuffers and descriptor sets. The destructor destroys pipeline, pipeline layout, descriptor pool, set layout, sampler, framebuffers and render pass.

- [ ] **Step 7: Add the pass to the renderer**

Add `std::unique_ptr<VulkanTonemapPass> m_tonemapPass;` to `renderer.h` and include `tonemap_pass.h`. In `CreateSwapchainResources`, construct it beside `m_forwardPass` — it needs `m_pipelineCache` — or call `OnTargetsRebuilt` when it already exists, and make `m_scenePasses` `{m_forwardPass.get(), m_tonemapPass.get()}`. Add it to `SyncSceneTargets`'s rebuild sequence and to the destructor's reset order before `m_sceneTargets`.

- [ ] **Step 8: Point ImGui back at the LDR target**

In `DrawFrame`, pass `m_sceneTargets->GetLdrTextureId(imageIndex)` to `DrawEditorUi`. Delete the temporary HDR transition block Task 5 added after the pass loop: the tone mapping pass's own `Io()` now puts the HDR target into `SHADER_READ_ONLY_OPTIMAL`, and its `finalLayout` leaves the LDR target in `COLOR_ATTACHMENT_OPTIMAL`.

The LDR target must be in `SHADER_READ_ONLY_OPTIMAL` for the ImGui pass, so keep one transition after the pass loop, now naming the LDR target:

```cpp
    // ImGui samples the tone mapped image in the editor pass, which is not an IScenePass because
    // it writes the swapchain rather than a scene target.
    static constexpr std::array<RenderTargetId, 1> kImGuiReads = {RenderTargetId::SceneLdr};
    RenderPassIo imguiIo{};
    imguiIo.reads = kImGuiReads;
    RecordTransitions(commandBuffer, imguiIo, frame);
```

- [ ] **Step 9: Remove the now-unused HDR texture accessor**

Delete `SceneRenderTargets::GetHdrTextureId` and set the HDR target's `bindToImGui` to `false` in `SelectFormats`. It existed only for Task 5's intermediate state, and an ImGui binding on a target nothing displays is a descriptor leak waiting to confuse the next reader.

- [ ] **Step 10: Build and run**

Run: `cmake --build --preset vs2026-x64-debug --parallel`
Expected: SUCCESS.

Run: `out/build/vs2026-x64/app/Debug/miniengine_app.exe --backend vulkan --frames 60`
Expected: exit code 0, zero validation messages.

- [ ] **Step 11: Confirm the image is unchanged**

Launch the app on the default scene. The viewport must be indistinguishable from before phase one began. Check specifically:

- Overall brightness and contrast, which is what a misplaced tone mapping operator changes first.
- The background color outside all geometry, still the `{0.08, 0.1, 0.16}` clear.
- The viewport's alpha: no editor background showing through the 3D view.
- All five light types, on a scene containing each.
- Blend materials compositing back to front as the camera moves, and Mask cutout silhouettes.
- Viewport resize, including to a degenerate size and back, then an OS window resize.

- [ ] **Step 12: Run the full verification**

Run: `ctest --test-dir out/build/vs2026-x64 -C Debug --output-on-failure`
Run: `scripts/check-format.ps1`
Run: `git diff --check`
Expected: all clean. Review the changed-file scope against this plan's File Structure table.

- [ ] **Step 13: Commit**

```bash
git add -A engine/renderer shaders
git commit -m "feat(vulkan): resolve the HDR target in a tone mapping pass"
```

---

## Phase One Exit Criteria

- The viewport image is indistinguishable from the pre-phase-one image on the default scene.
- The frame records two `IScenePass` entries plus the ImGui pass, and every layout change in it is an explicit barrier produced by `RenderTargetLayoutTracker`.
- `miniengine.scene_pass` passes, and so does every pre-existing test.
- `out/build/vs2026-x64/app/Debug/miniengine_app.exe --backend vulkan --frames 60` emits zero validation messages in a Debug build.
- `VulkanSceneViewport` no longer exists.
- Adding a pass means writing one `IScenePass` and appending to `m_scenePasses`.
