# Combined Transform and Rotation Gizmo Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace separate translate and rotate modes with one Unity-style combined gizmo and make `R` toggle between that combined mode and scale.

**Architecture:** Put the combined operation constant, mode transition, and drag-stable snap-family policy in a focused `engine_logic` unit. Keep YAML compatibility in `editor_scene.cpp`, then wire the tested policy into the existing single `ImGuizmo::Manipulate` viewport call and the scene inspector.

**Tech Stack:** C++20, ImGuizmo 1.10, Dear ImGui, GLM, yaml-cpp, CMake/CTest, Visual Studio 2026 x64 Debug.

## Global Constraints

- Combined mode is exactly `ImGuizmo::TRANSLATE | ImGuizmo::ROTATE`; do not add scale bits to it.
- Combined mode must expose X/Y/Z translation axes, XY/YZ/XZ planar handles, and rotation rings through one `ImGuizmo::Manipulate` call.
- `R` is the only gizmo-mode shortcut and toggles Combined/Scale; `W` and `E` must not change gizmo mode.
- Ignore `R` while `ImGuizmo::IsUsing()` reports an active drag.
- Preserve World/Local mode and the existing translation, rotation, and scale snap values.
- Latch the snap family at drag start and keep it unchanged until the drag ends.
- Point and Ambient lights remain translation-only.
- Read legacy YAML `translate` and `rotate` as Combined; write only `combined` or `scale`.
- Do not change transform decomposition, pivot offsets, entity picking, light wireframes, camera controls, ImGuizmo source, or dependency versions.
- Do not stage or modify the user's existing `imgui.ini` or untracked `.claude/` tree.
- Automated verification does not replace the user's GUI acceptance.

---

## File Structure

- Create `engine/logic/gizmo_settings.h`: public gizmo settings, combined operation constant, mode toggle, snap-family state, and snap-value interface.
- Create `engine/logic/gizmo_settings.cpp`: pure operation and snap-family policy implementation.
- Modify `engine/logic/editor_world.h`: consume `GizmoSettings` from the focused header instead of defining it inline.
- Modify `engine/logic/editor_scene.cpp`: canonical YAML parsing and emission.
- Modify `engine/logic/CMakeLists.txt`: compile the new logic unit.
- Create `tests/gizmo_settings_tests.cpp`: focused operation, snap, and YAML compatibility tests.
- Modify `tests/CMakeLists.txt`: register `miniengine_gizmo_settings_tests`.
- Modify `engine/editor/editor_ui.h`: own the per-viewport `GizmoDragSnapState`.
- Modify `engine/editor/ui/editor_viewport_panel.cpp`: `R` toggle, combined manipulation, snap-family latching, and help text.
- Modify `engine/editor/ui/editor_scene_panel.cpp`: expose only Combined and Scale buttons.

---

### Task 1: Gizmo Operation and Snap Policy

**Files:**
- Create: `engine/logic/gizmo_settings.h`
- Create: `engine/logic/gizmo_settings.cpp`
- Modify: `engine/logic/editor_world.h:1-29`
- Modify: `engine/logic/CMakeLists.txt:1-9`
- Create: `tests/gizmo_settings_tests.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `ImGuizmo::OPERATION`, `WorldUnits` snap defaults, and `glm::vec3`.
- Produces: `kCombinedGizmoOperation`, `GizmoSettings`, `ToggleGizmoOperation(ImGuizmo::OPERATION)`, `GizmoSnapFamily`, `GizmoDragSnapState`, and `BuildGizmoSnapValues(const GizmoSettings&, GizmoSnapFamily)`.

- [ ] **Step 1: Add the failing policy test and CTest target**

Create `tests/gizmo_settings_tests.cpp`:

```cpp
#include <gizmo_settings.h>

#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace
{
void Require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

bool NearlyEqual(float lhs, float rhs)
{
    return std::abs(lhs - rhs) < 0.0001f;
}
}

