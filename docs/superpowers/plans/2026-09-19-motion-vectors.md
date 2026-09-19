# Motion Vectors Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Write a per-pixel motion vector for camera and object motion into a new G-buffer target and show it as a Graphics Debug view, leaving the shaded image pixel-identical.

**Architecture:** The geometry pass gains an `R16G16_SFLOAT` attachment written from the current and previous clip positions. The previous view-projection rides in the camera block; each draw's previous model matrix lives in a set 0 storage buffer indexed by `firstInstance`. A pure `MotionHistory` unit keyed by (entity, submesh ordinal) supplies both.

**Tech Stack:** C++20, Vulkan 1.3, GLSL via glslc, GLM, EnTT, Dear ImGui, CMake + vcpkg.

**Spec:** [docs/superpowers/specs/2026-09-19-motion-vectors-design.md](../specs/2026-09-19-motion-vectors-design.md)

## Global Constraints

- C++20, Allman braces, `namespace me`. `scripts/check-format.ps1` must pass before every commit.
- Build: `cmake --build --preset vs2026-x64-debug --parallel`. Tests: `ctest --test-dir out/build/vs2026-x64 -C Debug --output-on-failure`.
- `ObjectPushConstants` stays exactly 128 bytes; its assertions in `engine/renderer/material.h` may not be relaxed.
- `CameraUniformData` members are all mat4/vec4; its size and offset assertions are updated, never removed.
- Every render pass keeps `initialLayout == finalLayout`; the layout tracker issues every barrier.
- Debug builds run with validation layers: zero validation messages is a gate on every task that changes recording.
- The shaded image must stay pixel-identical in both the deferred and the forward-only order.
- Commits go directly to `main`, message style `type(scope): summary`, ending with `Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>`. `miniengine.settings.json` and `docs/superpowers/plans/2026-09-13-gbuffer-phase2-deferred-shading.md` are the user's local changes and are never staged.

## File Structure

| File | Change | Responsibility |
|---|---|---|
| `engine/renderer/motion_history.h/.cpp` | Create | Keyed previous-frame matrices (pure, no Vulkan) |
| `tests/motion_history_tests.cpp` | Create | Unit tests for `MotionHistory` |
| `tests/CMakeLists.txt` | Modify | `miniengine.motion_history` target |
| `engine/renderer/CMakeLists.txt` | Modify | Add `motion_history.*` to `engine_render_core` |
| `engine/renderer/vulkan/render_target_layout.h` | Modify | `GBufferVelocity` id |
| `engine/renderer/vulkan/scene_render_targets.cpp` | Modify | Velocity format and usage |
| `engine/renderer/vulkan/geometry_pass.h` | Modify | Fifth color attachment |
| `engine/renderer/vulkan/pipeline_set.h/.cpp` | Modify | Allow five color attachments |
| `engine/renderer/vulkan/uniform_buffer.h/.cpp` | Modify | `prevViewProj`; previous model storage buffer at set 0 binding 2 |
| `engine/renderer/vulkan/command.h`, `material_draw.cpp` | Modify | `motionSlot` passed as `firstInstance` |
| `engine/renderer/vulkan/renderer.h/.cpp` | Modify | Motion keys, `MotionHistory`, reset on resize |
| `engine/renderer/vulkan/gbuffer_inputs.h`, `tonemap_pass.cpp` | Modify | Velocity in set 2 and in the tone mapping reads |
| `engine/renderer/render_types.h` | Modify | `GBufferDebugView::MotionVectors` |
| `engine/editor/ui/editor_misc_panels.cpp` | Modify | Combo entry |
| `shaders/vulkan/scene_common.glsl`, `triangle.vert`, `gbuffer.frag`, `gbuffer_inputs.glsl`, `tonemap.frag` | Modify | Shader side |
| `tests/scene_pass_tests.cpp` | Modify | Velocity is a color target |
| `README.md` | Modify | Record the capability |

