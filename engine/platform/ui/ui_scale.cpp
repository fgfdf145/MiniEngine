#include "ui_scale.h"

#include <algorithm>

namespace platform::ui
{
UiScaleConfiguration BuildDefaultUiScaleConfiguration()
{
    UiScaleConfiguration configuration{};

#if defined(__APPLE__)
    configuration.macos = kMinimumUiScale;
#endif

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
#elif defined(__APPLE__)
    return OperatingSystem::MacOS;
#elif defined(__linux__)
    return OperatingSystem::Linux;
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
    case OperatingSystem::MacOS:
        return "macOS";
    case OperatingSystem::Linux:
        return "Linux";
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

#if defined(__APPLE__)
    // SDL + ImGui already render in logical points on macOS and apply the
    // framebuffer scale separately for Retina backbuffers. Multiplying the UI
    // sizes by the display scale again makes the entire editor oversized.
    return 1.0f;
#else
    const float displayScale = SDL_GetWindowDisplayScale(window);
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
    case OperatingSystem::MacOS:
        if (configuration.macos.has_value())
        {
            return ClampUiScale(*configuration.macos);
        }
        break;
    case OperatingSystem::Linux:
        if (configuration.linux.has_value())
        {
            return ClampUiScale(*configuration.linux);
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
    case OperatingSystem::MacOS:
        configuration.macos = clampedValue;
        return;
    case OperatingSystem::Linux:
        configuration.linux = clampedValue;
        return;
    default:
        configuration.fallback = clampedValue;
        return;
    }
}
}
