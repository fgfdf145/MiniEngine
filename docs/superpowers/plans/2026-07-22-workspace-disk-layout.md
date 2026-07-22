# Workspace Disk Layout Guardrails Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Preserve the current architecture-isolated x64/x86 dependency roots, remove confirmed obsolete build copies, and prevent future vcpkg installations inside build trees or the repository root.

**Architecture:** Keep dependency-location policy in `cmake/MiniEngineVcpkg.cmake`, with small pure CMake functions that can be exercised in script mode. Make both bootstrap scripts pass the same explicit architecture bucket to vcpkg, then enforce the policy with a contract test before performing a narrowly validated filesystem cleanup.

**Tech Stack:** CMake 3.25+, CTest, PowerShell 7/Windows PowerShell, Bash, vcpkg manifest mode, Git.

## Global Constraints

- Preserve `.deps/vcpkg_installed/x64/` and `.deps/vcpkg_installed/x86/` without deleting or reinstalling them.
- Preserve `out/build/vs2026-x64/` and `out/build/vs2026-x86/`, except the obsolete nested `out/build/vs2026-x64/vcpkg_installed/`.
- Preserve `.deps/vcpkg/`, `.vs/`, `.cache/`, `assets/`, `.git/`, the user's `imgui.ini`, and pre-existing `docs/superpowers/plans/` content.
- Repository-local dependency installs are allowed only at `.deps/vcpkg_installed/<architecture>/`; explicit dependency roots outside the repository remain allowed.
- Reject unknown triplet architecture prefixes instead of falling back to `vcpkg_installed/` at the repository root.
- Resolve and validate every recursive deletion target as an absolute path beneath the repository root before deleting it.
- CMake remains the source of truth for every IDE and build entry point.

---

### Task 1: Add a failing vcpkg layout contract test

**Files:**
- Create: `tests/vcpkg_layout_case.cmake`
- Create: `tests/vcpkg_layout_contract.cmake`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `cmake/MiniEngineVcpkg.cmake`, `CMakePresets.json`, `scripts/bootstrap-deps.ps1`, and `scripts/bootstrap-deps.sh`.
- Produces: a standalone `cmake -P` contract test and the CTest name `miniengine.vcpkg_layout_contract`.

- [ ] **Step 1: Create the per-case validator driver**

Create `tests/vcpkg_layout_case.cmake` with this content:

```cmake
cmake_minimum_required(VERSION 3.25)

foreach(required_variable IN ITEMS REPO_ROOT CASE_TRIPLET CASE_INSTALL_DIR)
    if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR "Missing required variable: ${required_variable}")
    endif()
endforeach()

set(VCPKG_TARGET_TRIPLET "${CASE_TRIPLET}")
set(VCPKG_INSTALLED_DIR "${CASE_INSTALL_DIR}")
include("${REPO_ROOT}/cmake/MiniEngineVcpkg.cmake")
miniengine_validate_vcpkg_installed_dir(
    "${REPO_ROOT}"
    "${REPO_ROOT}/out/build/layout-contract"
    "${CASE_TRIPLET}"
    "${CASE_INSTALL_DIR}"
)
```

- [ ] **Step 2: Create the contract runner**

Create `tests/vcpkg_layout_contract.cmake` with helpers that run the case driver in a child CMake process so expected failures do not terminate the whole test:

```cmake
cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED REPO_ROOT OR "${REPO_ROOT}" STREQUAL "")
    message(FATAL_ERROR "REPO_ROOT is required")
endif()

set(case_script "${REPO_ROOT}/tests/vcpkg_layout_case.cmake")

function(run_layout_case case_name triplet install_dir expect_success)
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            "-DREPO_ROOT=${REPO_ROOT}"
            "-DCASE_TRIPLET=${triplet}"
            "-DCASE_INSTALL_DIR=${install_dir}"
            -P "${case_script}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error_output
    )

    if(expect_success AND NOT result EQUAL 0)
        message(FATAL_ERROR
            "${case_name} should pass but failed (${result})\n${output}\n${error_output}")
    endif()
    if(NOT expect_success AND result EQUAL 0)
        message(FATAL_ERROR "${case_name} should fail but passed")
    endif()
endfunction()

function(require_file_contains relative_path expected_text)
    file(READ "${REPO_ROOT}/${relative_path}" contents)
    string(FIND "${contents}" "${expected_text}" match_index)
    if(match_index EQUAL -1)
        message(FATAL_ERROR "${relative_path} does not contain: ${expected_text}")
    endif()
endfunction()

run_layout_case(
    x64_bucket x64-windows
    "${REPO_ROOT}/.deps/vcpkg_installed/x64" TRUE
)
run_layout_case(
    x86_bucket x86-windows
    "${REPO_ROOT}/.deps/vcpkg_installed/x86" TRUE
)
run_layout_case(
    external_root x64-windows
    "${REPO_ROOT}/../miniengine-shared-vcpkg/x64" TRUE
)
run_layout_case(
    repository_root x64-windows
    "${REPO_ROOT}/vcpkg_installed" FALSE
)
run_layout_case(
    build_tree x64-windows
    "${REPO_ROOT}/out/build/layout-contract/vcpkg_installed" FALSE
)
run_layout_case(
    clion_tree x64-windows
    "${REPO_ROOT}/cmake-build-debug/vcpkg_installed" FALSE
)
run_layout_case(
    wrong_bucket x64-windows
    "${REPO_ROOT}/.deps/vcpkg_installed/x86" FALSE
)

require_file_contains("CMakePresets.json" ".deps/vcpkg_installed/x64")
require_file_contains("CMakePresets.json" ".deps/vcpkg_installed/x86")
require_file_contains("scripts/bootstrap-deps.ps1" "--x-install-root=")
require_file_contains("scripts/bootstrap-deps.sh" "--x-install-root=")

message(STATUS "MiniEngine vcpkg layout contract passed")
```

- [ ] **Step 3: Register the standalone contract with CTest**

Append this test registration to `tests/CMakeLists.txt`:

```cmake
add_test(
    NAME miniengine.vcpkg_layout_contract
    COMMAND ${CMAKE_COMMAND}
        "-DREPO_ROOT=${CMAKE_SOURCE_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg_layout_contract.cmake"
)
```

- [ ] **Step 4: Run the contract test and verify RED**

Run:

```powershell
cmake -DREPO_ROOT="$PWD" -P .\tests\vcpkg_layout_contract.cmake
```

Expected: non-zero exit. The output must identify the missing `miniengine_validate_vcpkg_installed_dir` command and/or missing `--x-install-root=` script contract. A syntax error in either new test is not an acceptable RED result.

- [ ] **Step 5: Commit the failing contract**

```powershell
git add tests/vcpkg_layout_case.cmake tests/vcpkg_layout_contract.cmake tests/CMakeLists.txt
git commit -m "test: define vcpkg workspace layout contract"
```

### Task 2: Enforce one architecture-isolated dependency layout

**Files:**
- Modify: `cmake/MiniEngineVcpkg.cmake`
- Modify: `CMakeLists.txt`
- Modify: `scripts/bootstrap-deps.ps1`
- Modify: `scripts/bootstrap-deps.sh`

**Interfaces:**
- Consumes: `VCPKG_TARGET_TRIPLET`, optional `VCPKG_INSTALLED_DIR`, `CMAKE_CURRENT_SOURCE_DIR`, and `CMAKE_BINARY_DIR`.
- Produces: `miniengine_vcpkg_architecture_bucket(out_var, triplet)`, `miniengine_vcpkg_default_installed_dir(out_var, source_root, triplet)`, and `miniengine_validate_vcpkg_installed_dir(source_root, binary_root, triplet, install_dir)`.

- [ ] **Step 1: Add pure layout functions to `MiniEngineVcpkg.cmake`**

Add the following functions after `include_guard(GLOBAL)`:

```cmake
function(miniengine_vcpkg_architecture_bucket out_var triplet)
    if("${triplet}" MATCHES "^x64-")
        set(bucket "x64")
    elseif("${triplet}" MATCHES "^x86-")
        set(bucket "x86")
    elseif("${triplet}" MATCHES "^arm64-")
        set(bucket "arm64")
    else()
        message(FATAL_ERROR
            "Unsupported vcpkg triplet architecture: '${triplet}'. "
            "Expected an x64-, x86-, or arm64- prefix."
        )
    endif()
    set(${out_var} "${bucket}" PARENT_SCOPE)
endfunction()

function(miniengine_vcpkg_default_installed_dir out_var source_root triplet)
    miniengine_vcpkg_architecture_bucket(bucket "${triplet}")
    get_filename_component(
        installed_dir
        "${source_root}/.deps/vcpkg_installed/${bucket}"
        ABSOLUTE
    )
    file(TO_CMAKE_PATH "${installed_dir}" installed_dir)
    set(${out_var} "${installed_dir}" PARENT_SCOPE)
endfunction()

function(miniengine_validate_vcpkg_installed_dir source_root binary_root triplet install_dir)
    miniengine_vcpkg_default_installed_dir(
        allowed_repo_dir "${source_root}" "${triplet}"
    )
    get_filename_component(source_dir "${source_root}" ABSOLUTE)
    get_filename_component(
        resolved_install_dir "${install_dir}" ABSOLUTE BASE_DIR "${source_root}"
    )
    file(TO_CMAKE_PATH "${source_dir}" source_dir)
    file(TO_CMAKE_PATH "${resolved_install_dir}" resolved_install_dir)

    if(resolved_install_dir STREQUAL allowed_repo_dir)
        return()
    endif()

    string(TOLOWER "${source_dir}/" source_prefix)
    string(TOLOWER "${resolved_install_dir}/" install_prefix)
    string(FIND "${install_prefix}" "${source_prefix}" source_prefix_index)
    if(source_prefix_index EQUAL 0)
        message(FATAL_ERROR
            "MiniEngine rejects repository-local VCPKG_INSTALLED_DIR "
            "'${resolved_install_dir}'. Use '${allowed_repo_dir}' for "
            "triplet '${triplet}', or use an explicit path outside the repository."
        )
    endif()
endfunction()
```

Replace the old generic fallback block with logic that requires a known triplet, chooses the architecture bucket, and validates an existing override before `project()` can invoke vcpkg:

```cmake
if(DEFINED CMAKE_TOOLCHAIN_FILE)
    if(NOT DEFINED VCPKG_TARGET_TRIPLET OR "${VCPKG_TARGET_TRIPLET}" STREQUAL "")
        message(FATAL_ERROR
            "MiniEngine requires VCPKG_TARGET_TRIPLET when the vcpkg toolchain is enabled."
        )
    endif()

    miniengine_vcpkg_default_installed_dir(
        default_installed_dir
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${VCPKG_TARGET_TRIPLET}"
    )
    if(NOT DEFINED VCPKG_INSTALLED_DIR OR "${VCPKG_INSTALLED_DIR}" STREQUAL "")
        set(VCPKG_INSTALLED_DIR
            "${default_installed_dir}"
            CACHE PATH
            "Architecture-isolated vcpkg installed-packages directory"
        )
    endif()

    miniengine_validate_vcpkg_installed_dir(
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_BINARY_DIR}"
        "${VCPKG_TARGET_TRIPLET}"
        "${VCPKG_INSTALLED_DIR}"
    )
endif()
```

- [ ] **Step 2: Revalidate after `project()`**

Immediately after `project(MiniEngine LANGUAGES CXX)` in the root `CMakeLists.txt`, add:

```cmake
if(DEFINED CMAKE_TOOLCHAIN_FILE)
    miniengine_validate_vcpkg_installed_dir(
        "${CMAKE_SOURCE_DIR}"
        "${CMAKE_BINARY_DIR}"
        "${VCPKG_TARGET_TRIPLET}"
        "${VCPKG_INSTALLED_DIR}"
    )
endif()
```

This second gate detects any toolchain-side cache change made during language enablement.

- [ ] **Step 3: Make the PowerShell bootstrap use the architecture bucket**

Add this function beside `Get-DefaultTriplet` in `scripts/bootstrap-deps.ps1`:

```powershell
function Get-TripletArchitecture([string]$TripletName)
{
    if ($TripletName -match '^(x64|x86|arm64)-')
    {
        return $Matches[1]
    }
    Fail("Unsupported vcpkg triplet architecture: '$TripletName'.")
    return ""
}
```

After `$selectedTriplet` is resolved, compute and log the install root:

```powershell
$tripletArchitecture = Get-TripletArchitecture $selectedTriplet
$installedRoot = Join-Path $repoRoot ".deps\vcpkg_installed\$tripletArchitecture"

Write-Info("vcpkg installed root: $installedRoot")
```

Add the explicit root to the `vcpkg install` argument list:

```powershell
"--x-install-root=$installedRoot"
```

- [ ] **Step 4: Make the Bash bootstrap use the same bucket**

After the selected triplet is resolved in `scripts/bootstrap-deps.sh`, add:

```bash
triplet_architecture="${triplet%%-*}"
case "$triplet_architecture" in
  x64|x86|arm64)
    ;;
  *)
    die "Unsupported vcpkg triplet architecture: '$triplet'."
    ;;
esac
installed_root="$repo_root/.deps/vcpkg_installed/$triplet_architecture"
log "vcpkg installed root: $installed_root"
```

Add the explicit root to `vcpkg install`:

```bash
"--x-install-root=$installed_root" \
```

- [ ] **Step 5: Run the standalone contract and verify GREEN**

```powershell
cmake -DREPO_ROOT="$PWD" -P .\tests\vcpkg_layout_contract.cmake
```

Expected: exit code 0 and `MiniEngine vcpkg layout contract passed`.

- [ ] **Step 6: Verify supported and rejected configure paths**

Run the official preset configure without deleting its existing build tree:

```powershell
cmake --preset vs2026-x64
cmake --preset vs2026-x86
```

Expected: both exit 0 and retain `.deps/vcpkg_installed/x64` and `/x86`.

Then run the isolated negative case:

```powershell
cmake -DREPO_ROOT="$PWD" `
  -DCASE_TRIPLET=x64-windows `
  -DCASE_INSTALL_DIR="$PWD/out/build/forbidden/vcpkg_installed" `
  -P .\tests\vcpkg_layout_case.cmake
```

Expected: non-zero exit with `MiniEngine rejects repository-local VCPKG_INSTALLED_DIR`.

- [ ] **Step 7: Commit the implementation**

```powershell
git add cmake/MiniEngineVcpkg.cmake CMakeLists.txt scripts/bootstrap-deps.ps1 scripts/bootstrap-deps.sh
git commit -m "build: enforce architecture-isolated vcpkg roots"
```

### Task 3: Document and verify the maintenance boundary

**Files:**
- Modify: `README.md`
- Modify: `scripts/README.md`
- Modify: `.gitignore`

**Interfaces:**
- Consumes: the layout functions and bootstrap behavior from Task 2.
- Produces: repository-visible maintenance rules and recovery commands.

- [ ] **Step 1: Update README build guidance and development log**

Add a compact dependency-layout subsection stating:

```markdown
### vcpkg 磁盘布局

仓库内依赖只允许安装到 `.deps/vcpkg_installed/<architecture>/`；当前 Windows
preset 分别使用 `x64/` 和 `x86/`。不要把 `VCPKG_INSTALLED_DIR` 指向 `out/`、
`cmake-build-*`、仓库根 `vcpkg_installed/` 或其他仓库内目录。确需共享依赖时，
使用仓库外的绝对路径。CMake 会在安装依赖前拒绝不符合约束的路径。
```

Append a 2026-07-22 development-log entry recording the explicit bootstrap root, CMake rejection gate, contract test, and one-time removal of obsolete dependency copies.

- [ ] **Step 2: Update `scripts/README.md`**

Document that both bootstrap scripts derive `x64`, `x86`, or `arm64` from the triplet and pass `--x-install-root=.deps/vcpkg_installed/<architecture>`. State that an unknown prefix is an error and that `.deps/vcpkg/{downloads,packages,buildtrees}` remains a rebuild cache rather than another installed root.

- [ ] **Step 3: Clarify ignored-directory policy**

Add this comment above the generated-directory patterns in `.gitignore`:

```gitignore
# Ignored build directories are not valid vcpkg install roots; CMake enforces
# .deps/vcpkg_installed/<architecture> for repository-local dependencies.
```

- [ ] **Step 4: Run documentation and contract checks**

```powershell
cmake -DREPO_ROOT="$PWD" -P .\tests\vcpkg_layout_contract.cmake
git diff --check
```

Expected: both commands exit 0.

- [ ] **Step 5: Commit documentation**

```powershell
git add README.md scripts/README.md .gitignore
git commit -m "docs: define vcpkg disk layout boundary"
```

### Task 4: Remove only confirmed obsolete copies

**Files:**
- Delete generated directories only; do not stage generated-file deletion because every target is ignored.

**Interfaces:**
- Consumes: current CMake cache references and the preserve/delete lists from the design spec.
- Produces: a cleaned working directory with only the `x64` and `x86` repository-local installed roots.

- [ ] **Step 1: Snapshot preserved roots and current disk usage**

Run:

```powershell
$repoRoot = (git rev-parse --show-toplevel).Trim()
$keepRoots = @(
  (Join-Path $repoRoot '.deps\vcpkg_installed\x64'),
  (Join-Path $repoRoot '.deps\vcpkg_installed\x86')
)
$before = foreach ($path in $keepRoots) {
  if (-not (Test-Path -LiteralPath $path)) { throw "Missing preserved root: $path" }
  $measure = Get-ChildItem -LiteralPath $path -Force -File -Recurse |
    Measure-Object Length -Sum
  [pscustomobject]@{ Path=$path; Files=$measure.Count; Bytes=[int64]$measure.Sum }
}
$before | ConvertTo-Json | Set-Content -LiteralPath "$env:TEMP\miniengine-vcpkg-before.json"
Get-FileHash -Algorithm SHA256 -LiteralPath `
  (Join-Path $keepRoots[0] 'x64-windows\debug\lib\SPIRV-Tools-opt.lib'), `
  (Join-Path $keepRoots[1] 'x86-windows\debug\lib\SPIRV-Tools-opt.lib') |
  ConvertTo-Json | Set-Content -LiteralPath "$env:TEMP\miniengine-vcpkg-hashes-before.json"