---

### Task 1: `MotionHistory`

**Files:**
- Create: `engine/renderer/motion_history.h`, `engine/renderer/motion_history.cpp`, `tests/motion_history_tests.cpp`
- Modify: `engine/renderer/CMakeLists.txt` (engine_render_core sources), `tests/CMakeLists.txt`

**Interfaces:**
- Produces: `me::MotionKey { uint32_t entity; uint32_t submeshOrdinal; }`, `me::MotionFrame { glm::mat4 previousViewProjection; std::vector<glm::mat4> previousModels; }`, `me::MotionHistory::Advance(const glm::mat4&, std::span<const MotionKey>, std::span<const glm::mat4>) -> MotionFrame`, `me::MotionHistory::Reset()`.

- [ ] **Step 1: Write the failing test** — `tests/motion_history_tests.cpp`:

```cpp
#include <engine/renderer/motion_history.h>

#include <glm/ext/matrix_transform.hpp>

#include <array>
#include <iostream>
#include <stdexcept>

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

glm::mat4 Translation(float x)
{
    return glm::translate(glm::mat4(1.0f), glm::vec3(x, 0.0f, 0.0f));
}

void FirstFrameReportsNoMotion()
{
    MotionHistory history;
    const std::array<MotionKey, 1> keys = {MotionKey{7, 0}};
    const std::array<glm::mat4, 1> models = {Translation(1.0f)};

    const MotionFrame frame = history.Advance(Translation(5.0f), keys, models);

    Require(frame.previousViewProjection == Translation(5.0f), "the first frame must report the current camera");
    Require(frame.previousModels.size() == 1, "one previous model per draw");
    Require(frame.previousModels[0] == Translation(1.0f), "the first frame must report the current model");
}

void SecondFrameReportsLastFrame()
{
    MotionHistory history;
    const std::array<MotionKey, 1> keys = {MotionKey{7, 0}};
    const std::array<glm::mat4, 1> first = {Translation(1.0f)};
    const std::array<glm::mat4, 1> second = {Translation(2.0f)};

    history.Advance(Translation(5.0f), keys, first);
    const MotionFrame frame = history.Advance(Translation(6.0f), keys, second);

    Require(frame.previousViewProjection == Translation(5.0f), "the camera must report last frame");
    Require(frame.previousModels[0] == Translation(1.0f), "a known key must report last frame's model");
}

void NewKeyReportsNoMotion()
{
    MotionHistory history;
    const std::array<MotionKey, 1> firstKeys = {MotionKey{7, 0}};
    const std::array<glm::mat4, 1> firstModels = {Translation(1.0f)};
    const std::array<MotionKey, 2> secondKeys = {MotionKey{7, 0}, MotionKey{8, 0}};
    const std::array<glm::mat4, 2> secondModels = {Translation(2.0f), Translation(9.0f)};

    history.Advance(glm::mat4(1.0f), firstKeys, firstModels);
    const MotionFrame frame = history.Advance(glm::mat4(1.0f), secondKeys, secondModels);

    Require(frame.previousModels[0] == Translation(1.0f), "the known key keeps its history");
    Require(frame.previousModels[1] == Translation(9.0f), "a new key must report its current model");
}

void ReorderedKeysKeepTheirOwnHistory()
{
    MotionHistory history;
    const std::array<MotionKey, 2> firstKeys = {MotionKey{1, 0}, MotionKey{1, 1}};
    const std::array<glm::mat4, 2> firstModels = {Translation(1.0f), Translation(2.0f)};
    const std::array<MotionKey, 2> secondKeys = {MotionKey{1, 1}, MotionKey{1, 0}};
    const std::array<glm::mat4, 2> secondModels = {Translation(20.0f), Translation(10.0f)};

    history.Advance(glm::mat4(1.0f), firstKeys, firstModels);
    const MotionFrame frame = history.Advance(glm::mat4(1.0f), secondKeys, secondModels);

    Require(frame.previousModels[0] == Translation(2.0f), "slot 0 now holds submesh 1 and must get its history");
    Require(frame.previousModels[1] == Translation(1.0f), "slot 1 now holds submesh 0 and must get its history");
}

void RemovedKeyIsForgotten()
{
    MotionHistory history;
    const std::array<MotionKey, 1> keys = {MotionKey{3, 0}};
    const std::array<glm::mat4, 1> first = {Translation(1.0f)};
    const std::array<glm::mat4, 1> third = {Translation(3.0f)};

    history.Advance(glm::mat4(1.0f), keys, first);
    history.Advance(glm::mat4(1.0f), {}, {});
    const MotionFrame frame = history.Advance(glm::mat4(1.0f), keys, third);

    Require(frame.previousModels[0] == Translation(3.0f), "a key absent for a frame must lose its history");
}

void ResetReportsNoMotion()
{
    MotionHistory history;
    const std::array<MotionKey, 1> keys = {MotionKey{7, 0}};
    const std::array<glm::mat4, 1> first = {Translation(1.0f)};
    const std::array<glm::mat4, 1> second = {Translation(2.0f)};

    history.Advance(Translation(5.0f), keys, first);
    history.Reset();
    const MotionFrame frame = history.Advance(Translation(6.0f), keys, second);

    Require(frame.previousViewProjection == Translation(6.0f), "after Reset the camera must report no motion");
    Require(frame.previousModels[0] == Translation(2.0f), "after Reset every draw must report no motion");
}

void DuplicateKeysAreRejected()
{
    MotionHistory history;
    const std::array<MotionKey, 2> keys = {MotionKey{7, 0}, MotionKey{7, 0}};
    const std::array<glm::mat4, 2> models = {Translation(1.0f), Translation(2.0f)};

    bool threw = false;
    try
    {
        history.Advance(glm::mat4(1.0f), keys, models);
    }
    catch (const std::invalid_argument&)
    {
        threw = true;
    }
    Require(threw, "duplicate keys must throw std::invalid_argument");
}

void MismatchedSpansAreRejected()
{
    MotionHistory history;
    const std::array<MotionKey, 2> keys = {MotionKey{7, 0}, MotionKey{7, 1}};
    const std::array<glm::mat4, 1> models = {Translation(1.0f)};

    bool threw = false;
    try
    {
        history.Advance(glm::mat4(1.0f), keys, models);
    }
    catch (const std::invalid_argument&)
    {
        threw = true;
    }
    Require(threw, "keys and models of different lengths must throw std::invalid_argument");
}
}

int main()
{
    try
    {
        FirstFrameReportsNoMotion();
        SecondFrameReportsLastFrame();
        NewKeyReportsNoMotion();
        ReorderedKeysKeepTheirOwnHistory();
        RemovedKeyIsForgotten();
        ResetReportsNoMotion();
        DuplicateKeysAreRejected();
        MismatchedSpansAreRejected();
    }
    catch (const std::exception& error)
    {
        std::cerr << "motion history tests failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "motion history tests passed\n";
    return 0;
}
```

