#pragma once

#include <engine/renderer/spherical_harmonics.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace me
{

// The light the Material Editor's preview is shown in (docs/design/2026-10-09-material-editor-redesign-design.md),
// made up rather than taken from the scene, as Unreal's preview scene is its own: a sky around the
// object and a key light that casts shadows. Radiance is in the preview's own unit, which its
// exposure turns into GT7 frame-buffer units.
enum class MaterialPreviewEnvironmentPreset : uint32_t
{
    // A dark grey room with a softbox overhead and strip lights either side: car paint shows its
    // coat, metals their colour.
    Studio = 0,
    // A blue sky over brown ground, and the sun.
    Daylight = 1
};

inline constexpr uint32_t kMaterialPreviewEnvironmentPresetCount = 2;
const char* ToString(MaterialPreviewEnvironmentPreset preset);

// An environment ready to shade with: the sky as equirectangular radiance (EquirectangularDirection's
// layout), prefiltered for GGX lobes of increasing roughness as the environment probe's mips are,
// projected on spherical harmonics for the diffuse light, and the key light. Immutable once built.
class MaterialPreviewEnvironment
{
  public:
    // Built once per preset and kept (a few milliseconds on the task system).
    static std::shared_ptr<const MaterialPreviewEnvironment> Get(MaterialPreviewEnvironmentPreset preset);

    // The sky along direction, at the given perceptual roughness (0 the sharp sky, 1 the widest
    // lobe), as SampleLevel(R, roughness * (mips - 1)) on the probe reads it.
    glm::vec3 Specular(const glm::vec3& direction, float roughness) const;
    // What the background shows: the sky, slightly blurred.
    glm::vec3 Background(const glm::vec3& direction) const;
    // The irradiance on a surface facing normal, over pi: the radiance a white Lambertian surface
    // sends back (SceneDiffuseAmbient's unit).
    glm::vec3 DiffuseRadiance(const glm::vec3& normal) const;

    // The key light at its default direction (unit, toward the light), its illuminance on a surface
    // facing it, and its angular radius in radians (soft shadows and highlights).
    glm::vec3 KeyDirection() const
    {
        return m_keyDirection;
    }
    glm::vec3 KeyIlluminance() const
    {
        return m_keyIlluminance;
    }
    float KeyAngularRadius() const
    {
        return m_keyAngularRadius;
    }
    MaterialPreviewEnvironmentPreset Preset() const
    {
        return m_preset;
    }

    struct Level
    {
        int width = 0;
        int height = 0;
        std::vector<glm::vec3> texels;
        glm::vec3 Sample(const glm::vec3& direction) const;
    };

  private:
    static std::shared_ptr<const MaterialPreviewEnvironment> Build(MaterialPreviewEnvironmentPreset preset);

    MaterialPreviewEnvironmentPreset m_preset = MaterialPreviewEnvironmentPreset::Studio;
    std::vector<Level> m_levels;
    ShCoefficients m_sh{};
    glm::vec3 m_keyDirection{0.0f, 1.0f, 0.0f};
    glm::vec3 m_keyIlluminance{0.0f};
    float m_keyAngularRadius = 0.0f;
};

// Where a direction falls in an equirectangular map, the inverse of EquirectangularDirection.
glm::vec2 MaterialPreviewEquirectangularUv(const glm::vec3& direction);
}