```

Expected: both preserved roots exist and both snapshot files are written outside the repository.

- [ ] **Step 2: Confirm current build caches point to the preserved roots**

```powershell
Select-String -LiteralPath `
  '.\out\build\vs2026-x64\CMakeCache.txt', `
  '.\out\build\vs2026-x86\CMakeCache.txt' `
  -Pattern '^VCPKG_INSTALLED_DIR:'
```

Expected: the x64 cache points to `.deps/vcpkg_installed/x64`; the x86 cache points to `.deps/vcpkg_installed/x86`. Stop without deleting if either result differs.

- [ ] **Step 3: Resolve and validate every deletion target**

Use one PowerShell process end-to-end:

```powershell
$repoRoot = [IO.Path]::GetFullPath((git rev-parse --show-toplevel).Trim())
$repoPrefix = $repoRoot.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
$keepPaths = @(
  [IO.Path]::GetFullPath((Join-Path $repoRoot '.deps\vcpkg_installed\x64')),
  [IO.Path]::GetFullPath((Join-Path $repoRoot '.deps\vcpkg_installed\x86')),
  [IO.Path]::GetFullPath((Join-Path $repoRoot 'out\build\vs2026-x64')),
  [IO.Path]::GetFullPath((Join-Path $repoRoot 'out\build\vs2026-x86'))
)
$relativeTargets = @(
  'cmake-build-debug',
  'vcpkg_installed',
  'out\build\x64-debug',
  'out\build\vs2026-x64\vcpkg_installed',
  '.deps\vcpkg_installed\x64-windows',
  '.deps\vcpkg_installed\x86-windows',
  '.deps\vcpkg_installed\vcpkg'
)
$targets = foreach ($relative in $relativeTargets) {
  $resolved = [IO.Path]::GetFullPath((Join-Path $repoRoot $relative))
  if (-not $resolved.StartsWith($repoPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Deletion target escaped repository: $resolved"
  }
  foreach ($keep in $keepPaths) {
    $targetPrefix = $resolved.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    if ($keep.Equals($resolved, [StringComparison]::OrdinalIgnoreCase) -or
        $keep.StartsWith($targetPrefix, [StringComparison]::OrdinalIgnoreCase)) {
      throw "Deletion target overlaps preserved path: $resolved -> $keep"
    }
  }
  $resolved
}
$targets
```

Expected: seven explicit absolute paths under the MiniEngine repository and no exception.

- [ ] **Step 4: Delete validated targets in the same PowerShell process**

After the exact validation block above has completed successfully, continue in that same process:

```powershell
foreach ($target in $targets) {
  if (Test-Path -LiteralPath $target) {
    Remove-Item -LiteralPath $target -Recurse -Force
  }
}
```

Do not delete `.vs`, `.cache`, `.deps/vcpkg`, `assets`, or either preserved architecture root.

- [ ] **Step 5: Prove preserved roots are unchanged**