Add the target to `tests/CMakeLists.txt`, after the shadow cascades block, following its pattern:

```cmake
add_executable(miniengine_motion_history_tests
    motion_history_tests.cpp
)
miniengine_group_target_sources(miniengine_motion_history_tests)

target_link_libraries(miniengine_motion_history_tests
    PRIVATE
        engine_render_core
)

add_test(
    NAME miniengine.motion_history
    COMMAND miniengine_motion_history_tests
)

if(WIN32)
    add_custom_command(TARGET miniengine_motion_history_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:miniengine_motion_history_tests>
            $<TARGET_FILE_DIR:miniengine_motion_history_tests>
        COMMAND_EXPAND_LISTS
    )
endif()

set_target_properties(miniengine_motion_history_tests PROPERTIES FOLDER "Tests")
```

- [ ] **Step 2: Run to verify it fails.** `cmake --preset vs2026-x64` then build: fails to compile, `engine/renderer/motion_history.h` not found.

- [ ] **Step 3: Implement.** `engine/renderer/motion_history.h`:

```cpp
#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace me
{

// Identifies one draw across frames: the entity it belongs to and its position among that entity's
// submeshes. A slot index would not do, because a content reload may reorder the draw list.
struct MotionKey
{
    uint32_t entity = 0;
    uint32_t submeshOrdinal = 0;
};

struct MotionFrame
{
    glm::mat4 previousViewProjection{1.0f};
    // Parallel to the models passed to Advance.
    std::vector<glm::mat4> previousModels;
};

// Remembers last frame's view-projection and each draw's model matrix, for motion vectors. A draw
// with no history, and every draw after Reset, reports its current matrix: zero motion rather than
// a guess.
class MotionHistory
{
  public:
    // Returns last frame's matrices for this frame's draws, then remembers this frame's. Throws
    // std::invalid_argument when keys and models differ in length or a key repeats, since a
    // duplicate would silently give one draw the other's history.
    MotionFrame Advance(
        const glm::mat4& viewProjection,
        std::span<const MotionKey> keys,
        std::span<const glm::mat4> models);

    void Reset();

  private:
    static uint64_t Pack(const MotionKey& key);

    bool m_hasHistory = false;
    glm::mat4 m_viewProjection{1.0f};
    std::unordered_map<uint64_t, glm::mat4> m_models;
};
}
```

