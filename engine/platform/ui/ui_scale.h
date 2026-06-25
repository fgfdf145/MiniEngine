#pragma once

#include <SDL3/SDL_video.h>

#include <optional>

namespace platform::ui
{
enum class OperatingSystem
{
    Windows,
    Linux,
    Unknown
};

struct UiScaleConfiguration
{
    float fallback = 1.0f;
    std::optional<float> windows;
    std::optional<float> linux;
};

constexpr float kMinimumUiScale = 0.75f;
constexpr float kMaximumUiScale = 3.0f;

UiScaleConfiguration BuildDefaultUiScaleConfiguration();
float ClampUiScale(float value);
OperatingSystem GetCurrentOperatingSystem();
const char* GetCurrentOperatingSystemName();
float ResolveWindowUiScale(SDL_Window* window);
float ResolveConfiguredUiScale(const UiScaleConfiguration& configuration);
void SetConfiguredUiScaleForCurrentPlatform(UiScaleConfiguration& configuration, float value);
}
