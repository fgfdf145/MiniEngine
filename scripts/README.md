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

Linux:

```bash
./scripts/bootstrap-deps.sh
```

Build after dependencies are ready:

Windows PowerShell:

```powershell
.\scripts\build.ps1 x64-debug
```

Generate a Visual Studio solution that is split by the current CMake targets:

```powershell
.\scripts\generate-sln.ps1
```

On Visual Studio 2026 / CMake 4.3, the generated solution entry file is typically `MiniEngine.slnx`.

Linux:

```bash
./scripts/build.sh
```

Optional arguments:

- `--vcpkg-root <path>` or `-VcpkgRoot <path>`: use a custom vcpkg checkout path.
- `--triplet <name>` or `-Triplet <name>`: override the detected vcpkg triplet.
- `--skip-install` or `-SkipInstall`: only clone/bootstrap vcpkg, skip `vcpkg install`.

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

## Default vcpkg Location

The scripts resolve `VCPKG_ROOT` in this order:

1. Existing `VCPKG_ROOT` environment variable.
2. Repository-local `.deps/vcpkg`.

If you prefer a shared/global checkout such as `C:\vcpkg`, pass it explicitly with `--vcpkg-root` / `-VcpkgRoot` or export `VCPKG_ROOT` before running the script.

## Model Import

The bootstrap flow is cross-platform, and model import now uses `tinygltf` for glTF 2.0 assets (`.gltf`, `.glb`) on every supported desktop platform.