`engine/renderer/motion_history.cpp`:

```cpp
#include "motion_history.h"

#include <stdexcept>

namespace me
{

MotionFrame MotionHistory::Advance(
    const glm::mat4& viewProjection,
    std::span<const MotionKey> keys,
    std::span<const glm::mat4> models)
{
    if (keys.size() != models.size())
    {
        throw std::invalid_argument("MotionHistory::Advance needs one model per key");
    }

    MotionFrame frame{};
    frame.previousViewProjection = m_hasHistory ? m_viewProjection : viewProjection;
    frame.previousModels.reserve(models.size());

    std::unordered_map<uint64_t, glm::mat4> current;
    current.reserve(keys.size());
    for (size_t index = 0; index < keys.size(); ++index)
    {
        const uint64_t packed = Pack(keys[index]);
        if (!current.emplace(packed, models[index]).second)
        {
            throw std::invalid_argument("MotionHistory::Advance was given the same key twice");
        }

        const auto previous = m_hasHistory ? m_models.find(packed) : m_models.end();
        frame.previousModels.push_back(previous != m_models.end() ? previous->second : models[index]);
    }

    m_models = std::move(current);
    m_viewProjection = viewProjection;
    m_hasHistory = true;
    return frame;
}

void MotionHistory::Reset()
{
    m_hasHistory = false;
    m_models.clear();
}

uint64_t MotionHistory::Pack(const MotionKey& key)
{
    return (static_cast<uint64_t>(key.entity) << 32) | key.submeshOrdinal;
}
}
```

Add `motion_history.cpp` and `motion_history.h` to `engine_render_core` in `engine/renderer/CMakeLists.txt`, alphabetically after `material_pipeline.h`.

- [ ] **Step 4: Run to verify it passes.** Build, then `ctest --test-dir out/build/vs2026-x64 -C Debug -R motion_history --output-on-failure`: PASS. Full ctest: all pass.

- [ ] **Step 5: Commit** (after `scripts/check-format.ps1`):

```bash
git add engine/renderer/motion_history.h engine/renderer/motion_history.cpp engine/renderer/CMakeLists.txt tests/motion_history_tests.cpp tests/CMakeLists.txt
git commit -m "feat(renderer): remember last frame's matrices for motion vectors"
```

---

