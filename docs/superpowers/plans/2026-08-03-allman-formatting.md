# Repository-Wide Allman Formatting Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Convert every Git-tracked MiniEngine source file to its applicable Allman style and add deterministic Windows and Unix format/check entry points.

**Architecture:** A root `.clang-format` defines C/C++ and GLSL formatting. A shared CMake script discovers only approved Git-tracked source paths, invokes clang-format 22.x in CHECK or APPLY mode, and validates structural braces in PowerShell and Bash; four thin shell wrappers provide native entry points without affecting normal builds.

**Tech Stack:** clang-format 22.x, CMake script mode, Git, PowerShell, Bash, C++20, GLSL/Vulkan.

## Global Constraints

- Cover Git-tracked C/C++ under `app/`, `engine/`, and `tests/`, including the tracked Dear ImGui backend copies.
- Cover GLSL under `shaders/`, PowerShell and Bash under `scripts/`, and common whitespace in CMake sources.
- Use four spaces, no tabs, Allman braces, no single-line functions/blocks/conditionals/lambdas/enums, no include sorting, and no forced column limit.
- Require clang-format major version 22 for deterministic output.
- Do not touch `.deps/`, build/IDE outputs, assets, linked worktrees, generated files, documentation outside this plan/spec, or untracked files.
- Preserve the user's existing `README.md`, `imgui.ini`, and `docs/superpowers/plans/2026-07-26-combined-transform-rotation-gizmo.md` and `docs/superpowers/plans/2026-07-30-material-alpha-pipeline.md` changes.
- Automated build, CTest, and smoke evidence does not establish GUI or visual acceptance.

---

### Task 1: Add deterministic format and check tooling

**Files:**
- Create: `.clang-format`
- Create: `cmake/MiniEngineFormat.cmake`
- Create: `scripts/format-code.ps1`
- Create: `scripts/format-code.sh`
- Create: `scripts/check-format.ps1`
- Create: `scripts/check-format.sh`

**Interfaces:**
- Consumes: Git-tracked paths below `app/`, `engine/`, `tests/`, `shaders/`, `scripts/`, and `cmake/`; optional `MINIENGINE_CLANG_FORMAT` CMake variable or `CLANG_FORMAT` environment variable.
- Produces: `MINIENGINE_FORMAT_MODE=CHECK|APPLY`; native `scripts/check-format.*` and `scripts/format-code.*` entry points; exit code zero only when the requested operation and all structural checks succeed.

- [ ] **Step 1: Record and protect the starting worktree**

Run:

```powershell
git status --short --branch
git diff -- README.md imgui.ini
git status --short -- docs/superpowers/plans/2026-07-26-combined-transform-rotation-gizmo.md docs/superpowers/plans/2026-07-30-material-alpha-pipeline.md
```

Expected: `README.md` and `imgui.ini` remain modified, the two pre-existing plans remain untracked, and no task file is staged.

- [ ] **Step 2: Create the clang-format rule file**

Create `.clang-format` with exactly:

```yaml
---
Language: Cpp
BasedOnStyle: Microsoft
Standard: c++20
IndentWidth: 4
TabWidth: 4
UseTab: Never
ColumnLimit: 0
BreakBeforeBraces: Allman
AllowShortBlocksOnASingleLine: Never
AllowShortCaseExpressionOnASingleLine: false
AllowShortCaseLabelsOnASingleLine: false
AllowShortCompoundRequirementOnASingleLine: false
AllowShortEnumsOnASingleLine: false
AllowShortFunctionsOnASingleLine: None
AllowShortIfStatementsOnASingleLine: Never
AllowShortLambdasOnASingleLine: None
AllowShortLoopsOnASingleLine: false
SortIncludes: Never
DerivePointerAlignment: false
PointerAlignment: Left
LineEnding: DeriveLF
...
```

- [ ] **Step 3: Create the shared CHECK/APPLY implementation**

Create `cmake/MiniEngineFormat.cmake` with exactly:

```cmake
cmake_minimum_required(VERSION 3.25)

get_filename_component(MINIENGINE_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

if(NOT DEFINED MINIENGINE_FORMAT_MODE)
    set(MINIENGINE_FORMAT_MODE CHECK)
endif()
string(TOUPPER "${MINIENGINE_FORMAT_MODE}" MINIENGINE_FORMAT_MODE)
if(NOT MINIENGINE_FORMAT_MODE STREQUAL "CHECK" AND
   NOT MINIENGINE_FORMAT_MODE STREQUAL "APPLY")
    message(FATAL_ERROR "MINIENGINE_FORMAT_MODE must be CHECK or APPLY")
endif()

find_program(GIT_EXECUTABLE NAMES git REQUIRED)

if(DEFINED MINIENGINE_CLANG_FORMAT AND NOT MINIENGINE_CLANG_FORMAT STREQUAL "")
    set(CLANG_FORMAT_EXECUTABLE "${MINIENGINE_CLANG_FORMAT}")
else()
    set(_clang_format_hints)
    if(WIN32)
        list(APPEND _clang_format_hints
            "$ENV{ProgramFiles}/LLVM/bin"
            "$ENV{ProgramFiles}/Microsoft Visual Studio/18/Community/VC/Tools/Llvm/x64/bin")
    endif()
    find_program(CLANG_FORMAT_EXECUTABLE
        NAMES clang-format clang-format-22
        HINTS ${_clang_format_hints}
        REQUIRED)
endif()

execute_process(
    COMMAND "${CLANG_FORMAT_EXECUTABLE}" --version
    RESULT_VARIABLE _version_result
    OUTPUT_VARIABLE _version_output
    ERROR_VARIABLE _version_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT _version_result EQUAL 0)
    message(FATAL_ERROR "clang-format --version failed: ${_version_error}")
endif()
if(NOT _version_output MATCHES "clang-format version 22\\.")
    message(FATAL_ERROR "clang-format 22.x is required; found: ${_version_output}")
endif()

execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${MINIENGINE_ROOT}" ls-files --
        CMakeLists.txt app engine tests shaders scripts cmake
    RESULT_VARIABLE _git_result
    OUTPUT_VARIABLE _tracked_output
    ERROR_VARIABLE _git_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT _git_result EQUAL 0)
    message(FATAL_ERROR "git ls-files failed: ${_git_error}")
endif()

string(REPLACE "\r\n" "\n" _tracked_output "${_tracked_output}")
string(REPLACE "\n" ";" _tracked_files "${_tracked_output}")
list(FILTER _tracked_files EXCLUDE REGEX "^$")

set(_clang_files)
set(_script_files)
set(_whitespace_files)
foreach(_relative_path IN LISTS _tracked_files)
    set(_absolute_path "${MINIENGINE_ROOT}/${_relative_path}")
    if(_relative_path MATCHES "\\.(c|cc|cpp|cxx|h|hh|hpp|hxx|inl|vert|frag|comp|geom|tesc|tese|glsl)$")
        list(APPEND _clang_files "${_absolute_path}")
    endif()
    if(_relative_path MATCHES "\\.(ps1|sh)$")
        list(APPEND _script_files "${_relative_path}")
    endif()
    if(_relative_path STREQUAL "CMakeLists.txt" OR
       _relative_path MATCHES "\\.(c|cc|cpp|cxx|h|hh|hpp|hxx|inl|vert|frag|comp|geom|tesc|tese|glsl|ps1|sh|cmake)$")
        list(APPEND _whitespace_files "${_relative_path}")
    endif()
endforeach()

if(NOT _clang_files)
    message(FATAL_ERROR "No C/C++ or GLSL files were discovered")
endif()

if(MINIENGINE_FORMAT_MODE STREQUAL "APPLY")
    set(_clang_arguments -i --style=file --fallback-style=none --)
else()
    set(_clang_arguments --dry-run --Werror --style=file --fallback-style=none --)
endif()

execute_process(
    COMMAND "${CLANG_FORMAT_EXECUTABLE}" ${_clang_arguments} ${_clang_files}
    WORKING_DIRECTORY "${MINIENGINE_ROOT}"
    RESULT_VARIABLE _clang_result
    OUTPUT_VARIABLE _clang_output
    ERROR_VARIABLE _clang_error)
if(NOT _clang_result EQUAL 0)
    message(FATAL_ERROR
        "clang-format ${MINIENGINE_FORMAT_MODE} failed:\n${_clang_output}${_clang_error}")
endif()

set(_style_violations)
foreach(_relative_path IN LISTS _script_files)
    file(READ "${MINIENGINE_ROOT}/${_relative_path}" _contents)
    if(_relative_path MATCHES "\\.ps1$")
        if(_contents MATCHES "(^|\n)[^\n]*\\)[ \t]*\\{" OR
           _contents MATCHES "(^|\n)[ \t]*(else|try|finally|do)[ \t]*\\{" OR
           _contents MATCHES "(^|\n)[ \t]*(class|enum)[^\n{]*\\{")
            list(APPEND _style_violations "${_relative_path}: PowerShell opening brace must be on the following line")
        endif()
    elseif(_contents MATCHES "(^|\n)[ \t]*(function[ \t]+)?[A-Za-z_][A-Za-z0-9_]*[ \t]*(\\(\\))?[ \t]*\\{")
        list(APPEND _style_violations "${_relative_path}: Bash function opening brace must be on the following line")
    endif()
endforeach()

foreach(_relative_path IN LISTS _whitespace_files)
    file(READ "${MINIENGINE_ROOT}/${_relative_path}" _contents)
    if(_contents MATCHES "[ \t]+(\r?\n|$)")
        list(APPEND _style_violations "${_relative_path}: trailing whitespace")
    endif()
endforeach()

if(_style_violations)
    list(JOIN _style_violations "\n  " _violation_text)
    message(FATAL_ERROR "Allman/style violations:\n  ${_violation_text}")
endif()

list(LENGTH _clang_files _clang_count)
list(LENGTH _script_files _script_count)
message(STATUS
    "MiniEngine format ${MINIENGINE_FORMAT_MODE} passed: ${_clang_count} clang-format files, ${_script_count} scripts")
```

