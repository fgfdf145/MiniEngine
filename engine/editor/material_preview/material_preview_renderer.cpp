#include "material_preview_renderer.h"

#include <engine/core/threading/task_system.h>

#include <TaskScheduler.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace me
{

namespace
{
constexpr uint32_t kTileSize = 32;
// Blend surfaces one behind another a primary ray composes before it stops.
constexpr uint32_t kMaxLayers = 8;
constexpr float kPi = 3.14159265358979f;
// The preview's unit (a white surface in its light) in GT7 frame-buffer units at exposure 0: a
// little under the SDR paper white (2.5), so white keeps some of the curve's shoulder.
constexpr float kBaseExposure = 1.6f;

uint32_t Hash(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float ToUnit(uint32_t bits)
{
    return static_cast<float>(bits >> 8) * (1.0f / 16777216.0f);
}

// The R2 sequence's nth point (Roberts 2018), shifted by a per-pixel offset (Cranley-Patterson), so
// each pixel's samples cover the square evenly and neighbouring pixels differ.
glm::vec2 SequencePoint(uint32_t index, const glm::vec2& offset)
{
    const glm::vec2 point = offset + static_cast<float>(index) * glm::vec2(0.7548776662f, 0.5698402910f);
    return point - glm::floor(point);
}

// Any unit vector perpendicular to n, and the third axis.
void Basis(const glm::vec3& n, glm::vec3& tangent, glm::vec3& bitangent)
{
    const float s = n.z >= 0.0f ? 1.0f : -1.0f;
    const float a = -1.0f / (s + n.z);
    const float b = n.x * n.y * a;
    tangent = glm::vec3(1.0f + s * n.x * n.x * a, s * b, -s * n.x);
    bitangent = glm::vec3(b, s + n.y * n.y * a, -n.y);
}

// A direction in the cone of half-angle acos(cosMax) about axis, uniform in solid angle.
glm::vec3 SampleCone(const glm::vec3& axis, float cosMax, const glm::vec2& u)
{
    const float cosTheta = 1.0f - u.x * (1.0f - cosMax);
    const float sinTheta = std::sqrt(std::max(1.0f - cosTheta * cosTheta, 0.0f));
    const float phi = 2.0f * kPi * u.y;
    glm::vec3 tangent;
    glm::vec3 bitangent;
    Basis(axis, tangent, bitangent);
    return glm::normalize(tangent * (sinTheta * std::cos(phi)) + bitangent * (sinTheta * std::sin(phi)) + axis * cosTheta);
}

// A cosine-weighted direction about n.
glm::vec3 SampleCosine(const glm::vec3& n, const glm::vec2& u)
{
    const float radius = std::sqrt(u.x);
    const float phi = 2.0f * kPi * u.y;
    glm::vec3 tangent;
    glm::vec3 bitangent;
    Basis(n, tangent, bitangent);
    return glm::normalize(tangent * (radius * std::cos(phi)) + bitangent * (radius * std::sin(phi)) + n * std::sqrt(std::max(1.0f - u.x, 0.0f)));
}

uint8_t EncodeSrgb(float linear)
{
    const float clamped = std::clamp(linear, 0.0f, 1.0f);
    const float encoded = clamped <= 0.0031308f ? clamped * 12.92f : 1.055f * std::pow(clamped, 1.0f / 2.4f) - 0.055f;
    return static_cast<uint8_t>(std::clamp(encoded * 255.0f + 0.5f, 0.0f, 255.0f));
}

float SmoothStep(float edge0, float edge1, float x)
{
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
}

glm::vec3 MaterialPreviewCamera::Position() const
{
    return target + distance * glm::vec3(std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw));
}

// Everything a pass reads: shared, immutable inputs, the frame's numbers, and the images it writes
// (the renderer's, untouched by anything else while the pass runs).
struct MaterialPreviewRenderer::PassInput
{
    std::shared_ptr<const MaterialPreviewGeometry> geometry;
    std::shared_ptr<const std::vector<MaterialPreviewMaterial>> materials;
    std::shared_ptr<const MaterialPreviewEnvironment> environment;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t sample = 0;
    bool writeDisplay = true;
    glm::vec3 eye{0.0f};
    glm::vec3 forward{0.0f, 0.0f, -1.0f};
    glm::vec3 right{1.0f, 0.0f, 0.0f};
    glm::vec3 up{0.0f, 1.0f, 0.0f};
    float tanHalfFov = 0.3f;
    float aspect = 1.0f;
    // Radians a pixel spans, for the textures' level of detail.
    float pixelSpread = 0.0f;
    MaterialPreviewLighting lighting;
    float keyCosRadius = 1.0f;
    float exposure = kBaseExposure;
    bool floor = false;
    float floorY = 0.0f;
    glm::vec2 floorCentre{0.0f};
    float floorFadeStart = 0.0f;
    float floorFadeEnd = 1.0f;
    float gridSpacing = 1.0f;
    // The sky's diffuse light on the floor, and the object's box.
    glm::vec3 floorAmbient{0.0f};
    glm::vec3 sceneMin{0.0f};
    glm::vec3 sceneMax{0.0f};
    // How far the occlusion rays look, and the offset that keeps secondary rays off their surface.
    float aoDistance = 1.0f;
    float epsilon = 1e-4f;
    glm::vec3* accumulation = nullptr;
    int32_t* slots = nullptr;
    uint8_t* display = nullptr;
};

class MaterialPreviewRenderer::PassTask : public enki::ITaskSet
{
  public:
    PassInput input;

    void ExecuteRange(enki::TaskSetPartition range, uint32_t threadNumber) override
    {
        static_cast<void>(threadNumber);
        for (uint32_t tile = range.start; tile < range.end; ++tile)
        {
            TraceTile(tile);
        }
    }

  private:
    // A Mask surface's cut-out parts are not there for any ray.
    bool MaskCovers(const MaterialPreviewMaterial& material, const MaterialPreviewCandidate& candidate, const Ray& ray, float pixelSpread) const
    {
        const MaterialPreviewMesh& mesh = input.geometry->Meshes().data()[candidate.mesh];
        const MaterialPreviewSurfacePoint point =
            InterpolateMaterialPreviewPoint(*mesh.mesh, candidate.triangle, candidate.u, candidate.v, ray, candidate.t, pixelSpread);
        return MaterialPreviewAlpha(material, point) >= material.material.alphaCutoff;
    }

    // Shadow and occlusion rays: blocked by every surface but a Blend one (as the renderer's rays
    // pass through glass and decals) and a Mask surface's cut-outs, from either side.
    bool Occluded(const Ray& ray) const
    {
        if (!input.geometry)
        {
            return false;
        }
        const MaterialPreviewMaterial* const materials = input.materials->data();
        return input.geometry->Occluded(ray, [&](const MaterialPreviewCandidate& candidate)
                                        {
                                            const MaterialPreviewMaterial& material = materials[candidate.mesh];
                                            if (material.alphaMode == MaterialAlphaMode::Blend)
                                            {
                                                return false;
                                            }
                                            return material.alphaMode != MaterialAlphaMode::Mask || MaskCovers(material, candidate, ray, 0.0f);
                                        });
    }

    float FloorDistance(const Ray& ray) const
    {
        if (!input.floor || ray.direction.y >= 0.0f || ray.origin.y <= input.floorY)
        {
            return std::numeric_limits<float>::infinity();
        }
        return (input.floorY - ray.origin.y) / ray.direction.y;
    }

    // The key light's visibility through one shadow ray toward a point of its disk, and the ambient
    // light's through one occlusion ray (the floor counts for the latter).
    void TraceVisibility(
        const glm::vec3& origin,
        const glm::vec3& offsetNormal,
        bool needKey,
        uint32_t pixel,
        float& keyVisibility,
        float& ambientVisibility) const
    {
        const uint32_t seed = Hash(pixel * 0x9E3779B1u + 0x632BE5ABu);
        keyVisibility = 1.0f;
        if (needKey)
        {
            const glm::vec2 u = SequencePoint(input.sample, glm::vec2(ToUnit(Hash(seed ^ 0x1u)), ToUnit(Hash(seed ^ 0x2u))));
            const glm::vec3 direction = SampleCone(input.lighting.keyDirection, input.keyCosRadius, u);
            keyVisibility = Occluded(Ray{origin, direction, 0.0f, std::numeric_limits<float>::infinity()}) ? 0.0f : 1.0f;
        }
        const glm::vec2 u = SequencePoint(input.sample, glm::vec2(ToUnit(Hash(seed ^ 0x3u)), ToUnit(Hash(seed ^ 0x4u))));
        const glm::vec3 direction = SampleCosine(offsetNormal, u);
        const Ray ray{origin, direction, 0.0f, input.aoDistance};
        ambientVisibility = FloorDistance(ray) < input.aoDistance || Occluded(ray) ? 0.0f : 1.0f;
    }

    // The floor is a matte backdrop: the key light through its shadow ray and the sky's diffuse light
    // through an occlusion ray, Lambertian, no specular lobe. Its occlusion ray is traced only where
    // something could block it, within the occlusion distance of the object's box.
    glm::vec3 ShadeFloor(const Ray& ray, float t, uint32_t pixel) const
    {
        const glm::vec3 position = ray.origin + ray.direction * t;
        // Grid lines a pixel or so wide, every spacing, and stronger every fifth.
        const float footprint = std::max(input.pixelSpread * t / std::max(-ray.direction.y, 0.1f), 1e-6f);
        const auto line = [&](float spacing, float widthPixels)
        {
            const glm::vec2 cell(position.x / spacing, position.z / spacing);
            const glm::vec2 distance = glm::abs(cell - glm::round(cell)) * spacing;
            const float width = footprint * widthPixels;
            return 1.0f - SmoothStep(0.0f, width, std::min(distance.x, distance.y));
        };
        const float minor = line(input.gridSpacing, 0.8f);
        const float major = line(input.gridSpacing * 5.0f, 1.2f);
        const float albedo = glm::mix(0.22f, 0.15f, std::max(minor * 0.6f, major));
        const glm::vec3 up(0.0f, 1.0f, 0.0f);
        const glm::vec3 origin = position + up * input.epsilon;
        const uint32_t seed = Hash(pixel * 0x9E3779B1u + 0x632BE5ABu);
        float keyVisibility = 0.0f;
        if (input.lighting.keyDirection.y > 0.0f)
        {
            const glm::vec2 u = SequencePoint(input.sample, glm::vec2(ToUnit(Hash(seed ^ 0x1u)), ToUnit(Hash(seed ^ 0x2u))));
            const glm::vec3 direction = SampleCone(input.lighting.keyDirection, input.keyCosRadius, u);
            keyVisibility = Occluded(Ray{origin, direction, 0.0f, std::numeric_limits<float>::infinity()}) ? 0.0f : 1.0f;
        }
        float ambientVisibility = 1.0f;
        if (position.x > input.sceneMin.x - input.aoDistance && position.x < input.sceneMax.x + input.aoDistance &&
            position.z > input.sceneMin.z - input.aoDistance && position.z < input.sceneMax.z + input.aoDistance)
        {
            const glm::vec2 u = SequencePoint(input.sample, glm::vec2(ToUnit(Hash(seed ^ 0x3u)), ToUnit(Hash(seed ^ 0x4u))));
            ambientVisibility = Occluded(Ray{origin, SampleCosine(up, u), 0.0f, input.aoDistance}) ? 0.0f : 1.0f;
        }
        return albedo * (input.lighting.keyIlluminance * (input.lighting.keyDirection.y * keyVisibility / kPi) +
                         input.floorAmbient * ambientVisibility);
    }

    // What a camera ray sees: the surfaces it meets, Blend ones composed over what lies behind them,
    // then the floor or the sky. slot: the material slot of the nearest surface, -1 for none.
    glm::vec3 TracePixel(Ray ray, uint32_t pixel, int32_t& slot) const
    {
        slot = -1;
        glm::vec3 throughput(1.0f);
        glm::vec3 result(0.0f);
        const float floorDistance = FloorDistance(ray);
        const std::vector<MaterialPreviewMaterial>* const materials = input.materials.get();
        for (uint32_t layer = 0; layer < kMaxLayers; ++layer)
        {
            MaterialPreviewHit hit;
            const bool found = input.geometry && materials != nullptr &&
                               input.geometry->Intersect(ray, hit, [&](const MaterialPreviewCandidate& candidate)
                                                         {
                                                             const MaterialPreviewMaterial& material = materials->data()[candidate.mesh];
                                                             // Single-sided surfaces are culled from behind, as the rasteriser culls them.
                                                             if (!material.doubleSided && !candidate.frontFace)
                                                             {
                                                                 return false;
                                                             }
                                                             return material.alphaMode != MaterialAlphaMode::Mask ||
                                                                    MaskCovers(material, candidate, ray, input.pixelSpread);
                                                         });
            if (!found || hit.t >= floorDistance)
            {
                glm::vec3 behind = input.environment->Background(ray.direction);
                if (std::isfinite(floorDistance))
                {
                    const glm::vec3 position = ray.origin + ray.direction * floorDistance;
                    const float fade = SmoothStep(
                        input.floorFadeStart, input.floorFadeEnd, glm::length(glm::vec2(position.x, position.z) - input.floorCentre));
                    if (fade < 1.0f)
                    {
                        behind = glm::mix(ShadeFloor(ray, floorDistance, pixel), behind, fade);
                    }
                }
                result += throughput * behind;
                break;
            }
            const MaterialPreviewMesh& mesh = input.geometry->Meshes().data()[hit.mesh];
            const MaterialPreviewMaterial& material = materials->data()[hit.mesh];
            if (slot < 0)
            {
                slot = static_cast<int32_t>(mesh.materialSlot);
            }
            MaterialPreviewSurfacePoint point =
                InterpolateMaterialPreviewPoint(*mesh.mesh, hit.triangle, hit.u, hit.v, ray, hit.t, input.pixelSpread);
            point.lodDither = ToUnit(Hash(pixel * 0x2545F491u + input.sample * 0x9E3779B9u + layer));
            const MaterialPreviewSurface surface = EvaluateMaterialPreviewSurface(material, point);
            const float coverage = material.alphaMode == MaterialAlphaMode::Blend ? std::clamp(surface.alpha, 0.0f, 1.0f) : 1.0f;
            glm::vec3 color;
            if ((surface.flags & kShadingFlagUnlit) != 0u)
            {
                color = ShadeMaterialPreviewSurface(surface, -ray.direction, input.lighting, 1.0f, 1.0f) / input.exposure;
            }
            else
            {
                const glm::vec3 offsetNormal = point.frontFacing ? point.faceNormal : -point.faceNormal;
                const glm::vec3 origin = point.position + offsetNormal * input.epsilon;
                const bool needKey = glm::dot(surface.normal, input.lighting.keyDirection) > 0.0f ||
                                     glm::dot(surface.coatNormal, input.lighting.keyDirection) > 0.0f;
                float keyVisibility = 1.0f;
                float ambientVisibility = 1.0f;
                TraceVisibility(origin, offsetNormal, needKey, pixel, keyVisibility, ambientVisibility);
                color = ShadeMaterialPreviewSurface(surface, -ray.direction, input.lighting, keyVisibility, ambientVisibility);
            }
            result += throughput * coverage * color;
            throughput *= 1.0f - coverage;
            if (std::max({throughput.r, throughput.g, throughput.b}) < 1.0f / 512.0f)
            {
                break;
            }
            ray.tMin = hit.t + input.epsilon;
        }
        return result;
    }

    void TraceTile(uint32_t tile) const
    {
        const uint32_t tilesX = (input.width + kTileSize - 1) / kTileSize;
        const uint32_t x0 = (tile % tilesX) * kTileSize;
        const uint32_t y0 = (tile / tilesX) * kTileSize;
        const uint32_t x1 = std::min(x0 + kTileSize, input.width);
        const uint32_t y1 = std::min(y0 + kTileSize, input.height);
        const float inverseSamples = 1.0f / static_cast<float>(input.sample + 1);
        for (uint32_t y = y0; y < y1; ++y)
        {
            for (uint32_t x = x0; x < x1; ++x)
            {
                const uint32_t pixel = y * input.width + x;
                // The first sample goes through the pixel's centre, so the slots it records are the
                // pixel's own; the others spread over the pixel.
                const uint32_t seed = Hash(pixel * 0x85EBCA77u + 0x27D4EB2Fu);
                const glm::vec2 jitter =
                    input.sample == 0 ? glm::vec2(0.5f) : SequencePoint(input.sample, glm::vec2(ToUnit(Hash(seed)), ToUnit(Hash(seed ^ 0x5u))));
                const float ndcX = (2.0f * (static_cast<float>(x) + jitter.x) / static_cast<float>(input.width) - 1.0f) * input.tanHalfFov * input.aspect;
                const float ndcY = (1.0f - 2.0f * (static_cast<float>(y) + jitter.y) / static_cast<float>(input.height)) * input.tanHalfFov;
                const Ray ray{input.eye, glm::normalize(input.forward + input.right * ndcX + input.up * ndcY), 0.0f, std::numeric_limits<float>::infinity()};
                int32_t slot = -1;
                glm::vec3 color = TracePixel(ray, pixel, slot);
                if (!std::isfinite(color.r) || !std::isfinite(color.g) || !std::isfinite(color.b))
                {
                    color = glm::vec3(0.0f);
                }
                glm::vec3& sum = input.accumulation[pixel];
                sum = input.sample == 0 ? color : sum + color;
                if (input.sample == 0)
                {
                    input.slots[pixel] = slot;
                }
                if (input.writeDisplay)
                {
                    const glm::vec3 mapped = MaterialPreviewToneMap(sum * (inverseSamples * input.exposure));
                    uint8_t* out = input.display + static_cast<size_t>(pixel) * 4;
                    out[0] = EncodeSrgb(mapped.r);
                    out[1] = EncodeSrgb(mapped.g);
                    out[2] = EncodeSrgb(mapped.b);
                    out[3] = 255;
                }
            }
        }
    }
};

MaterialPreviewRenderer::MaterialPreviewRenderer() = default;

MaterialPreviewRenderer::~MaterialPreviewRenderer()
{
    if (m_task && TaskSystem::IsRunning())
    {
        TaskSystem::Scheduler().WaitforTask(m_task.get());
    }
}

void MaterialPreviewRenderer::SetScene(
    std::shared_ptr<const MaterialPreviewGeometry> geometry, std::shared_ptr<const std::vector<MaterialPreviewMaterial>> materials)
{
    m_geometry = std::move(geometry);
    m_materials = std::move(materials);
}

void MaterialPreviewRenderer::SetMaterials(std::shared_ptr<const std::vector<MaterialPreviewMaterial>> materials)
{
    m_materials = std::move(materials);
}

void MaterialPreviewRenderer::SetHighlightSlot(int slot)
{
    if (slot == m_highlightSlot)
    {
        return;
    }
    m_highlightSlot = slot;
    if (!m_task && m_frame.width > 0)
    {
        ComposeFrame();
        m_newFrame = true;
    }
}

bool MaterialPreviewRenderer::TakeNewFrame()
{
    return std::exchange(m_newFrame, false);
}

int MaterialPreviewRenderer::SlotAt(float u, float v) const
{
    if (m_frame.width == 0 || m_frame.height == 0 || m_frame.slots.empty() || u < 0.0f || v < 0.0f || u >= 1.0f || v >= 1.0f)
    {
        return -1;
    }
    const uint32_t x = std::min(static_cast<uint32_t>(u * static_cast<float>(m_frame.width)), m_frame.width - 1);
    const uint32_t y = std::min(static_cast<uint32_t>(v * static_cast<float>(m_frame.height)), m_frame.height - 1);
    return m_frame.slots[static_cast<size_t>(y) * m_frame.width + x];
}

bool MaterialPreviewRenderer::IsRendering() const
{
    return m_task != nullptr || m_samples < m_targetSamples;
}

void MaterialPreviewRenderer::Update(
    uint32_t width, uint32_t height, const MaterialPreviewCamera& camera, const MaterialPreviewSettings& settings, bool interacting)
{
    if (m_task)
    {
        if (TaskSystem::IsRunning() && !m_task->GetIsComplete())
        {
            return;
        }
        FinishPass();
    }
    width = std::max(width, 1u);
    height = std::max(height, 1u);
    if (interacting)
    {
        width = std::max(width / 2, 1u);
        height = std::max(height / 2, 1u);
    }
    Key key;
    key.geometry = m_geometry.get();
    key.materials = m_materials.get();
    key.width = width;
    key.height = height;
    key.interacting = interacting;
    key.camera = camera;
    key.settings = settings;
    const bool reset = !m_hasKey || !(key == m_key) || interacting;
    m_targetSamples = std::max(settings.targetSamples, 1u);
    if (!reset && m_samples >= m_targetSamples)
    {
        return;
    }
    m_key = key;
    m_hasKey = true;
    StartPass(width, height, camera, settings, interacting, reset);
}

void MaterialPreviewRenderer::StartPass(
    uint32_t width, uint32_t height, const MaterialPreviewCamera& camera, const MaterialPreviewSettings& settings, bool interacting, bool reset)
{
    if (reset)
    {
        m_samples = 0;
    }
    if (width != m_width || height != m_height)
    {
        m_width = width;
        m_height = height;
        const size_t pixels = static_cast<size_t>(width) * height;
        m_accumulation.assign(pixels, glm::vec3(0.0f));
        m_slots.assign(pixels, -1);
        m_display.assign(pixels * 4, 0);
        m_samples = 0;
    }

    auto task = std::make_unique<PassTask>();
    PassInput& input = task->input;
    input.geometry = m_geometry;
    input.materials = m_materials;
    input.environment = MaterialPreviewEnvironment::Get(settings.environment);
    input.width = width;
    input.height = height;
    input.sample = m_samples;
    // Every interactive pass shows; while samples add up, the image is tone mapped when they double
    // and at the end, which is when it visibly changes.
    const uint32_t next = m_samples + 1;
    input.writeDisplay = interacting || (next & (next - 1)) == 0 || next >= m_targetSamples || next % 16 == 0;
    input.eye = camera.Position();
    input.forward = glm::normalize(camera.target - input.eye);
    const glm::vec3 worldUp = std::abs(input.forward.y) > 0.999f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
    input.right = glm::normalize(glm::cross(input.forward, worldUp));
    input.up = glm::cross(input.right, input.forward);
    input.tanHalfFov = std::tan(camera.verticalFov * 0.5f);
    input.aspect = static_cast<float>(width) / static_cast<float>(height);
    input.pixelSpread = 2.0f * input.tanHalfFov / static_cast<float>(height);

    // The key light, turned as the user turned it.
    const MaterialPreviewEnvironment& environment = *input.environment;
    glm::vec3 key = environment.KeyDirection();
    const float keyYaw = std::atan2(key.x, key.z) + settings.lightYaw;
    const float keyPitch = std::clamp(std::asin(std::clamp(key.y, -1.0f, 1.0f)) + settings.lightPitch, -1.45f, 1.45f);
    key = glm::vec3(std::cos(keyPitch) * std::sin(keyYaw), std::sin(keyPitch), std::cos(keyPitch) * std::cos(keyYaw));
    input.lighting.environment = input.environment.get();
    input.lighting.keyDirection = glm::normalize(key);
    input.lighting.keyIlluminance = environment.KeyIlluminance();
    input.lighting.keySize = std::sin(environment.KeyAngularRadius());
    input.keyCosRadius = std::cos(environment.KeyAngularRadius());
    input.exposure = kBaseExposure * std::exp2(settings.exposureEv);

    float radius = 1.0f;
    glm::vec3 centre(0.0f);
    if (m_geometry && m_geometry->HasBounds())
    {
        centre = 0.5f * (m_geometry->BoundsMin() + m_geometry->BoundsMax());
        radius = std::max(0.5f * glm::length(m_geometry->BoundsMax() - m_geometry->BoundsMin()), 1e-3f);
        input.floor = settings.floor;
        input.floorY = m_geometry->BoundsMin().y - radius * 1e-3f;
        input.sceneMin = m_geometry->BoundsMin();
        input.sceneMax = m_geometry->BoundsMax();
    }
    input.floorAmbient = environment.DiffuseRadiance(glm::vec3(0.0f, 1.0f, 0.0f));
    input.floorCentre = glm::vec2(centre.x, centre.z);
    input.floorFadeStart = radius * 2.0f;
    input.floorFadeEnd = radius * 5.0f;
    // Lines about every tenth of the object, at a round number of metres.
    input.gridSpacing = std::pow(10.0f, std::floor(std::log10(radius * 0.4f)));
    input.aoDistance = radius * 0.25f;
    input.epsilon = std::max(radius * 2e-5f, 1e-5f);
    input.accumulation = m_accumulation.data();
    input.slots = m_slots.data();
    input.display = m_display.data();

    const uint32_t tiles = ((width + kTileSize - 1) / kTileSize) * ((height + kTileSize - 1) / kTileSize);
    task->m_SetSize = tiles;
    task->m_MinRange = 1;
    task->m_Priority = enki::TASK_PRIORITY_LOW;
    m_passStart = std::chrono::steady_clock::now();
    m_task = std::move(task);
    if (TaskSystem::IsRunning() && TaskSystem::CanWaitOnCurrentThread())
    {
        TaskSystem::Scheduler().AddTaskSetToPipe(m_task.get());
    }
    else
    {
        m_task->ExecuteRange(enki::TaskSetPartition{0, tiles}, 0);
        FinishPass();
    }
}

void MaterialPreviewRenderer::FinishPass()
{
    const bool displayed = m_task->input.writeDisplay;
    m_task.reset();
    m_lastPassMilliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_passStart).count();
    ++m_samples;
    if (displayed)
    {
        ComposeFrame();
        m_frame.samples = m_samples;
        ++m_frame.serial;
        m_newFrame = true;
    }
}

void MaterialPreviewRenderer::ComposeFrame()
{
    m_frame.width = m_width;
    m_frame.height = m_height;
    m_frame.rgba = m_display;
    m_frame.slots = m_slots;
    if (m_highlightSlot < 0 || m_width < 3 || m_height < 3)
    {
        return;
    }
    // Unreal's selection outline: the pixels next to the highlighted slot's, outside it, two pixels
    // wide, in the viewport's selection colour.
    const int32_t highlight = m_highlightSlot;
    const int width = static_cast<int>(m_width);
    const int height = static_cast<int>(m_height);
    const int32_t* const slots = m_slots.data();
    uint8_t* const out = m_frame.rgba.data();
    TaskSystem::ParallelFor(
        static_cast<uint32_t>(height),
        16,
        [&](uint32_t begin, uint32_t end)
        {
            for (int y = static_cast<int>(begin); y < static_cast<int>(end); ++y)
            {
                for (int x = 0; x < width; ++x)
                {
                    if (slots[y * width + x] == highlight)
                    {
                        continue;
                    }
                    int neighbours = 0;
                    for (int dy = -2; dy <= 2; ++dy)
                    {
                        for (int dx = -2; dx <= 2; ++dx)
                        {
                            const int sx = x + dx;
                            const int sy = y + dy;
                            if ((dx != 0 || dy != 0) && std::abs(dx) + std::abs(dy) <= 2 && sx >= 0 && sy >= 0 && sx < width && sy < height &&
                                slots[sy * width + sx] == highlight)
                            {
                                ++neighbours;
                            }
                        }
                    }
                    if (neighbours > 0)
                    {
                        uint8_t* pixel = out + (static_cast<size_t>(y) * width + x) * 4;
                        const float weight = neighbours >= 2 ? 1.0f : 0.6f;
                        pixel[0] = static_cast<uint8_t>(pixel[0] + (255 - pixel[0]) * weight);
                        pixel[1] = static_cast<uint8_t>(pixel[1] + (196 - pixel[1]) * weight);
                        pixel[2] = static_cast<uint8_t>(pixel[2] + (64 - pixel[2]) * weight);
                    }
                }
            }
        });
}

void MaterialPreviewRenderer::Finish(uint32_t width, uint32_t height, const MaterialPreviewCamera& camera, const MaterialPreviewSettings& settings)
{
    for (;;)
    {
        if (m_task && TaskSystem::IsRunning())
        {
            TaskSystem::Scheduler().WaitforTask(m_task.get());
        }
        Update(width, height, camera, settings, false);
        if (!m_task && m_samples >= std::max(settings.targetSamples, 1u))
        {
            return;
        }
    }
}
}