### Task 2: Velocity target, camera motion and the debug view

Object motion is not yet wired: `triangle.vert` uses the current model matrix for both positions, so this task's vectors are exactly the camera motion. Task 3 replaces that one line.

**Files:**
- Modify: `engine/renderer/vulkan/render_target_layout.h`, `scene_render_targets.cpp`, `geometry_pass.h`, `pipeline_set.h`, `pipeline_set.cpp`, `uniform_buffer.h`, `uniform_buffer.cpp`, `gbuffer_inputs.h`, `tonemap_pass.cpp`, `renderer.h`, `renderer.cpp`; `engine/renderer/render_types.h`; `engine/editor/ui/editor_misc_panels.cpp`; `shaders/vulkan/scene_common.glsl`, `triangle.vert`, `gbuffer.frag`, `gbuffer_inputs.glsl`, `tonemap.frag`; `tests/scene_pass_tests.cpp`

**Interfaces:**
- Consumes: `MotionHistory`, `MotionKey`, `MotionFrame` (Task 1).
- Produces: `RenderTargetId::GBufferVelocity`; `CameraUniformData::prevViewProj`; `VulkanUniformBuffer::Update(..., const ShadowUniformData& shadow, const glm::mat4& prevViewProj)`; `GBufferDebugView::MotionVectors = 6`; `VulkanRenderer::m_motionHistory`.

- [ ] **Step 1: Failing test.** In `tests/scene_pass_tests.cpp`, `GBufferTargetsAreColorTargets`, grow the array to five and add `RenderTargetId::GBufferVelocity`. Build: fails, `GBufferVelocity` is not a member.

- [ ] **Step 2: Target.** In `render_target_layout.h`, add `GBufferVelocity` after `GBufferEmissive`, and reword the enum comment's phase three sentence to: "`GBufferVelocity` (motion vectors) follows the four phase two targets; GB4 (entity id) is appended in phase three, which is why Count stays last." In `scene_render_targets.cpp`, after the emissive `describeGBufferTarget` call:

```cpp
    // Motion vectors: current UV minus previous UV, written by the geometry pass. Both of the
    // features ChooseFormat asks for are mandatory for this format, so it has no fallback.
    static constexpr std::array<VkFormat, 1> kVelocityCandidates = {VK_FORMAT_R16G16_SFLOAT};
    describeGBufferTarget(RenderTargetId::GBufferVelocity, "G-buffer velocity", kVelocityCandidates);
```

Build and run `ctest -R scene_pass`: PASS.

- [ ] **Step 3: Geometry pass.** In `geometry_pass.h`, insert `RenderTargetId::GBufferVelocity` after `GBufferEmissive` in `kAttachments` (size 6), set `kColorAttachmentCount = 5`, and update the comments ("the five colors, then depth"; "locations 0-4"). In `pipeline_set.h`, `kMaxMaterialColorAttachments = 5`; in `pipeline_set.cpp` change the throw message to "1 to 5 color attachments". The clear value for index 4 is already zero-initialised, which is the "no motion" background.

- [ ] **Step 4: Camera block.** `uniform_buffer.h`: after `invViewProj` add

```cpp
    // Last frame's proj * view, for motion vectors. Equal to this frame's when there is no history
    // (first frame, or after the scene targets were rebuilt).
    glm::mat4 prevViewProj{1.0f};
```

grow the size assertion by `+ 64`, and add

```cpp
static_assert(
    offsetof(CameraUniformData, prevViewProj) ==
        2 * 64 + 2 * 16 + kMaxSceneLights * 80 + 16 + kShadowCascadeCount * 64 + 3 * 16 + 64,
    "prevViewProj must follow invViewProj with no padding");
```

Add `const glm::mat4& prevViewProj` as the last parameter of `Update` (header and definition) and set `data.prevViewProj = prevViewProj;`. `scene_common.glsl`: after `invViewProj` add `mat4 prevViewProj; // last frame's proj * view, for motion vectors`.