- [ ] **Step 4: Create the PowerShell entry points**

Create `scripts/check-format.ps1` with exactly:

```powershell
[CmdletBinding()]
param(
    [string]$ClangFormat = $env:CLANG_FORMAT
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$arguments = @("-DMINIENGINE_FORMAT_MODE=CHECK")
if (-not [string]::IsNullOrWhiteSpace($ClangFormat))
{
    $arguments += "-DMINIENGINE_CLANG_FORMAT=$([System.IO.Path]::GetFullPath($ClangFormat))"
}
$arguments += @("-P", (Join-Path $repoRoot "cmake/MiniEngineFormat.cmake"))

& cmake @arguments
if ($LASTEXITCODE -ne 0)
{
    throw "Format check failed with exit code $LASTEXITCODE"
}
```

Create `scripts/format-code.ps1` with exactly:

```powershell
[CmdletBinding()]
param(
    [string]$ClangFormat = $env:CLANG_FORMAT
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$arguments = @("-DMINIENGINE_FORMAT_MODE=APPLY")
if (-not [string]::IsNullOrWhiteSpace($ClangFormat))
{
    $arguments += "-DMINIENGINE_CLANG_FORMAT=$([System.IO.Path]::GetFullPath($ClangFormat))"
}
$arguments += @("-P", (Join-Path $repoRoot "cmake/MiniEngineFormat.cmake"))

& cmake @arguments
if ($LASTEXITCODE -ne 0)
{
    throw "Formatting failed with exit code $LASTEXITCODE"
}
```

- [ ] **Step 5: Create the Bash entry points**

Create `scripts/check-format.sh` with exactly:

```bash
#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/.." && pwd)"
cmake_args=(-DMINIENGINE_FORMAT_MODE=CHECK)

if [[ -n "${CLANG_FORMAT:-}" ]]
then
  cmake_args+=("-DMINIENGINE_CLANG_FORMAT=$CLANG_FORMAT")
fi

cmake "${cmake_args[@]}" -P "$repo_root/cmake/MiniEngineFormat.cmake"
```