int main()
{
    try
    {
        const int combinedBits = static_cast<int>(kCombinedGizmoOperation);
        Require(
            (combinedBits & static_cast<int>(ImGuizmo::TRANSLATE)) ==
                static_cast<int>(ImGuizmo::TRANSLATE),
            "combined operation is missing translation handles"
        );
        Require(
            (combinedBits & static_cast<int>(ImGuizmo::ROTATE)) ==
                static_cast<int>(ImGuizmo::ROTATE),
            "combined operation is missing rotation handles"
        );
        Require(
            (combinedBits & static_cast<int>(ImGuizmo::SCALE)) == 0,
            "combined operation unexpectedly contains scale handles"
        );

        GizmoSettings settings{};
        Require(settings.operation == kCombinedGizmoOperation, "combined mode is not the default");
        Require(
            ToggleGizmoOperation(kCombinedGizmoOperation) == ImGuizmo::SCALE,
            "R toggle did not enter scale"
        );
        Require(
            ToggleGizmoOperation(ImGuizmo::SCALE) == kCombinedGizmoOperation,
            "R toggle did not return to combined"
        );

        GizmoDragSnapState dragState;
        dragState.PrepareForManipulate(kCombinedGizmoOperation, false, true);
        Require(dragState.Family() == GizmoSnapFamily::Rotation, "rotation hover did not select rotation snap");
        dragState.FinishManipulate(true);
        dragState.PrepareForManipulate(kCombinedGizmoOperation, true, false);
        Require(dragState.Family() == GizmoSnapFamily::Rotation, "snap family changed during rotation drag");
        dragState.FinishManipulate(false);
        Require(dragState.Family() == GizmoSnapFamily::None, "snap family did not clear after drag");

        dragState.PrepareForManipulate(kCombinedGizmoOperation, false, false);
        Require(dragState.Family() == GizmoSnapFamily::Translation, "axis or plane did not select translation snap");
        dragState.FinishManipulate(false);
        dragState.PrepareForManipulate(ImGuizmo::SCALE, false, false);
        Require(dragState.Family() == GizmoSnapFamily::Scale, "scale mode did not select scale snap");

        settings.translationSnap = { 1.0f, 2.0f, 3.0f };
        settings.rotationSnap = 15.0f;
        settings.scaleSnap = { 0.1f, 0.2f, 0.3f };
        const std::array<float, 3> translation = BuildGizmoSnapValues(settings, GizmoSnapFamily::Translation);
        const std::array<float, 3> rotation = BuildGizmoSnapValues(settings, GizmoSnapFamily::Rotation);
        const std::array<float, 3> scale = BuildGizmoSnapValues(settings, GizmoSnapFamily::Scale);
        Require(
            NearlyEqual(translation[0], 1.0f) &&
                NearlyEqual(translation[1], 2.0f) &&
                NearlyEqual(translation[2], 3.0f),
            "translation snap values changed"
        );
        Require(
            NearlyEqual(rotation[0], 15.0f) &&
                NearlyEqual(rotation[1], 0.0f) &&
                NearlyEqual(rotation[2], 0.0f),
            "rotation snap values changed"
        );
        Require(
            NearlyEqual(scale[0], 0.1f) &&
                NearlyEqual(scale[1], 0.2f) &&
                NearlyEqual(scale[2], 0.3f),
            "scale snap values changed"
        );

        std::cout << "gizmo settings tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "gizmo settings tests failed: " << error.what() << '\n';
        return 1;
    }
}
```

Append this target to `tests/CMakeLists.txt`:

```cmake
add_executable(miniengine_gizmo_settings_tests
    gizmo_settings_tests.cpp
)
miniengine_group_target_sources(miniengine_gizmo_settings_tests)

target_link_libraries(miniengine_gizmo_settings_tests
    PRIVATE
        engine_logic
        imguizmo::imguizmo
)

add_test(
    NAME miniengine.gizmo_settings
    COMMAND miniengine_gizmo_settings_tests
)

if(WIN32)
    add_custom_command(TARGET miniengine_gizmo_settings_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:miniengine_gizmo_settings_tests>
            $<TARGET_FILE_DIR:miniengine_gizmo_settings_tests>
        COMMAND_EXPAND_LISTS
    )
endif()

set_target_properties(miniengine_gizmo_settings_tests PROPERTIES FOLDER "Tests")
```

- [ ] **Step 2: Build the new test and verify it fails for the missing policy header**

Run:

```powershell
cmake --build --preset vs2026-x64-debug --target miniengine_gizmo_settings_tests
```

Expected: FAIL while compiling `gizmo_settings_tests.cpp` because `gizmo_settings.h` does not exist.

- [ ] **Step 3: Implement the minimal focused gizmo policy**

Create `engine/logic/gizmo_settings.h`:

```cpp
#pragma once

#include <world_units.h>

#include <ImGuizmo.h>
#include <glm/glm.hpp>

#include <array>