- [ ] **Step 5: Shaders.** `triangle.vert`: add outputs

```glsl
// Clip positions of this vertex this frame and last frame, divided per fragment by gbuffer.frag
// for motion vectors. triangle.frag does not declare them.
layout(location = 5) out vec4 fragCurrClip;
layout(location = 6) out vec4 fragPrevClip;
```

and after `gl_Position`:

```glsl
    fragCurrClip = gl_Position;
    fragPrevClip = ubo.prevViewProj * (drawData.model * vec4(inPosition, 1.0));
```

`gbuffer.frag` (it needs no camera data, so no new include): add inputs `layout(location = 5) in vec4 fragCurrClip; layout(location = 6) in vec4 fragPrevClip;`, output `layout(location = 4) out vec4 outVelocity; // R16G16_SFLOAT: current uv - previous uv`, and at the end of `main`:

```glsl
    // uv = ndc * 0.5 + 0.5 with the Y flip inside the projection, so half the NDC difference is
    // the motion in UV units. A consumer finds the previous position at uv - velocity.
    vec2 currNdc = fragCurrClip.xy / fragCurrClip.w;
    vec2 prevNdc = fragPrevClip.xy / fragPrevClip.w;
    outVelocity = vec4((currNdc - prevNdc) * 0.5, 0.0, 0.0);
```

Update the output table comment above the outputs to list location 4.

- [ ] **Step 6: Renderer.** `renderer.h`: `#include <engine/renderer/motion_history.h>`, member `MotionHistory m_motionHistory;` with the comment "Last frame's matrices for motion vectors. Reset whenever the scene targets are rebuilt, because a new extent is a new projection." In `renderer.cpp`:
  - `SyncSceneTargets`: after `m_layoutTracker.Reset();` add `m_motionHistory.Reset();`.
  - `CreateSwapchainResources`: after its `m_layoutTracker.Reset();` add `m_motionHistory.Reset();`.
  - `DrawFrame`, before `m_uniformBuffer->Update(`:

```cpp
    const glm::mat4 viewProjection = State().viewportMatrices.renderProjection * State().viewportMatrices.view;
    const MotionFrame motion = m_motionHistory.Advance(viewProjection, {}, {});
```

  and pass `motion.previousViewProjection` as the new last argument of `Update`. (Task 3 passes real keys.)

- [ ] **Step 7: Debug view.** `render_types.h`: `MotionVectors = 6` in `GBufferDebugView`. `gbuffer_inputs.h`: append `RenderTargetId::GBufferVelocity` to `kInputs` (size 6). `gbuffer_inputs.glsl`: `layout(set = 2, binding = 5) uniform sampler2D gbufferVelocity;`. `tonemap_pass.cpp` `Io`: append `RenderTargetId::GBufferVelocity` to `kReads` (size 7) and update its comment ("all six G-buffer inputs"). `tonemap.frag`: add `const uint GBUFFER_VIEW_MOTION_VECTORS = 6u;` and before the final `else`:

```glsl
    else if (constants.gbufferView == GBUFFER_VIEW_MOTION_VECTORS)
    {
        // Mid-grey is still. Red grows with rightward motion and green with downward motion,
        // saturating at 16 pixels per frame. Background pixels hold no vector and read grey.
        vec2 pixels = texture(gbufferVelocity, fragTexCoord).rg * vec2(textureSize(gbufferVelocity, 0));
        color = vec3(clamp(0.5 + pixels / 32.0, 0.0, 1.0), 0.5);
    }
```

  `editor_misc_panels.cpp`: grow `kGBufferViewNames` to 7 with `"G-buffer: motion vectors"`.

- [ ] **Step 8: Verify.** Build Debug, full ctest passes, `scripts/check-format.ps1` passes. Run `out\build\vs2026-x64\app\Debug\miniengine_app.exe --backend vulkan --frames 300` and require zero `[error]`/`[warning]` lines from the validation layer.