Create `scripts/format-code.sh` with exactly:

```bash
#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/.." && pwd)"
cmake_args=(-DMINIENGINE_FORMAT_MODE=APPLY)

if [[ -n "${CLANG_FORMAT:-}" ]]
then
  cmake_args+=("-DMINIENGINE_CLANG_FORMAT=$CLANG_FORMAT")
fi

cmake "${cmake_args[@]}" -P "$repo_root/cmake/MiniEngineFormat.cmake"
```

Mark both Bash files executable:

```powershell
git update-index --add --chmod=+x scripts/check-format.sh scripts/format-code.sh
```

- [ ] **Step 6: Run the new checker against the unformatted tree**

Run:

```powershell
.\scripts\check-format.ps1 -ClangFormat 'C:\Program Files\LLVM\bin\clang-format.exe'
```

Expected: nonzero exit. Diagnostics include existing C/C++/GLSL violations such as `engine/logic/editor_world.h` or `shaders/vulkan/triangle.frag`. This is the red check proving that the guard detects the current tree.

- [ ] **Step 7: Verify and commit only the tooling**

Run:

```powershell
git diff --check -- .clang-format cmake/MiniEngineFormat.cmake scripts/check-format.ps1 scripts/check-format.sh scripts/format-code.ps1 scripts/format-code.sh
git add -- .clang-format cmake/MiniEngineFormat.cmake scripts/check-format.ps1 scripts/check-format.sh scripts/format-code.ps1 scripts/format-code.sh
git diff --cached --check
git diff --cached --name-status
git commit -m "build: enforce repository Allman formatting"
```

Expected: the staged list contains exactly those six files; protected user files are not staged.

---

### Task 2: Convert all tracked source code to Allman style

**Files:**
- Modify: all Git-tracked `*.c`, `*.cc`, `*.cpp`, `*.cxx`, `*.h`, `*.hh`, `*.hpp`, `*.hxx`, and `*.inl` files under `app/`, `engine/`, and `tests/`
- Modify: all Git-tracked `*.vert`, `*.frag`, `*.comp`, `*.geom`, `*.tesc`, `*.tese`, and `*.glsl` files under `shaders/`
- Modify: `scripts/build.ps1`
- Modify: `scripts/bootstrap-deps.sh`
- Modify: `scripts/build.sh`

**Interfaces:**
- Consumes: `.clang-format`, `MINIENGINE_FORMAT_MODE=APPLY`, and the native wrappers from Task 1.
- Produces: a source tree for which both PowerShell and Bash format checks exit zero and whose pre-existing code differs only in whitespace/layout.

- [ ] **Step 1: Convert the PowerShell inline conditional**

In `scripts/build.ps1`, replace:

```powershell
$resolvedJobs = if ($Jobs -gt 0) { $Jobs } else { [System.Environment]::ProcessorCount }
```

with:

```powershell
$resolvedJobs = if ($Jobs -gt 0)
{
    $Jobs
}
else
{
    [System.Environment]::ProcessorCount
}
```

- [ ] **Step 2: Convert every Bash function declaration**

Apply these six declaration-only replacements in `scripts/bootstrap-deps.sh`:

```diff
-log() {
+log()
+{
-step() {
+step()
+{
-die() {
+die()
+{
-require_command() {
+require_command()
+{
-detect_host() {
+detect_host()
+{
-default_triplet() {
+default_triplet()
+{
```

Each existing function body and closing brace stays in its current order; only the opening brace moves.

Apply these three declaration-only replacements in `scripts/build.sh`:

```diff
-log() {
+log()
+{
-die() {
+die()
+{
-default_preset() {
+default_preset()
+{
```

- [ ] **Step 3: Apply clang-format to every discovered C/C++ and GLSL file**

Run:

```powershell
.\scripts\format-code.ps1 -ClangFormat 'C:\Program Files\LLVM\bin\clang-format.exe'
```

Expected: exit zero and a summary reporting the number of formatted clang files and checked scripts. No documentation, untracked file, submodule, build output, or linked worktree is touched.

