#include "engine_paths.h"

#include <cstdlib>
#include <string>
#include <system_error>

namespace me
{

namespace
{
struct ResolvedRoots
{
    std::filesystem::path project;
    std::filesystem::path assets;
    std::filesystem::path cache;
    std::filesystem::path shaders;
};

std::optional<std::filesystem::path> ReadEnvironmentPath(const char* name)
{
#if defined(_MSC_VER)
    // std::getenv is deprecated under MSVC; _dupenv_s is the sanctioned form.
    char* rawValue = nullptr;
    size_t length = 0;
    if (_dupenv_s(&rawValue, &length, name) != 0 || rawValue == nullptr)
    {
        return std::nullopt;
    }

    const std::string value(rawValue);
    std::free(rawValue);
#else
    const char* rawValue = std::getenv(name);
    if (rawValue == nullptr)
    {
        return std::nullopt;
    }

    const std::string value(rawValue);
#endif

    if (value.empty())
    {
        return std::nullopt;
    }

    return std::filesystem::path(value);
}

std::filesystem::path Normalize(const std::filesystem::path& path)
{
    std::error_code errorCode;
    const std::filesystem::path absolute = std::filesystem::absolute(path, errorCode);
    return (errorCode ? path : absolute).lexically_normal();
}

std::filesystem::path ResolveRoot(
    const std::optional<std::filesystem::path>& explicitPath,
    const char* environmentVariable,
    const std::filesystem::path& fallback)
{
    if (explicitPath.has_value())
    {
        return Normalize(*explicitPath);
    }

    if (const std::optional<std::filesystem::path> fromEnvironment = ReadEnvironmentPath(environmentVariable);
        fromEnvironment.has_value())
    {
        return Normalize(*fromEnvironment);
    }

    return Normalize(fallback);
}

ResolvedRoots Resolve(const EnginePaths::Overrides& overrides)
{
    ResolvedRoots roots;
    roots.project = ResolveRoot(
        overrides.projectRoot, "MINIENGINE_PROJECT_DIR", MINIENGINE_DEFAULT_PROJECT_DIR);
    roots.assets = ResolveRoot(
        overrides.assetsRoot, "MINIENGINE_ASSETS_DIR", roots.project / "assets");
    roots.cache = ResolveRoot(
        overrides.cacheRoot, "MINIENGINE_CACHE_DIR", MINIENGINE_DEFAULT_CACHE_DIR);
    roots.shaders = ResolveRoot(
        overrides.shaderRoot, "MINIENGINE_SHADER_DIR", MINIENGINE_DEFAULT_SHADER_DIR);
    return roots;
}

ResolvedRoots& Roots()
{
    static ResolvedRoots roots = Resolve(EnginePaths::Overrides{});
    return roots;
}
}

namespace EnginePaths
{
void Initialize(const Overrides& overrides)
{
    Roots() = Resolve(overrides);
}

const std::filesystem::path& ProjectRoot()
{
    return Roots().project;
}

const std::filesystem::path& AssetsRoot()
{
    return Roots().assets;
}

const std::filesystem::path& CacheRoot()
{
    return Roots().cache;
}

const std::filesystem::path& ShaderRoot()
{
    return Roots().shaders;
}
}
}