```powershell
$before = Get-Content -LiteralPath "$env:TEMP\miniengine-vcpkg-before.json" -Raw |
  ConvertFrom-Json
$after = foreach ($path in $before.Path) {
  $measure = Get-ChildItem -LiteralPath $path -Force -File -Recurse |
    Measure-Object Length -Sum
  [pscustomobject]@{ Path=$path; Files=$measure.Count; Bytes=[int64]$measure.Sum }
}
Compare-Object $before $after -Property Path,Files,Bytes

$hashesBefore = Get-Content -LiteralPath "$env:TEMP\miniengine-vcpkg-hashes-before.json" -Raw |
  ConvertFrom-Json
$hashesAfter = Get-FileHash -Algorithm SHA256 -LiteralPath $hashesBefore.Path
Compare-Object $hashesBefore $hashesAfter -Property Path,Hash
```

Expected: both `Compare-Object` commands produce no output.

### Task 5: Full verification and handoff

**Files:**
- Modify only if verification exposes a defect in a file already in scope.

**Interfaces:**
- Consumes: all changes from Tasks 1-4.
- Produces: fresh evidence for configuration, build, tests, repository status, and reclaimed space.

- [ ] **Step 1: Run the contract and official preset configuration**

```powershell
cmake -DREPO_ROOT="$PWD" -P .\tests\vcpkg_layout_contract.cmake
cmake --preset vs2026-x64
cmake --preset vs2026-x86
```

Expected: all three commands exit 0 without creating any forbidden `vcpkg_installed` directory.

- [ ] **Step 2: Build and run CTest for both current architectures**

```powershell
cmake --build --preset vs2026-x64-debug --parallel
ctest --test-dir .\out\build\vs2026-x64 -C Debug --output-on-failure
cmake --build --preset vs2026-x86-debug --parallel
ctest --test-dir .\out\build\vs2026-x86 -C Debug --output-on-failure
```

Expected: both builds exit 0 and all CTest tests pass, including `miniengine.vcpkg_layout_contract`.

- [ ] **Step 3: Audit forbidden directories and preserved roots**

```powershell
$forbidden = @(
  '.\cmake-build-debug',
  '.\vcpkg_installed',
  '.\out\build\x64-debug',
  '.\out\build\vs2026-x64\vcpkg_installed',
  '.\.deps\vcpkg_installed\x64-windows',
  '.\.deps\vcpkg_installed\x86-windows',
  '.\.deps\vcpkg_installed\vcpkg'
)
$forbidden | Where-Object { Test-Path -LiteralPath $_ }
Test-Path -LiteralPath '.\.deps\vcpkg_installed\x64'
Test-Path -LiteralPath '.\.deps\vcpkg_installed\x86'
```

Expected: the forbidden-path query prints nothing, followed by `True` and `True`.

- [ ] **Step 4: Re-measure the repository**

```powershell
$all = Get-ChildItem -LiteralPath . -Force -File -Recurse -ErrorAction Stop |
  Measure-Object Length -Sum
'TOTAL_GIB={0:N3}' -f ($all.Sum / 1GB)
Get-ChildItem -LiteralPath . -Force -Directory | ForEach-Object {
  $m = Get-ChildItem -LiteralPath $_.FullName -Force -File -Recurse |
    Measure-Object Length -Sum
  [pscustomobject]@{ Name=$_.Name; GiB=[math]::Round($m.Sum/1GB,3) }
} | Sort-Object GiB -Descending | Format-Table -AutoSize
```

Expected: total logical size is substantially below the 43.230 GiB baseline. Record the measured value rather than claiming the 21.9 GiB estimate as exact physical reclamation.

- [ ] **Step 5: Verify working-tree scope**

```powershell
git diff --check
git status --short
git log -5 --oneline
```

Expected: `imgui.ini` and the pre-existing README plan remain user-owned changes; only files named in Tasks 1-3 and this implementation plan are new task changes. No generated directory is staged.

- [ ] **Step 6: Commit any final in-scope verification correction**

Only if Step 1-5 required an in-scope correction:

```powershell
git add CMakeLists.txt CMakePresets.json cmake/MiniEngineVcpkg.cmake tests scripts README.md .gitignore
git commit -m "build: finalize workspace disk guardrails"
```

If no correction was required, do not create an empty commit.
