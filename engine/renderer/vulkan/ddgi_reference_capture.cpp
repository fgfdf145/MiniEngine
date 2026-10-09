// VulkanRenderer::CaptureDdgiReference: the DDGI probes against a CPU path tracer, step 6 of
// docs/design/2026-09-27-ddgi-design.md. Kept apart from renderer.cpp, which is long enough.

#include "renderer.h"
#include "reverse_depth.h"
#include "viewport_capture.h"

#include <engine/core/log/log.h>
#include <engine/core/threading/task_system.h>
#include <engine/renderer/ddgi_volume.h>
#include <engine/renderer/reference_path_tracer.h>

#include <stb_image_write.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace me
{

namespace
{
constexpr glm::vec3 kLuma(0.2126f, 0.7152f, 0.0722f);

// Runs work once on each of the task system's active threads at the same time; work takes its share
// of the job from a counter it shares with the others.
template <typename Work>
void RunOnEveryTaskThread(const Work& work)
{
    TaskSystem::ParallelFor(TaskSystem::ActiveThreadCount(), 1, [&](uint32_t begin, uint32_t end)
                            {
                                for (uint32_t share = begin; share < end; ++share)
                                {
                                    work();
                                }
                            });
}

// Portable Float Map, RGB, rows from the bottom as the format has them.
void WritePfm(const std::filesystem::path& path, const std::vector<glm::vec3>& pixels, uint32_t width, uint32_t height)
{
    std::ofstream file(path, std::ios::binary);
    if (!file)
    {
        throw std::runtime_error("Failed to write '" + path.string() + "'");
    }
    file << "PF\n"
         << width << ' ' << height << "\n-1.0\n";
    for (uint32_t row = height; row-- > 0;)
    {
        file.write(reinterpret_cast<const char*>(&pixels[static_cast<size_t>(row) * width]), static_cast<std::streamsize>(width * sizeof(glm::vec3)));
    }
}

uint8_t EncodeSrgb(float linear)
{
    const float clamped = std::clamp(linear, 0.0f, 1.0f);
    const float encoded = clamped <= 0.0031308f ? clamped * 12.92f : 1.055f * std::pow(clamped, 1.0f / 2.4f) - 0.055f;
    return static_cast<uint8_t>(std::lround(encoded * 255.0f));
}
}

// DdgiIrradiance of ddgi_common.slang redone on the CPU for one surface point from the read-back
// probes, every level's every probe logged with its weights: where a lookup goes wrong, this says
// which probe and which term (--reference-explain).
void VulkanRenderer::ExplainDdgiLookup(const ImageCaptureRequest& device, glm::vec3 P, glm::vec3 N, glm::vec3 V)
{
    const ReferenceFrame& frame = m_referenceFrame;
    const std::vector<uint8_t> stateBytes =
        ReadBufferBytes(device, m_ddgi->GetProbeStateHandle(), static_cast<size_t>(kDdgiProbeStateBytes) * kDdgiProbesPerLevel * kDdgiMaxLevels);
    constexpr int kIrrTile = kDdgiIrradianceTexels + 2;
    constexpr int kVisTile = kDdgiVisibilityTexels + 2;
    const glm::ivec2 irrSize(kIrrTile * kDdgiGridSize.x * kDdgiGridSize.y, kIrrTile * kDdgiGridSize.z);
    const glm::ivec2 visSize(kVisTile * kDdgiGridSize.x * kDdgiGridSize.y, kVisTile * kDdgiGridSize.z);
    std::vector<std::vector<glm::vec4>> irr(kDdgiMaxLevels);
    std::vector<std::vector<glm::vec4>> vis(kDdgiMaxLevels);
    for (uint32_t level = 0; level < frame.ddgiLevels; ++level)
    {
        ImageCaptureRequest request = device;
        request.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        request.layer = level;
        request.texture = m_ddgi->GetIrradianceTexture();
        request.extent = {static_cast<uint32_t>(irrSize.x), static_cast<uint32_t>(irrSize.y)};
        irr[level] = ReadImageHalfFloats(request);
        request.texture = m_ddgi->GetVisibilityTexture();
        request.format = VK_FORMAT_R16G16_SFLOAT;
        request.extent = {static_cast<uint32_t>(visSize.x), static_cast<uint32_t>(visSize.y)};
        vis[level] = ReadImageHalfFloats(request);
    }
    const auto bilinear = [](const std::vector<glm::vec4>& image, glm::ivec2 size, glm::vec2 uv)
    {
        const glm::vec2 pixel = uv * glm::vec2(size) - 0.5f;
        const glm::ivec2 base = glm::ivec2(glm::floor(pixel));
        const glm::vec2 f = pixel - glm::vec2(base);
        const auto at = [&](int x, int y)
        {
            x = std::clamp(x, 0, size.x - 1);
            y = std::clamp(y, 0, size.y - 1);
            return image[static_cast<size_t>(y) * size.x + x];
        };
        return glm::mix(glm::mix(at(base.x, base.y), at(base.x + 1, base.y), f.x), glm::mix(at(base.x, base.y + 1), at(base.x + 1, base.y + 1), f.x), f.y);
    };
    const auto octEncode = [](glm::vec3 d)
    {
        d /= std::abs(d.x) + std::abs(d.y) + std::abs(d.z);
        glm::vec2 e(d.x, d.z);
        if (d.y < 0.0f)
        {
            e = (1.0f - glm::abs(glm::vec2(e.y, e.x))) * glm::vec2(e.x >= 0.0f ? 1.0f : -1.0f, e.y >= 0.0f ? 1.0f : -1.0f);
        }
        return e;
    };
    const auto atlasUv = [&](glm::ivec3 slot, glm::vec3 direction, int texels, glm::ivec2 size)
    {
        const glm::vec2 corner = glm::vec2(glm::ivec2(slot.x + kDdgiGridSize.x * slot.y, slot.z) * (texels + 2) + 1);
        const glm::vec2 inside = (octEncode(direction) * 0.5f + 0.5f) * static_cast<float>(texels);
        return (corner + inside) / glm::vec2(size);
    };
    LOG_INFO("EXPLAIN P ({:.3f},{:.3f},{:.3f}) N ({:.3f},{:.3f},{:.3f}) V ({:.3f},{:.3f},{:.3f})", P.x, P.y, P.z, N.x, N.y, N.z, V.x, V.y, V.z);
    float remaining = 1.0f;
    glm::vec4 result(0.0f);
    for (uint32_t level = 0; level < frame.ddgiLevels && remaining > 1e-3f; ++level)
    {
        const float spacing = frame.ddgiSpacings[level];
        const glm::ivec3 origin = frame.ddgiOrigins[level];
        // DdgiIrradianceAlong's fade: by the distance to the camera.
        const glm::vec3 fromCamera = glm::abs(P - frame.cameraPosition) / spacing;
        const glm::vec3 toEdge = (glm::vec3(kDdgiGridSize / 2 - 1) - 0.5f - fromCamera) /
                                 glm::vec3(kDdgiFadeCellsHorizontal, kDdgiFadeCellsVertical, kDdgiFadeCellsHorizontal);
        const float fade = std::clamp(std::min(std::min(toEdge.x, toEdge.y), toEdge.z), 0.0f, 1.0f);
        if (fade <= 0.0f)
        {
            LOG_INFO("EXPLAIN level {} spacing {} origin ({},{},{}): outside (fade 0)", level, spacing, origin.x, origin.y, origin.z);
            continue;
        }
        const glm::vec3 biased = P + (N * frame.ddgiBias.x + V * frame.ddgiBias.y) * spacing;
        const glm::vec3 gridPosition = biased / spacing - glm::vec3(origin);
        const glm::ivec3 base = glm::ivec3(glm::floor(gridPosition));
        const glm::vec3 alpha = gridPosition - glm::vec3(base);
        glm::vec4 sum(0.0f);
        float total = 0.0f;
        float coverage = 0.0f;
        for (int corner = 0; corner < 8; ++corner)
        {
            const glm::ivec3 offset(corner & 1, (corner >> 1) & 1, (corner >> 2) & 1);
            const glm::ivec3 coord = origin + base + offset;
            const glm::ivec3 slot = DdgiStorageSlot(coord);
            const size_t stateIndex = level * kDdgiProbesPerLevel + DdgiSlotIndex(slot);
            glm::ivec4 flags;
            glm::vec4 relocation;
            std::memcpy(&flags, stateBytes.data() + stateIndex * kDdgiProbeStateBytes, sizeof(flags));
            std::memcpy(&relocation, stateBytes.data() + stateIndex * kDdgiProbeStateBytes + 16, sizeof(relocation));
            const glm::vec3 trilinear3 = glm::mix(1.0f - alpha, alpha, glm::vec3(offset));
            const float trilinear = trilinear3.x * trilinear3.y * trilinear3.z;
            const bool stale = glm::ivec3(flags) != coord || (flags.w & 1) == 0;
            const bool inactive = (flags.w & 2) != 0;
            const glm::vec3 probe = glm::vec3(coord) * spacing + glm::vec3(relocation);
            if (stale)
            {
                LOG_INFO("EXPLAIN  L{} probe ({},{},{}) STALE tri {:.3f}", level, coord.x, coord.y, coord.z, trilinear);
                continue;
            }
            coverage += trilinear;
            const glm::vec3 toProbe = glm::normalize(probe - P);
            float weight = (glm::dot(toProbe, N) + 1.0f) * 0.5f;
            weight = weight * weight + 0.2f;
            const float backWeight = weight;
            const glm::vec3 probeToPoint = biased - probe;
            const float distance = glm::length(probeToPoint);
            const glm::vec4 moments = bilinear(vis[level], visSize, atlasUv(slot, probeToPoint / std::max(distance, 1e-4f), kDdgiVisibilityTexels, visSize));
            float chebyshev = 1.0f;
            if (distance > moments.x)
            {
                const float maxDeviation = 0.05f * spacing;
                const float variance = std::min(std::abs(moments.x * moments.x - moments.y), maxDeviation * maxDeviation);
                const float excess = distance - moments.x;
                chebyshev = variance / (variance + excess * excess);
                chebyshev = chebyshev * chebyshev * chebyshev;
            }
            weight *= std::max(chebyshev, 0.05f);
            weight = std::max(weight, 1e-6f);
            if (weight < 0.2f)
            {
                weight *= weight * weight / 0.04f;
            }
            weight *= trilinear;
            const glm::vec4 value = bilinear(irr[level], irrSize, atlasUv(slot, N, kDdgiIrradianceTexels, irrSize));
            LOG_INFO(
                "EXPLAIN  L{} probe ({},{},{}) at ({:.2f},{:.2f},{:.2f}) {} tri {:.3f} back {:.3f} dist {:.2f} mean {:.2f} cheb {:.4f} -> w {:.5f} value {:.1f}",
                level, coord.x, coord.y, coord.z, probe.x, probe.y, probe.z, inactive ? "INACTIVE" : "active", trilinear, backWeight, distance,
                moments.x, chebyshev, weight, glm::dot(glm::vec3(value), kLuma));
            if (inactive)
            {
                continue;
            }
            sum += value * weight;
            total += weight;
        }
        const glm::vec4 value = total > 0.0f ? sum / total : glm::vec4(0.0f);
        if (total <= 0.0f)
        {
            coverage = 0.0f;
        }
        const float levelWeight = fade * std::clamp(coverage, 0.0f, 1.0f);
        LOG_INFO("EXPLAIN level {} spacing {}: value {:.1f} coverage {:.3f} fade {:.3f} levelWeight {:.3f}", level, spacing, glm::dot(glm::vec3(value), kLuma), coverage, fade, levelWeight);
        result += value * levelWeight * remaining;
        remaining *= 1.0f - levelWeight;
    }
    LOG_INFO("EXPLAIN result {:.1f} weight {:.3f}", glm::dot(glm::vec3(result), kLuma), 1.0f - remaining);
}

void VulkanRenderer::CompareDdgiProbes(const std::filesystem::path& prefix, const ImageCaptureRequest& device, uint32_t samples)
{
    const ReferenceFrame& frame = m_referenceFrame;
    // The finest level's probe records and irradiance tiles.
    const std::vector<uint8_t> stateBytes =
        ReadBufferBytes(device, m_ddgi->GetProbeStateHandle(), static_cast<size_t>(kDdgiProbeStateBytes) * kDdgiProbesPerLevel);
    ImageCaptureRequest atlasRequest = device;
    atlasRequest.texture = m_ddgi->GetIrradianceTexture();
    atlasRequest.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    constexpr uint32_t kTile = kDdgiIrradianceTexels + 2;
    atlasRequest.extent = {kTile * static_cast<uint32_t>(kDdgiGridSize.x * kDdgiGridSize.y), kTile * static_cast<uint32_t>(kDdgiGridSize.z)};
    const std::vector<glm::vec4> atlas = ReadImageHalfFloats(atlasRequest);

    struct Probe
    {
        glm::vec3 position{0.0f};
        glm::ivec3 slot{0};
        float distance = 0.0f;
    };
    std::vector<Probe> probes;
    for (uint32_t index = 0; index < kDdgiProbesPerLevel; ++index)
    {
        glm::ivec4 coordAndFlags;
        glm::vec4 offset;
        std::memcpy(&coordAndFlags, stateBytes.data() + static_cast<size_t>(index) * kDdgiProbeStateBytes, sizeof(coordAndFlags));
        std::memcpy(&offset, stateBytes.data() + static_cast<size_t>(index) * kDdgiProbeStateBytes + 16, sizeof(offset));
        const glm::ivec3 slot = DdgiSlotFromIndex(index);
        const glm::ivec3 coord = DdgiSlotCoordinate(slot, frame.ddgiOrigins[0]);
        // Updated for where it is, and active (DDGI_PROBE_UPDATED, DDGI_PROBE_INACTIVE).
        if (glm::ivec3(coordAndFlags) != coord || (coordAndFlags.w & 1) == 0 || (coordAndFlags.w & 2) != 0)
        {
            continue;
        }
        const glm::vec3 position = glm::vec3(coord) * frame.ddgiSpacings[0] + glm::vec3(offset);
        probes.push_back(Probe{position, slot, glm::length(position - frame.cameraPosition)});
    }
    // The nearest ones: the rest cost time and tell the same story.
    std::sort(probes.begin(), probes.end(), [](const Probe& a, const Probe& b)
              {
                  return a.distance < b.distance;
              });
    probes.resize(std::min<size_t>(probes.size(), 300));

    // Per probe, the texels whose directions lie nearest the six axes: TexelDirection in
    // ddgi_update.comp, DdgiOctDecode in ddgi_common.slang.
    const auto octDecode = [](glm::vec2 encoded)
    {
        glm::vec3 direction(encoded.x, 1.0f - std::abs(encoded.x) - std::abs(encoded.y), encoded.y);
        if (direction.y < 0.0f)
        {
            const float x = (1.0f - std::abs(direction.z)) * (direction.x >= 0.0f ? 1.0f : -1.0f);
            const float z = (1.0f - std::abs(direction.x)) * (direction.z >= 0.0f ? 1.0f : -1.0f);
            direction.x = x;
            direction.z = z;
        }
        return glm::normalize(direction);
    };
    const std::array<glm::vec3, 6> axes = {
        glm::vec3(1, 0, 0), glm::vec3(-1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(0, -1, 0), glm::vec3(0, 0, 1), glm::vec3(0, 0, -1)};
    std::array<glm::ivec2, 6> texels{};
    std::array<glm::vec3, 6> directions{};
    for (size_t axis = 0; axis < axes.size(); ++axis)
    {
        float best = -2.0f;
        for (int y = 0; y < static_cast<int>(kDdgiIrradianceTexels); ++y)
        {
            for (int x = 0; x < static_cast<int>(kDdgiIrradianceTexels); ++x)
            {
                const glm::vec3 direction = octDecode((glm::vec2(x, y) + 0.5f) / static_cast<float>(kDdgiIrradianceTexels) * 2.0f - 1.0f);
                if (glm::dot(direction, axes[axis]) > best)
                {
                    best = glm::dot(direction, axes[axis]);
                    texels[axis] = glm::ivec2(x, y);
                    directions[axis] = direction;
                }
            }
        }
    }

    const RayScene scene = m_rayScene->CopyCpuScene();
    const std::vector<ReferenceMaterial> materials = m_rayScene->ReadMaterials();
    ReferenceSettings settings;
    settings.samples = samples;
    settings.skyRadiance = frame.skyRadiance;
    std::vector<std::array<glm::vec3, 6>> stored(probes.size());
    std::vector<std::array<glm::vec3, 6>> truth(probes.size());
    std::atomic<size_t> next{0};
    const auto work = [&]()
    {
        for (size_t index = next++; index < probes.size(); index = next++)
        {
            const glm::ivec2 tile(probes[index].slot.x + kDdgiGridSize.x * probes[index].slot.y, probes[index].slot.z);
            for (size_t axis = 0; axis < axes.size(); ++axis)
            {
                const glm::ivec2 texel = tile * static_cast<int>(kTile) + 1 + texels[axis];
                stored[index][axis] = glm::vec3(atlas[static_cast<size_t>(texel.y) * atlasRequest.extent.width + static_cast<size_t>(texel.x)]);
                truth[index][axis] = ReferenceIndirectIrradiance(
                    scene, materials, frame.lights, probes[index].position, directions[axis], settings, index * 8 + axis + 1);
            }
        }
    };
    RunOnEveryTaskThread(work);

    std::ofstream csv(prefix.string() + "_probes.csv");
    csv << "x,y,z,axis,probe,reference\n";
    std::array<double, 6> storedSum{};
    std::array<double, 6> truthSum{};
    for (size_t index = 0; index < probes.size(); ++index)
    {
        for (size_t axis = 0; axis < axes.size(); ++axis)
        {
            const float probe = glm::dot(stored[index][axis], kLuma);
            const float reference = glm::dot(truth[index][axis], kLuma);
            storedSum[axis] += probe;
            truthSum[axis] += reference;
            csv << probes[index].position.x << ',' << probes[index].position.y << ',' << probes[index].position.z << ',' << axis << ','
                << probe << ',' << reference << '\n';
        }
    }
    const auto ratio = [&](size_t axis)
    {
        return storedSum[axis] / std::max(truthSum[axis], 1e-9);
    };
    LOG_INFO(
        "DDGI probes against the reference ({} nearest probes): probes / reference along +x {:.3f}, -x {:.3f}, +y {:.3f}, -y {:.3f}, "
        "+z {:.3f}, -z {:.3f}",
        probes.size(),
        ratio(0),
        ratio(1),
        ratio(2),
        ratio(3),
        ratio(4),
        ratio(5));
}

void VulkanRenderer::CaptureDdgiReferenceNow(const DdgiReferenceRequest& reference)
{
    const std::filesystem::path& prefix = reference.prefix;
    const uint32_t samples = std::max(reference.samples, 1u);
    const uint32_t stride = std::max(reference.stride, 1u);
    const ReferenceFrame& frame = m_referenceFrame;
    if (!m_lastRecordedImageIndex.has_value() || !m_view.targets)
    {
        throw std::runtime_error("No frame has been drawn to compare");
    }
    // A path traced frame is checked the same way: SceneGi holds its diffuse light, demodulated, which
    // is the irradiance / pi the reference computes.
    const bool pathTraced = frame.pathTraced;
    const char* const subject = pathTraced ? "path tracer" : "probes";
    if (!pathTraced && (frame.view != GBufferDebugView::DdgiIrradiance || !frame.ddgiEnabled))
    {
        throw std::runtime_error(
            "The DDGI reference needs the last frame to show the DDGI irradiance view (--debug-view 15) with DDGI on, or to be path traced");
    }
    if (!frame.uniformSky)
    {
        // The path tracer's sky is one radiance: under a physical sky or an HDRI it would compare
        // against a different sky, so the comparison scenes use environment None.
        LOG_WARN("The DDGI reference takes the sky as the uniform ambient; this scene's sky is not, so the comparison is off");
    }
    for (const ReferenceLight& light : frame.lights)
    {
        LOG_INFO(
            "DDGI reference light: toward ({:.3f}, {:.3f}, {:.3f}), illuminance ({:.0f}, {:.0f}, {:.0f}) lux",
            light.directionToLight.x,
            light.directionToLight.y,
            light.directionToLight.z,
            light.illuminance.r,
            light.illuminance.g,
            light.illuminance.b);
    }
    m_nvrhi->Get()->waitForIdle();

    // What the probes sent each pixel: view 15 wrote irradiance / pi, pre-exposed, into SceneGi.
    ImageCaptureRequest request{};
    request.device = m_nvrhi->Get();
    request.texture = m_view.targets->GetTexture(
        RenderTargetId::SceneGi,
        m_view.targets->ResolveIndex(RenderTargetId::SceneGi, *m_lastRecordedImageIndex, frame.frameSlot));
    request.format = m_view.targets->GetFormat(RenderTargetId::SceneGi);
    request.extent = m_view.targets->GetExtent();
    const std::vector<glm::vec4> texels = ReadImageHalfFloats(request);

    const RayScene scene = m_rayScene->CopyCpuScene();
    const std::vector<ReferenceMaterial> materials = m_rayScene->ReadMaterials();
    ReferenceSettings settings;
    settings.samples = samples;
    settings.skyRadiance = frame.skyRadiance;

    const uint32_t width = (request.extent.width + stride - 1) / stride;
    const uint32_t height = (request.extent.height + stride - 1) / stride;
    std::vector<glm::vec3> probes(static_cast<size_t>(width) * height, glm::vec3(0.0f));
    std::vector<glm::vec3> truth(probes.size(), glm::vec3(0.0f));
    std::vector<uint8_t> valid(probes.size(), 0u);
    const glm::mat4 inverseViewProjection = glm::inverse(frame.viewProjection);

    const auto started = std::chrono::steady_clock::now();
    std::atomic<uint32_t> nextRow{0};
    std::mutex failureMutex;
    std::string failure;
    const auto work = [&]()
    {
        try
        {
            for (uint32_t row = nextRow++; row < height; row = nextRow++)
            {
                for (uint32_t column = 0; column < width; ++column)
                {
                    const uint32_t x = std::min(column * stride + stride / 2, request.extent.width - 1);
                    const uint32_t y = std::min(row * stride + stride / 2, request.extent.height - 1);
                    // The image's top row is ndc.y == -1, the projection's Y flip being inside it
                    // (deferred_lighting.frag); depth runs from the far plane at 0 to the near one at 1.
                    const glm::vec2 ndc = (glm::vec2(x, y) + 0.5f) / glm::vec2(request.extent.width, request.extent.height) * 2.0f - 1.0f;
                    const glm::vec4 far = inverseViewProjection * glm::vec4(ndc, kReverseDepthFar, 1.0f);
                    const glm::vec3 direction = glm::normalize(glm::vec3(far) / far.w - frame.cameraPosition);
                    const ReferenceSurface surface = ReferencePrimaryHit(scene, materials, frame.cameraPosition, direction);
                    const size_t index = static_cast<size_t>(row) * width + column;
                    probes[index] = glm::vec3(texels[static_cast<size_t>(y) * request.extent.width + x]) / frame.preExposure;
                    if (!surface.valid)
                    {
                        continue;
                    }
                    valid[index] = 1u;
                    truth[index] = ReferenceIndirectIrradiance(scene, materials, frame.lights, surface.position, surface.normal, settings, index + 1);
                }
            }
        }
        catch (const std::exception& error)
        {
            const std::lock_guard lock(failureMutex);
            failure = error.what();
            nextRow = height;
        }
    };
    RunOnEveryTaskThread(work);
    if (!failure.empty())
    {
        throw std::runtime_error("The reference path tracer failed: " + failure);
    }
    const float seconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - started).count();

    // How far the probes are from the reference, by luminance, over the surfaces both see: the ratio
    // of the sums (the overall bias) and the relative error per point, against the reference or a
    // twentieth of its mean where it is darker, so black corners do not dominate.
    double probeSum = 0.0;
    double referenceSum = 0.0;
    size_t count = 0;
    for (size_t index = 0; index < probes.size(); ++index)
    {
        if (valid[index] != 0u)
        {
            probeSum += glm::dot(probes[index], kLuma);
            referenceSum += glm::dot(truth[index], kLuma);
            ++count;
        }
    }
    if (count == 0 || referenceSum <= 0.0)
    {
        throw std::runtime_error("No surface on screen receives indirect light to compare");
    }
    const float referenceMean = static_cast<float>(referenceSum / static_cast<double>(count));
    std::vector<float> errors;
    errors.reserve(count);
    for (size_t index = 0; index < probes.size(); ++index)
    {
        if (valid[index] != 0u)
        {
            const float probe = glm::dot(probes[index], kLuma);
            const float expected = glm::dot(truth[index], kLuma);
            errors.push_back(std::abs(probe - expected) / std::max(expected, 0.05f * referenceMean));
        }
    }
    std::sort(errors.begin(), errors.end());
    const auto percentile = [&](float share)
    {
        return errors[std::min(static_cast<size_t>(share * static_cast<float>(errors.size())), errors.size() - 1)];
    };
    const size_t within = static_cast<size_t>(std::upper_bound(errors.begin(), errors.end(), 0.25f) - errors.begin());

    WritePfm(prefix.string() + "_ddgi.pfm", probes, width, height);
    WritePfm(prefix.string() + "_reference.pfm", truth, width, height);

    // Probes, reference and their ratio side by side. Both images share one mapping, v / (v + mean),
    // so equal values look equal; the ratio shows red where the probes are brighter, blue where
    // darker, full at a factor of two.
    const uint32_t panelWidth = width * 3;
    std::vector<uint8_t> pixels(static_cast<size_t>(panelWidth) * height * 3, 0u);
    for (uint32_t row = 0; row < height; ++row)
    {
        for (uint32_t column = 0; column < width; ++column)
        {
            const size_t index = static_cast<size_t>(row) * width + column;
            const auto put = [&](uint32_t panel, glm::vec3 colour)
            {
                uint8_t* pixel = &pixels[(static_cast<size_t>(row) * panelWidth + panel * width + column) * 3];
                pixel[0] = EncodeSrgb(colour.r);
                pixel[1] = EncodeSrgb(colour.g);
                pixel[2] = EncodeSrgb(colour.b);
            };
            put(0, probes[index] / (probes[index] + referenceMean));
            put(1, truth[index] / (truth[index] + referenceMean));
            if (valid[index] != 0u)
            {
                const float probe = glm::dot(probes[index], kLuma);
                const float expected = glm::dot(truth[index], kLuma);
                const float stops = std::clamp(std::log2(std::max(probe, 1e-6f * referenceMean) / std::max(expected, 1e-6f * referenceMean)), -1.0f, 1.0f);
                put(2, stops > 0.0f ? glm::vec3(stops, 0.0f, 0.0f) : glm::vec3(0.0f, 0.0f, -stops));
            }
        }
    }
    const std::string comparePath = prefix.string() + "_compare.png";
    if (stbi_write_png(comparePath.c_str(), static_cast<int>(panelWidth), static_cast<int>(height), 3, pixels.data(), static_cast<int>(panelWidth) * 3) == 0)
    {
        throw std::runtime_error("Failed to write '" + comparePath + "'");
    }

    if (!pathTraced)
    {
        CompareDdgiProbes(prefix, request, samples);
    }
    {
        const int column = reference.explainColumn;
        const int row = reference.explainRow;
        if (column >= 0 && row >= 0 && static_cast<uint32_t>(column) < width && static_cast<uint32_t>(row) < height)
        {
            const size_t index = static_cast<size_t>(row) * width + column;
            const uint32_t x = std::min(static_cast<uint32_t>(column) * stride + stride / 2, request.extent.width - 1);
            const uint32_t y = std::min(static_cast<uint32_t>(row) * stride + stride / 2, request.extent.height - 1);
            const glm::vec2 ndc = (glm::vec2(x, y) + 0.5f) / glm::vec2(request.extent.width, request.extent.height) * 2.0f - 1.0f;
            const glm::vec4 far = inverseViewProjection * glm::vec4(ndc, kReverseDepthFar, 1.0f);
            const glm::vec3 direction = glm::normalize(glm::vec3(far) / far.w - frame.cameraPosition);
            const ReferenceSurface surface = ReferencePrimaryHit(scene, materials, frame.cameraPosition, direction);
            LOG_INFO("EXPLAIN grid ({},{}): GPU probes {:.1f}, reference {:.1f}", column, row, glm::dot(probes[index], kLuma), glm::dot(truth[index], kLuma));
            ExplainDdgiLookup(request, surface.position, surface.normal, glm::normalize(frame.cameraPosition - surface.position));
        }
    }

    LOG_INFO(
        "{} against the reference ({} points, {} paths each, {:.1f} s): {} / reference {:.3f}; relative error median {:.3f}, "
        "90th percentile {:.3f}; {:.1f}% within 25%. Written to '{}'",
        pathTraced ? "Path tracing" : "DDGI",
        count,
        samples,
        seconds,
        subject,
        probeSum / referenceSum,
        percentile(0.5f),
        percentile(0.9f),
        100.0 * static_cast<double>(within) / static_cast<double>(count),
        comparePath);
}
}
