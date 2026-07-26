# Combined Transform and Rotation Gizmo Design

## Goal

Replace the separate translate and rotate editor gizmo modes with one combined
mode that behaves like Unity's combined transform tool. The combined gizmo
shows translation axes, translation planes, and rotation rings at the same
time. Scale remains a separate mode.

## User Interaction

- The editor starts in combined mode.
- Combined mode displays:
  - X, Y, and Z translation axes.
  - XY, YZ, and XZ planar translation handles.
  - X, Y, and Z rotation rings, including the screen-space rotation control
    supplied by ImGuizmo.
- `W` and `E` no longer change the gizmo mode.
- `R` toggles between combined mode and scale mode.
- Mode switching is ignored while a gizmo drag is active. The next `R` press
  after the drag ends performs the toggle.
- The scene inspector exposes two operation choices: `Combined` and `Scale`.
- World/Local mode and the existing translation, rotation, and scale snap
  settings remain available.
- Point and Ambient lights remain translation-only because their transforms
  have no meaningful editable orientation or scale in the current editor.

## Architecture

`GizmoSettings::operation` remains an `ImGuizmo::OPERATION`. Combined mode is
represented by the native ImGuizmo bit mask:

```cpp
ImGuizmo::TRANSLATE | ImGuizmo::ROTATE
```

The viewport continues to make one `ImGuizmo::Manipulate` call per frame. A
single call lets ImGuizmo resolve overlapping handles through its existing
hit-testing and avoids competing global manipulation state from multiple
gizmo calls.

The editor will centralize the operation values and mode transitions in small
logic helpers so they can be tested without rendering a viewport. The UI and
keyboard paths will use those helpers rather than duplicating bit-mask
comparisons.

## Snap Selection

ImGuizmo accepts one snap pointer per `Manipulate` call even when the operation
contains both translation and rotation bits. The editor therefore records the
handle family selected at the start of a combined-mode drag:

- Translation axes and planar handles select `translationSnap`.
- Rotation rings select `rotationSnap`.
- Scale mode selects `scaleSnap`.

The selected snap family remains fixed until the drag ends. This prevents a
snap pointer from changing while the object is already being manipulated. When
snap is disabled, the call passes a null snap pointer as it does today.

The drag state is editor viewport interaction state. It is not scene data and
is never serialized.

## Scene Compatibility

The scene YAML writer emits:

- `operation: combined` for the combined translate-and-rotate mode.
- `operation: scale` for scale mode.

The loader accepts the new values and upgrades legacy values as follows:

| Stored value | Loaded mode |
| --- | --- |
| `combined` | Combined |
| `translate` | Combined |
| `rotate` | Combined |
| `scale` | Scale |
| Missing or unknown | Existing fallback, which defaults to Combined |

This is a read-compatible schema change. Existing scenes remain loadable, and
the next save normalizes legacy translate or rotate values to `combined`.

## Scope

The implementation updates:

- Gizmo operation defaults and transition helpers.
- Viewport shortcut handling and overlay manipulation.
- Scene inspector operation controls.
- Gizmo YAML parsing and emission.
- Viewport shortcut help text and nearby comments.
- Focused automated tests for transition, serialization compatibility, and
  snap-family selection.

The implementation does not change:

- Transform matrix construction or decomposition.
- Model-bound center pivot offsets.
- Entity selection or picking.
- Light wireframe gizmos.
- Camera controls.
- ImGuizmo itself or the vcpkg dependency version.

## Verification

Automated verification:

1. Test that `R` toggles Combined to Scale and Scale to Combined.
2. Test that legacy `translate` and `rotate` YAML values load as Combined.
3. Test that Combined and Scale serialize to their canonical YAML values.
4. Test that combined-mode handle selection chooses translation snap for axes
   and planes, rotation snap for rings, and keeps that choice for the drag.
5. Configure/build the `vs2026-x64-debug` preset.
6. Run CTest with failure output enabled.
7. Run the Debug application for 60 frames as a smoke test.

Manual GUI acceptance:

1. Select a model entity and verify that translation axes, all three planar
   handles, and rotation rings are visible together by default.
2. Drag each planar handle and verify movement is constrained to its plane.
3. Drag rotation rings and verify the expected rotation.
4. Press `R` repeatedly and verify reliable Combined/Scale toggling.
5. Verify World/Local behavior.
6. Enable snap and verify translation, rotation, and scale use their respective
   configured values.
7. Select Point and Ambient lights and verify they remain translation-only.

Automated tests establish logic and startup stability, but the visual and
dragging behavior is accepted only after the user completes the GUI checks.