- [ ] **Step 4: Run the clean format check**

Run:

```powershell
.\scripts\check-format.ps1 -ClangFormat 'C:\Program Files\LLVM\bin\clang-format.exe'
```

Expected: exit zero and `MiniEngine format CHECK passed`.

- [ ] **Step 5: Prove the existing source changes are whitespace-only**

Run:

```powershell
git diff --ignore-all-space --ignore-blank-lines --exit-code -- app engine tests shaders scripts/build.ps1 scripts/bootstrap-deps.sh scripts/build.sh
```

Expected: exit zero with no diff. Any output is a stop condition: inspect and remove the non-whitespace source change before continuing.

- [ ] **Step 6: Verify scope and commit the migration**

Run:

```powershell
$allowedPattern = '^(app|engine|tests|shaders)/|^scripts/(build\.ps1|bootstrap-deps\.sh|build\.sh)$'
$changed = @(git diff --name-only)
$unexpected = @($changed | Where-Object { $_ -notmatch $allowedPattern -and $_ -notin @('README.md', 'imgui.ini') })
if ($unexpected.Count -ne 0)
{
    throw "Unexpected task paths: $($unexpected -join ', ')"
}
git diff --check -- app engine tests shaders scripts/build.ps1 scripts/bootstrap-deps.sh scripts/build.sh
git add -- app engine tests shaders scripts/build.ps1 scripts/bootstrap-deps.sh scripts/build.sh
git diff --cached --name-status
git commit -m "style: adopt Allman formatting"
```

Expected: only source/shader/script migration paths are staged; `README.md`, `imgui.ini`, and pre-existing untracked plans remain outside the commit.

---

### Task 3: Run full behavioral and scope verification

**Files:**
- Verify only: `.clang-format`, `cmake/MiniEngineFormat.cmake`, `scripts/check-format.ps1`, `scripts/check-format.sh`, `scripts/format-code.ps1`, `scripts/format-code.sh`, and all formatted sources
- Protect: `README.md`, `imgui.ini`, `docs/superpowers/plans/2026-07-26-combined-transform-rotation-gizmo.md`, `docs/superpowers/plans/2026-07-30-material-alpha-pipeline.md`

**Interfaces:**
- Consumes: committed tooling and formatted sources from Tasks 1 and 2; `vs2026-x64-debug` preset and existing CTest registrations.
- Produces: fresh format, diff, build, test, smoke, and protected-path evidence suitable for the final handoff.

- [ ] **Step 1: Re-run repository format and whitespace validation**

Run:

```powershell
.\scripts\check-format.ps1 -ClangFormat 'C:\Program Files\LLVM\bin\clang-format.exe'
git diff --check
```

Expected: both commands exit zero; the format summary reports CHECK passed.

- [ ] **Step 2: Build the x64 Debug preset**

Run:

```powershell
cmake --build --preset vs2026-x64-debug --parallel
```

Expected: exit zero and `miniengine_app.exe` is linked under `out/build/vs2026-x64/app/Debug/`.

- [ ] **Step 3: Run all configured x64 Debug tests**

Run:

```powershell
ctest --test-dir .\out\build\vs2026-x64 -C Debug --output-on-failure
```

Expected: exit zero and every discovered test passes; report the fresh passed/total count rather than assuming the historical count.

- [ ] **Step 4: Run the Vulkan 60-frame smoke path**

Run:

```powershell
.\out\build\vs2026-x64\app\Debug\miniengine_app.exe --backend vulkan --frames 60
```

Expected: exit zero after 60 frames without an unhandled exception. This does not establish GUI or visual acceptance.

- [ ] **Step 5: Verify final Git and protected-file state**

Run:

```powershell
git status --short --branch
git diff -- README.md imgui.ini
git status --short -- docs/superpowers/plans/2026-07-26-combined-transform-rotation-gizmo.md docs/superpowers/plans/2026-07-30-material-alpha-pipeline.md
git log -3 --oneline --decorate
```

Expected: the user's original protected changes and this untracked implementation plan remain; the two task commits are visible locally; nothing is pushed.
