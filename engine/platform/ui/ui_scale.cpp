#include "ui_scale.h"

#include <algorithm>

namespace me
{

namespace platform::ui
{
UiScaleConfiguration BuildDefaultUiScaleConfiguration()
{
    UiScaleConfiguration configuration{};

    return configuration;
}

float ClampUiScale(float value)
{
    return std::clamp(value, kMinimumUiScale, kMaximumUiScale);
}

OperatingSystem GetCurrentOperatingSystem()
{
#if defined(_WIN32)
    return OperatingSystem::Windows;
#elif defined(__linux__)
    return OperatingSystem::Linux;
#elif defined(__APPLE__)
    return OperatingSystem::MacOS;
#else
    return OperatingSystem::Unknown;
#endif
}

const char* GetCurrentOperatingSystemName()
{
    switch (GetCurrentOperatingSystem())
    {
    case OperatingSystem::Windows:
        return "Windows";
    case OperatingSystem::Linux:
        return "Linux";
    case OperatingSystem::MacOS:
        return "macOS";
    default:
        return "Unknown";
    }
}

float ResolveWindowUiScale(SDL_Window* window)
{
    if (window == nullptr)
    {
        return 1.0f;
    }

    const float displayScale = SDL_GetWindowDisplayScale(window);

#if defined(__APPLE__)
    // macOS windows are sized in points and the ImGui backend already maps
    // points to Retina pixels through DisplayFramebufferScale, so the pixel
    // density must not scale the UI a second time. Only the content scale
    // beyond the pixel density (if any) is relevant here.
    const float pixelDensity = SDL_GetWindowPixelDensity(window);
    if (displayScale > 0.0f && pixelDensity > 0.0f)
    {
        return displayScale / pixelDensity;
    }

    return 1.0f;
#else
    if (displayScale > 0.0f)
    {
        return displayScale;
    }

    const float pixelDensity = SDL_GetWindowPixelDensity(window);
    return pixelDensity > 0.0f ? pixelDensity : 1.0f;
#endif
}

float ResolveConfiguredUiScale(const UiScaleConfiguration& configuration)
{
    switch (GetCurrentOperatingSystem())
    {
    case OperatingSystem::Windows:
        if (configuration.windows.has_value())
        {
            return ClampUiScale(*configuration.windows);
        }
        break;
    case OperatingSystem::Linux:
        if (configuration.linux.has_value())
        {
            return ClampUiScale(*configuration.linux);
        }
        break;
    case OperatingSystem::MacOS:
        if (configuration.macos.has_value())
        {
            return ClampUiScale(*configuration.macos);
        }
        break;
    default:
        break;
    }

    return ClampUiScale(configuration.fallback);
}

void SetConfiguredUiScaleForCurrentPlatform(UiScaleConfiguration& configuration, float value)
{
    const float clampedValue = ClampUiScale(value);

    switch (GetCurrentOperatingSystem())
    {
    case OperatingSystem::Windows:
        configuration.windows = clampedValue;
        return;
    case OperatingSystem::Linux:
        configuration.linux = clampedValue;
        return;
    case OperatingSystem::MacOS:
        configuration.macos = clampedValue;
        return;
    default:
        configuration.fallback = clampedValue;
        return;
    }
}
}
}
