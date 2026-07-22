# Dependency Bootstrap

This folder contains the repository bootstrap scripts that fetch and restore the third-party package dependencies used by `MiniEngine`.

## What The Scripts Do

- Clone `vcpkg` automatically when it is not already available.
- Bootstrap the local `vcpkg` executable for the current host platform.
- Run `vcpkg install` against the repository `vcpkg.json` manifest.
- Detect the default triplet automatically from the host OS and architecture.
- On Windows, warn when `VULKAN_SDK` cannot be detected.

## Usage

Windows PowerShell:

```powershell
.\scripts\bootstrap-deps.ps1
```

Linux/macOS:

```bash
./scripts/bootstrap-deps.sh
```

Inspect the architecture bucket without checking commands or creating anything:

```powershell
.\scripts\bootstrap-deps.ps1 -Triplet x64-windows -PrintInstallRoot
```

```bash
./scripts/bootstrap-deps.sh --triplet arm64-linux --print-install-root
```

Each print-only mode writes the derived absolute install root and exits before command checks, directory creation, clone, bootstrap, download, or install work.

Build after dependencies are ready:

Windows PowerShell:

```powershell
.\scripts\build.ps1 x64-debug
```

Generate a Visual Studio solution that is split by the current CMake targets:

```powershell
.\scripts\generate-sln.ps1
```

For a stable entry point that can configure its build directory automatically, open the repository-root `MiniEngine.slnx` directly. It delegates all four `Debug/Release` and `x64/Win32` combinations to the existing CMake presets and passes `--parallel` on every build/rebuild. The generation script remains useful when you want the complete CMake target graph; on Visual Studio 2026 / CMake 4.3 its generated entry file is `out/build/<preset>/MiniEngine.slnx`.

Linux/macOS:

```bash
./scripts/build.sh
```

Optional arguments:

- `--vcpkg-root <path>` or `-VcpkgRoot <path>`: use a custom vcpkg checkout path.
- `--triplet <name>` or `-Triplet <name>`: override the detected vcpkg triplet.
- `--skip-install` or `-SkipInstall`: only clone/bootstrap vcpkg, skip `vcpkg install`.
- `--print-install-root` or `-PrintInstallRoot`: print the selected architecture install root and exit without side effects.

Build script options:

- `.\scripts\build.ps1 <preset>` / `./scripts/build.sh <preset>`: select the CMake preset to build.
- `-Jobs <n>` / `--jobs <n>`: override the auto-detected logical CPU count.
- `-Target <name>` / `--target <name>`: build a specific target only.

Runtime options:

- `--backend vulkan`: use the Vulkan backend.

Solution generation options:

- `.\scripts\generate-sln.ps1 -Preset vs2026-x64`: generate the default x64 Visual Studio 2026 solution.
- `.\scripts\generate-sln.ps1 -Preset vs2026-x86`: generate the Win32 Visual Studio 2026 solution.
- `.\scripts\generate-sln.ps1 -Open`: open the generated solution (`.sln` or `.slnx`) after configure finishes.

If `Jobs` is not provided, the build scripts automatically detect the machine's logical CPU count and pass it to `cmake --build --parallel`, so the build uses all available threads by default.

When `./scripts/build.sh` is called without a preset, it selects one from the host pair: Linux x64 → `linux-debug`, Linux ARM64 → `linux-arm64-debug`, macOS ARM64 → `macos-debug`, and macOS x64 → `macos-x64-debug`. The compatible macOS release names are `macos-release` for ARM64 and `macos-x64-release` for Intel.

## Default vcpkg Location

Both bootstrap scripts derive `x64`, `x86`, or `arm64` from the selected triplet and pass `--x-install-root=.deps/vcpkg_installed/<architecture>` to `vcpkg install`. Prefix matching is case-sensitive, so unknown or mixed-case forms such as `X64-windows` fail instead of creating a second bucket. `.deps/vcpkg/{downloads,packages,buildtrees}` is rebuild cache only; it is not another installed root.

The scripts resolve `VCPKG_ROOT` in this order:

1. Existing `VCPKG_ROOT` environment variable.
2. Repository-local `.deps/vcpkg`.

If you prefer a shared/global checkout such as `C:\vcpkg`, pass it explicitly with `--vcpkg-root` / `-VcpkgRoot` or export `VCPKG_ROOT` before running the script.

## Model Import

The bootstrap flow is cross-platform, and model import now uses `tinygltf` for glTF 2.0 assets (`.gltf`, `.glb`) on every supported desktop platform.