- [ ] **Step 9: Commit.**

```bash
git commit -m "feat(vulkan): write camera motion vectors into the G-buffer"
```

---

### Task 3: Object motion

**Files:**
- Modify: `engine/renderer/vulkan/uniform_buffer.h`, `uniform_buffer.cpp`, `command.h`, `material_draw.cpp`, `renderer.h`, `renderer.cpp`; `shaders/vulkan/triangle.vert`

**Interfaces:**
- Consumes: Task 2's `Update(..., prevViewProj)`, `MotionHistory`.
- Produces: `VulkanUniformBuffer(…, TextureDescriptorBinding shadowMap, uint32_t motionSlotCount)`; `Update(..., const glm::mat4& prevViewProj, std::span<const glm::mat4> prevModels)`; `VulkanDrawItem::motionSlot`; `RenderSubmesh::motionKey`.

- [ ] **Step 1: Storage buffer.** `uniform_buffer.h`: constructor gains `uint32_t motionSlotCount` as its last parameter; `Update` gains `std::span<const glm::mat4> prevModels` as its last parameter; members:

```cpp
    // Set 0 binding 2: each draw's previous model matrix, indexed by its firstInstance. One
    // host-visible buffer per swapchain image, sized to the draw list this object was built for.
    std::vector<VkBuffer> m_motionBuffers;
    std::vector<VkDeviceMemory> m_motionMemories;
    std::vector<void*> m_mappedMotionBuffers;
    uint32_t m_motionSlotCount = 0;
```

  Update the set 0 comment on `VulkanFrameDescriptorSetLayout` to mention binding 2. In `uniform_buffer.cpp`:
  - Constructor stores `m_motionSlotCount = std::max(motionSlotCount, 1u)` before `CreateBuffers`.
  - `VulkanFrameDescriptorSetLayout`: three bindings; binding 2 is `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER`, `VK_SHADER_STAGE_VERTEX_BIT`.
  - `CreateBuffers`: after the uniform buffers, create `imageCount` storage buffers of `sizeof(glm::mat4) * m_motionSlotCount` bytes, `VK_BUFFER_USAGE_STORAGE_BUFFER_BIT`, host-visible and coherent, mapped persistently, each initialised to identity matrices.
  - Destructor: destroy them (freeing mapped memory unmaps it).
  - `CreateDescriptorPool`: third pool size `{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, imageCount}`.
  - `CreateDescriptorSets`: third frame write, binding 2, `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER`, range `VK_WHOLE_SIZE`.
  - `Update`: throw `std::runtime_error("More previous model matrices than motion slots")` when `prevModels.size() > m_motionSlotCount`, else `memcpy` them into `m_mappedMotionBuffers[imageIndex]`. The throw is deliberate: a draw indexing past the buffer would read out of bounds on the GPU with no robustness feature enabled.

- [ ] **Step 2: Slot per draw.** `command.h` `VulkanDrawItem`: add `uint32_t motionSlot = 0;` with the comment "Index into the previous model matrix buffer, passed to the draw as firstInstance." `material_draw.cpp`: `vkCmdDrawIndexed(commandBuffer, drawItem.indexCount, 1, 0, 0, drawItem.motionSlot);`.

- [ ] **Step 3: Keys and history.** `renderer.h` `RenderSubmesh`: add `MotionKey motionKey;` ("Assigned in ApplyRenderContent: the entity and this submesh's position among that entity's submeshes."). `BuildDrawItems` takes `std::span<const glm::mat4> models` as a second parameter (parallel to `m_renderSubmeshes`) instead of calling `GetModelMatrix` itself. In `renderer.cpp`:
  - `ApplyRenderContent`, before building the uniform buffer:

```cpp
    std::unordered_map<uint32_t, uint32_t> nextOrdinal;
    for (RenderSubmesh& renderSubmesh : newRenderSubmeshes)
    {
        const uint32_t entity = static_cast<uint32_t>(entt::to_integral(renderSubmesh.entity));
        renderSubmesh.motionKey = MotionKey{entity, nextOrdinal[entity]++};
    }
```

    and pass `static_cast<uint32_t>(newRenderSubmeshes.size())` as `motionSlotCount`. `CreateDescriptorResources` passes `static_cast<uint32_t>(m_renderSubmeshes.size())`.
  - `DrawFrame` replaces Task 2's two lines with:

```cpp
    std::vector<glm::mat4> models;
    std::vector<MotionKey> motionKeys;
    models.reserve(m_renderSubmeshes.size());
    motionKeys.reserve(m_renderSubmeshes.size());
    for (const RenderSubmesh& renderSubmesh : m_renderSubmeshes)
    {
        models.push_back(State().rendererWorld.GetModelMatrix(renderSubmesh.entity));
        motionKeys.push_back(renderSubmesh.motionKey);
    }
    const glm::mat4 viewProjection = State().viewportMatrices.renderProjection * State().viewportMatrices.view;
    const MotionFrame motion = m_motionHistory.Advance(viewProjection, motionKeys, models);
```

    passes `motion.previousModels` as `Update`'s last argument, and calls `BuildDrawItems(imageIndex, models)`.
  - `BuildDrawItems` iterates by index, uses `models[submeshIndex]`, and sets `motionSlot = static_cast<uint32_t>(submeshIndex)` on each item.

- [ ] **Step 4: Shader.** `triangle.vert`:

```glsl
// Each draw's model matrix from last frame, indexed by the draw's firstInstance (see
// VulkanDrawItem::motionSlot). Push constants are full, so it cannot ride with the current one.
layout(set = 0, binding = 2) readonly buffer PreviousModelBuffer
{
    mat4 previousModels[];
}
previousModelData;
```

  and `fragPrevClip = ubo.prevViewProj * (previousModelData.previousModels[gl_InstanceIndex] * vec4(inPosition, 1.0));`.

- [ ] **Step 5: Verify.** Build Debug, full ctest, format check, validation run as in Task 2 Step 8 with zero messages, plus the same run with `--model assets\NewSponza_Main_glTF_003\NewSponza_Main_glTF_003.gltf` (406 submeshes exercise the buffer size).

- [ ] **Step 6: Commit.**

```bash
git commit -m "feat(vulkan): carry each draw's previous model matrix for object motion"
```

---

### Task 4: Verification and record

**Files:**
- Modify: `README.md` (section 7, 当前能力边界)

- [ ] **Step 1: Local capture harness (never committed).** On a local branch `verify/motion-vectors` from `main`, port the environment-driven parts of `engine/renderer/vulkan/vbao_spike.*` from `spike/vbao` (extent, camera pose and velocity, per-submesh translation, load gate, PNG capture and quit), minus every AO field. Apply the same harness to the commit before Task 2 (`git worktree` or a second local branch) for the baseline.
- [ ] **Step 2: Pixel equivalence.** Same Sponza pose, same frame, shaded view in both orders, before and after: the PNGs must be byte-identical.
- [ ] **Step 3: Object motion.** Camera still; translate the blue test box by `d = 0.01` m per frame along world Y. Sample the velocity capture over the box (decode sRGB to linear, `pixels = (value - 0.5) * 32`) and compare with `d * |proj[1][1]| * height / (2 * depth)`; the rest of the frame must read 0 within quantisation.
- [ ] **Step 4: Camera motion.** Translate the camera; the field must be smooth and larger on nearer surfaces.
- [ ] **Step 5: Record.** README section 7: state that the G-buffer carries camera and object motion vectors (Blend surfaces and background excluded) with a Graphics Debug view, and that nothing consumes them yet. Commit `docs(readme): record motion vectors`. Delete the verification branch.
