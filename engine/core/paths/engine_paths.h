#pragma once

#include <filesystem>
#include <optional>

namespace me
{

// Runtime-resolved engine directory roots.
//
// Every root resolves in the same order: an explicit startup override (command
// line), then an environment variable, then a compile-time default baked in at
// configure time. This is the only place a build-machine path enters the
// binary; no other target may bake one in.
namespace EnginePaths
{
struct Overrides
{
    std::optional<std::filesystem::path> projectRoot;
    std::optional<std::filesystem::path> assetsRoot;
    std::optional<std::filesystem::path> cacheRoot;
    std::optional<std::filesystem::path> shaderRoot;
};

// Resolves every root once, at startup, before any other subsystem touches the
// filesystem. When this is never called the accessors below fall back to the
// same resolution with no overrides, so test executables that link a single
// engine library still get usable roots.
void Initialize(const Overrides& overrides);

// Working root. Configuration files (miniengine.settings.json, imgui.ini) live
// directly under it. Environment: MINIENGINE_PROJECT_DIR.
const std::filesystem::path& ProjectRoot();

// A path as a scene or a manifest writes it: relative ones are relative to ProjectRoot(), never to
// the working directory, so the editor finds them wherever it was started from. Absolute paths and
// empty ones come back as they are.
std::filesystem::path ResolveProjectPath(const std::filesystem::path& path);

// Asset tree scanned by the asset registry and the asset browser.
// Environment: MINIENGINE_ASSETS_DIR. Defaults to ProjectRoot() / "assets".
const std::filesystem::path& AssetsRoot();

// Derived data (compressed material textures). Safe to delete at any time.
// Defaults to one per-user directory shared by every checkout and build
// directory (%LOCALAPPDATA%/MiniEngine/cache on Windows, else
// $XDG_CACHE_HOME/miniengine or ~/.cache/miniengine), falling back to the
// build directory when none of those is set. Environment: MINIENGINE_CACHE_DIR.
const std::filesystem::path& CacheRoot();

// Compiled SPIR-V produced by the build. Environment: MINIENGINE_SHADER_DIR.
const std::filesystem::path& ShaderRoot();
}
}