inline constexpr ImGuizmo::OPERATION kCombinedGizmoOperation =
    static_cast<ImGuizmo::OPERATION>(
        static_cast<int>(ImGuizmo::TRANSLATE) |
        static_cast<int>(ImGuizmo::ROTATE)
    );

enum class GizmoSnapFamily
{
    None,
    Translation,
    Rotation,
    Scale
};

struct GizmoSettings
{
    ImGuizmo::OPERATION operation = kCombinedGizmoOperation;
    ImGuizmo::MODE mode = ImGuizmo::WORLD;
    bool useSnap = false;
    glm::vec3 translationSnap = WorldUnits::kDefaultTranslationSnapMeters;
    float rotationSnap = WorldUnits::kDefaultRotationSnapDegrees;
    glm::vec3 scaleSnap = WorldUnits::kDefaultScaleSnap;
};

ImGuizmo::OPERATION ToggleGizmoOperation(ImGuizmo::OPERATION operation);
std::array<float, 3> BuildGizmoSnapValues(const GizmoSettings& settings, GizmoSnapFamily family);

class GizmoDragSnapState
{
public:
    void PrepareForManipulate(
        ImGuizmo::OPERATION operation,
        bool gizmoIsUsing,
        bool rotationHandleHovered
    );
    void FinishManipulate(bool gizmoIsUsing);
    GizmoSnapFamily Family() const { return m_family; }

private:
    GizmoSnapFamily m_family = GizmoSnapFamily::None;
};
```

Create `engine/logic/gizmo_settings.cpp`:

```cpp
#include "gizmo_settings.h"

namespace
{
bool ContainsOperation(ImGuizmo::OPERATION operation, ImGuizmo::OPERATION expected)
{
    const int operationBits = static_cast<int>(operation);
    const int expectedBits = static_cast<int>(expected);
    return (operationBits & expectedBits) == expectedBits;
}
}

ImGuizmo::OPERATION ToggleGizmoOperation(ImGuizmo::OPERATION operation)
{
    return operation == ImGuizmo::SCALE ? kCombinedGizmoOperation : ImGuizmo::SCALE;
}

std::array<float, 3> BuildGizmoSnapValues(const GizmoSettings& settings, GizmoSnapFamily family)
{
    switch (family)
    {
    case GizmoSnapFamily::Rotation:
        return { settings.rotationSnap, 0.0f, 0.0f };
    case GizmoSnapFamily::Scale:
        return { settings.scaleSnap.x, settings.scaleSnap.y, settings.scaleSnap.z };
    case GizmoSnapFamily::None:
    case GizmoSnapFamily::Translation:
    default:
        return {
            settings.translationSnap.x,
            settings.translationSnap.y,
            settings.translationSnap.z
        };
    }
}

void GizmoDragSnapState::PrepareForManipulate(
    ImGuizmo::OPERATION operation,
    bool gizmoIsUsing,
    bool rotationHandleHovered
)
{
    if (gizmoIsUsing && m_family != GizmoSnapFamily::None)
    {
        return;
    }

    if (operation == ImGuizmo::SCALE)
    {
        m_family = GizmoSnapFamily::Scale;
        return;
    }

    const bool hasRotation = ContainsOperation(operation, ImGuizmo::ROTATE);
    const bool hasTranslation = ContainsOperation(operation, ImGuizmo::TRANSLATE);
    if (hasRotation && (!hasTranslation || rotationHandleHovered))
    {
        m_family = GizmoSnapFamily::Rotation;
        return;
    }

    m_family = GizmoSnapFamily::Translation;
}

void GizmoDragSnapState::FinishManipulate(bool gizmoIsUsing)
{
    if (!gizmoIsUsing)
    {
        m_family = GizmoSnapFamily::None;
    }
}
```

Add the new files to `engine/logic/CMakeLists.txt`:

```cmake
add_library(engine_logic
    editor_scene.cpp
    editor_scene.h
    editor_world.h
    gizmo_settings.cpp
    gizmo_settings.h
    logic_layer.h
)
```

In `engine/logic/editor_world.h`, add:

```cpp
#include <gizmo_settings.h>
```

Then remove its current `#include <world_units.h>`, `#include <ImGuizmo.h>`, and
the inline `struct GizmoSettings` definition. Keep the remaining declarations
unchanged.

- [ ] **Step 4: Build and run the focused policy test**

Run:

```powershell
cmake --build --preset vs2026-x64-debug --target miniengine_gizmo_settings_tests
ctest --test-dir .\out\build\vs2026-x64 -C Debug -R miniengine.gizmo_settings --output-on-failure
```

Expected: build succeeds and CTest reports `100% tests passed, 0 tests failed out of 1`.

- [ ] **Step 5: Commit the policy unit**

```powershell
git add -- engine/logic/gizmo_settings.h engine/logic/gizmo_settings.cpp engine/logic/editor_world.h engine/logic/CMakeLists.txt tests/gizmo_settings_tests.cpp tests/CMakeLists.txt
git diff --cached --check
git commit -m "feat: define combined gizmo policy"
```

Expected: one commit containing only the focused logic unit, its CMake wiring,
and its tests.

---

### Task 2: Canonical YAML Compatibility

**Files:**
- Modify: `tests/gizmo_settings_tests.cpp`
- Modify: `engine/logic/editor_scene.cpp:60-94`

**Interfaces:**
- Consumes: `kCombinedGizmoOperation`, `GizmoSettings`, `LoadEditorSceneDataFromFile`, and `SaveEditorSceneDataToFile`.
- Produces: canonical `operation: combined`/`operation: scale` output and legacy `translate`/`rotate` upgrade behavior.

- [ ] **Step 1: Extend the focused test with failing YAML compatibility cases**

Add these includes to `tests/gizmo_settings_tests.cpp`:

```cpp
#include <editor_world.h>

#include <filesystem>
#include <fstream>
#include <string>
```

Add these helpers inside its anonymous namespace:

```cpp
ImGuizmo::OPERATION LoadStoredOperation(const std::string& storedValue)
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / ("miniengine_gizmo_" + storedValue + ".yaml");
    {
        std::ofstream file(path);
        file
            << "scene:\n"
            << "  version: 3\n"
            << "  selected_entity: 0\n"
            << "entities: []\n"
            << "lights: []\n"
            << "editor:\n"
            << "  gizmo:\n"
            << "    operation: " << storedValue << "\n";
    }

    const SerializedSceneData loaded = LoadEditorSceneDataFromFile(path.string());
    std::error_code removeError;
    std::filesystem::remove(path, removeError);
    return loaded.gizmo.operation;
}

std::string SaveStoredOperation(ImGuizmo::OPERATION operation, const char* suffix)
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / (std::string("miniengine_gizmo_save_") + suffix + ".yaml");
    SerializedSceneData scene{};
    scene.gizmo.operation = operation;
    SaveEditorSceneDataToFile(scene, path.string());

    std::ifstream file(path);
    const std::string yaml((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::error_code removeError;
    std::filesystem::remove(path, removeError);
    return yaml;
}
```

Insert these assertions after the snap-value assertions in `main()`:

```cpp
Require(
    LoadStoredOperation("combined") == kCombinedGizmoOperation,
    "combined yaml did not load as combined"
);
Require(
    LoadStoredOperation("translate") == kCombinedGizmoOperation,
    "legacy translate yaml did not upgrade to combined"
);
Require(
    LoadStoredOperation("rotate") == kCombinedGizmoOperation,
    "legacy rotate yaml did not upgrade to combined"
);
Require(
    LoadStoredOperation("scale") == ImGuizmo::SCALE,
    "scale yaml did not remain scale"
);
Require(
    LoadStoredOperation("unexpected") == kCombinedGizmoOperation,
    "unknown yaml did not use the combined fallback"
);
Require(
    SaveStoredOperation(kCombinedGizmoOperation, "combined").find("operation: combined") != std::string::npos,
    "combined mode did not serialize canonically"
);
Require(
    SaveStoredOperation(ImGuizmo::SCALE, "scale").find("operation: scale") != std::string::npos,
    "scale mode did not serialize canonically"
);
```

- [ ] **Step 2: Run the focused test and verify legacy/new YAML cases fail**

Run:

```powershell
cmake --build --preset vs2026-x64-debug --target miniengine_gizmo_settings_tests
ctest --test-dir .\out\build\vs2026-x64 -C Debug -R miniengine.gizmo_settings --output-on-failure
```

Expected: the executable builds, then the test fails first with
`combined yaml did not load as combined`.

- [ ] **Step 3: Implement canonical operation parsing and emission**

Replace `ParseOperation` in `engine/logic/editor_scene.cpp` with:

```cpp
ImGuizmo::OPERATION ParseOperation(
    const std::string& value,
    ImGuizmo::OPERATION fallback
)
{
    if (value == "scale")
    {
        return ImGuizmo::SCALE;
    }
    if (value == "combined" || value == "translate" || value == "rotate")
    {
        return kCombinedGizmoOperation;
    }
    return fallback;
}
```

Replace the operation overload of `ToString` with:

```cpp
const char* ToString(ImGuizmo::OPERATION operation)
{
    return operation == ImGuizmo::SCALE ? "scale" : "combined";
}
```

Update `ReadGizmoSettings` to pass its fallback:

```cpp
settings.operation = ParseOperation(
    gizmoNode["operation"].as<std::string>(ToString(settings.operation)),
    settings.operation
);
```

- [ ] **Step 4: Run focused and existing scene tests**

Run:

```powershell
cmake --build --preset vs2026-x64-debug --target miniengine_gizmo_settings_tests miniengine_scene_identity_tests
ctest --test-dir .\out\build\vs2026-x64 -C Debug -R "miniengine\.(gizmo_settings|scene_identity)" --output-on-failure
```

Expected: CTest reports `100% tests passed, 0 tests failed out of 2`.

- [ ] **Step 5: Commit YAML compatibility**

```powershell
git add -- engine/logic/editor_scene.cpp tests/gizmo_settings_tests.cpp
git diff --cached --check
git commit -m "feat: persist combined gizmo mode"
```

Expected: one commit containing the YAML behavior and its regression tests.

---

### Task 3: Viewport and Inspector Integration

**Files:**
- Modify: `engine/editor/editor_ui.h`
- Modify: `engine/editor/ui/editor_viewport_panel.cpp:440-578, 971, 980`
- Modify: `engine/editor/ui/editor_scene_panel.cpp:155-221`

**Interfaces:**
- Consumes: `kCombinedGizmoOperation`, `ToggleGizmoOperation`, `GizmoDragSnapState::PrepareForManipulate`, `GizmoDragSnapState::FinishManipulate`, and `BuildGizmoSnapValues`.
- Produces: one combined viewport gizmo, `R` Combined/Scale toggling, drag-stable snap selection, and two Inspector operation buttons.

- [ ] **Step 1: Confirm the tested policy is green before UI wiring**

Run:

```powershell
ctest --test-dir .\out\build\vs2026-x64 -C Debug -R miniengine.gizmo_settings --output-on-failure
```

Expected: `miniengine.gizmo_settings` passes.

- [ ] **Step 2: Give `EditorUiController` ownership of drag snap state**

Add this include to `engine/editor/editor_ui.h`:

```cpp
#include <gizmo_settings.h>
```

Add this member beside the other viewport/editor interaction state:

```cpp
GizmoDragSnapState m_gizmoDragSnapState;
```

- [ ] **Step 3: Replace W/E/R selection with guarded R toggling**

In `HandleViewportShortcuts` in
`engine/editor/ui/editor_viewport_panel.cpp`, replace the three operation
shortcut blocks with:

```cpp
GizmoSettings& gizmo = scene.GetGizmoSettings();
if (ImGui::IsKeyPressed(ImGuiKey_R, false) && !ImGuizmo::IsUsing())
{
    gizmo.operation = ToggleGizmoOperation(gizmo.operation);
}
```

Keep the existing `F` framing block unchanged.

- [ ] **Step 4: Wire drag-stable snap selection into the single Manipulate call**

Change the function signature to:

```cpp
void DrawGizmoOverlay(
    IEditorWorld& scene,
    ViewportMatrices& matrices,
    const ViewportOverlayRect& viewportRect,
    GizmoDragSnapState& dragSnapState
)
```

At the start of the function, clear transient state when the gizmo cannot be
drawn:

```cpp
if (!scene.HasSelection() ||
    viewportRect.size.x <= 0.0f ||
    viewportRect.size.y <= 0.0f ||
    viewportRect.drawList == nullptr)
{
    dragSnapState.FinishManipulate(false);
    return;
}
```

Keep the current pivot calculation and Point/Ambient `effectiveOperation`
restriction. Replace the current operation-dependent `snapValues` block and
the setup immediately before `Manipulate` with:

```cpp
ImGuizmo::SetOrthographic(false);
ImGuizmo::SetID(static_cast<int>(entt::to_integral(selectedEntity)));
ImGuizmo::SetDrawlist(viewportRect.drawList);
ImGuizmo::SetRect(viewportRect.origin.x, viewportRect.origin.y, viewportRect.size.x, viewportRect.size.y);

const bool gizmoWasUsing = ImGuizmo::IsUsing();
const bool rotationHandleHovered =
    !gizmoWasUsing &&
    effectiveOperation != ImGuizmo::SCALE &&
    ImGuizmo::IsOver(ImGuizmo::ROTATE);
dragSnapState.PrepareForManipulate(
    effectiveOperation,
    gizmoWasUsing,
    rotationHandleHovered
);
const std::array<float, 3> snapValues =
    BuildGizmoSnapValues(gizmo, dragSnapState.Family());

ImGuizmo::Manipulate(
    glm::value_ptr(matrices.view),
    glm::value_ptr(matrices.projection),
    effectiveOperation,
    gizmo.mode,
    glm::value_ptr(gizmoMatrix),
    nullptr,
    gizmo.useSnap ? snapValues.data() : nullptr
);

const bool gizmoIsUsing = ImGuizmo::IsUsing();
dragSnapState.FinishManipulate(gizmoIsUsing);
if (!gizmoIsUsing)
{
    return;
}
```

Keep the existing model-matrix application after that guard:

```cpp
matrices.model = gizmoMatrix * inversePivotOffset;
scene.ApplyTransformMatrix(selectedEntity, matrices.model);
```

Update the call in `EditorUiController::DrawViewportPanel`:

```cpp
DrawGizmoOverlay(scene, matrices, viewportRect, m_gizmoDragSnapState);
```

Update the viewport help line:

```cpp
ImGui::TextUnformatted("F to frame, R toggles combined/scale gizmo, drag assets here to place");
```

- [ ] **Step 5: Replace Inspector operation buttons**

In `DrawGizmoControls` in `engine/editor/ui/editor_scene_panel.cpp`, replace
the Translate/Rotate/Scale button row with:

```cpp
DrawOperationButton("Combined", kCombinedGizmoOperation, gizmo.operation);
ImGui::SameLine();
DrawOperationButton("Scale", ImGuizmo::SCALE, gizmo.operation);
```

Keep World/Local and all three snap editors unchanged. Update nearby comments
that describe separate translate/rotate controls so they describe the combined
gizmo.

- [ ] **Step 6: Build the complete Debug application**

Run:

```powershell
cmake --preset vs2026-x64
cmake --build --preset vs2026-x64-debug
```

Expected: configure and build both exit with code 0; the new tests and
`miniengine_app.exe` are built.

- [ ] **Step 7: Run all automated tests**

Run:

```powershell
ctest --test-dir .\out\build\vs2026-x64 -C Debug --output-on-failure
```

Expected: CTest reports all three tests passed:
`miniengine.scene_identity`, `miniengine.vcpkg_layout_contract`, and
`miniengine.gizmo_settings`.

- [ ] **Step 8: Run the 60-frame smoke test**

Run:

```powershell
.\out\build\vs2026-x64\app\Debug\miniengine_app.exe --frames 60
```

Expected: process exits with code 0 after 60 frames without an exception or
Vulkan initialization failure.

- [ ] **Step 9: Review scope and commit UI integration**

Run:

```powershell
git diff --check
git status --short
git diff -- engine/editor/editor_ui.h engine/editor/ui/editor_viewport_panel.cpp engine/editor/ui/editor_scene_panel.cpp
```

Expected: no whitespace errors; `imgui.ini` and `.claude/` remain outside the
feature diff.

Commit only the UI integration:

```powershell
git add -- engine/editor/editor_ui.h engine/editor/ui/editor_viewport_panel.cpp engine/editor/ui/editor_scene_panel.cpp
git diff --cached --check
git commit -m "feat: add combined transform rotation gizmo"
```

- [ ] **Step 10: Hand off manual GUI acceptance**

Ask the user to launch the Debug editor and verify:

1. A selected model shows X/Y/Z move axes, XY/YZ/XZ planar handles, and
   rotation rings together by default.
2. Every planar handle constrains movement to its plane.
3. Rotation rings rotate around the expected axis.
4. Repeated `R` presses toggle Combined/Scale, and `W`/`E` do not switch mode.
5. Pressing `R` during a drag does not change mode.
6. World/Local affects the combined gizmo correctly.
7. Translation, rotation, and scale each use their own configured snap values.
8. Point and Ambient lights remain translation-only.

Expected: implementation is not declared visually accepted until the user
confirms these checks.
