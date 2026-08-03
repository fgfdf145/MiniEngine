# Repository-Wide Allman Formatting Design

## Goal

Convert every Git-tracked source file in MiniEngine to the applicable Allman brace style and add deterministic, cross-platform formatting and checking entry points that prevent regressions.

## Scope

The migration covers:

- C and C++ sources and headers under `app/`, `engine/`, and `tests/`, including the tracked Dear ImGui SDL3 and Vulkan backend copies.
- GLSL shader sources under `shaders/`.
- PowerShell and Bash scripts under `scripts/`.
- CMake source files and root `CMakeLists.txt` for common whitespace validation. CMake has no structural braces, so no brace rewrite applies.

The migration excludes Git submodules, `.deps/`, build outputs, IDE outputs, assets, linked worktrees, generated files, documentation, and untracked files. Existing user changes to `README.md`, `imgui.ini`, and `docs/superpowers/plans/` are protected and must remain untouched.

## Formatting Rules

The repository root gains a `.clang-format` based on the existing four-space Microsoft-like layout, with these explicit constraints:

- `BreakBeforeBraces: Allman`.
- Four-space indentation and no tabs.
- No single-line functions, blocks, conditionals, lambdas, or enums.
- No automatic include sorting.
- No forced column limit, so the migration does not introduce unrelated wrapping.
- Existing pointer alignment remains left-associated.

The C/C++ and GLSL migration uses clang-format 22.x. Pinning the major version prevents different formatter releases from producing inconsistent trees.

PowerShell functions and control structures place opening braces on the following line. Bash function bodies place the opening brace on the line after the function declaration. Bash control flow uses `then` and `do`, while parameter expansion and other language-required brace syntax are not structural Allman braces. CMake remains structurally unchanged.

## Tooling Architecture

`cmake/MiniEngineFormat.cmake` is the shared implementation. It obtains the candidate list from `git ls-files`, filters it to the approved source roots and extensions, and supports two modes:

- `CHECK`: run clang-format in dry-run/error mode for C/C++ and GLSL, validate PowerShell and Bash structural-brace placement, and report every violating path with a nonzero exit status.
- `APPLY`: run clang-format in place for C/C++ and GLSL, then run the same cross-language validation. Script-language violations are reported for deliberate correction because regex-based automatic rewriting would risk changing shell semantics.

Four thin entry points provide native commands on each supported host:

- `scripts/format-code.ps1`
- `scripts/format-code.sh`
- `scripts/check-format.ps1`
- `scripts/check-format.sh`

The wrappers locate Git, CMake, and clang-format, require clang-format 22.x, invoke the shared CMake script, and propagate failures. They do not modify the normal configure or build path, so contributors who only build MiniEngine do not acquire a formatter dependency.

## Migration Sequence

1. Capture the current Git status and the exact protected paths.
2. Add the format configuration and check/apply entry points.
3. Run the check entry against the old tree and confirm it fails on known non-Allman code.
4. Run the apply entry over the tracked C/C++ and GLSL files.
5. Correct PowerShell or Bash structural-brace violations deliberately.
6. Run the check entry again and require a clean result.
7. Review the full diff and use a whitespace-ignoring diff to ensure existing business source has no token-level change.

## Error Handling

The tooling stops with a clear diagnostic when Git, CMake, or clang-format is unavailable; when clang-format is not version 22.x; when candidate discovery fails; when clang-format reports a mismatch; or when a PowerShell/Bash structural-brace violation remains. APPLY mode never expands its scope to ignored, untracked, generated, submodule, or documentation files.

## Verification

Completion requires fresh evidence from all of the following:

1. The pre-migration format check fails, establishing that the checker detects the existing violations.
2. The post-migration format check passes with zero violations.
3. `git diff --check` passes.
4. A whitespace-ignoring diff shows no non-whitespace change in pre-existing application, engine, test, shader, or script code.
5. The Visual Studio 2026 x64 Debug preset builds successfully.
6. All configured x64 Debug CTest tests pass.
7. The Vulkan application completes a 60-frame smoke run.
8. Git status and path-specific diffs confirm that the protected user files are unchanged by this task.

Builds, tests, and the smoke run establish formatting, compilation, automated behavior, and basic startup evidence. They do not establish GUI or visual acceptance.
