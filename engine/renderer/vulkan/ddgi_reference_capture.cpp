// VulkanRenderer::CaptureDdgiReference: the DDGI probes against a CPU path tracer, step 6 of
// docs/design/2026-09-27-ddgi-design.md. Kept apart from renderer.cpp, which is long enough.

#include "renderer.h"
#include "viewport_capture.h"

#include <engine/core/log/log.h>
#include <engine/renderer/reference_path_tracer.h>

#include <stb_image_write.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace me
{

namespace
{
constexpr glm::vec3 kLuma(0.2126f, 0.7152f, 0.0722f);

// Portable Float Map, RGB, rows from the bottom as the format has them.
void WritePfm(const std::filesystem::path& path, const std::vector<glm::vec3>& pixels, uint32_t width, uint32_t height)
{
    std::ofstream file(path, std::ios::binary);
    if (!file)
    {
        throw std::runtime_error("Failed to write '" + path.string() + "'");
    }
    file << "PF\n" << width << ' ' << height << "\n-1.0\n";
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

void VulkanRenderer::CaptureDdgiReference(const std::filesystem::path& prefix, uint32_t samples, uint32_t stride)
{
    const ReferenceFrame& frame = m_referenceFrame;
    if (!m_lastRecordedImageIndex.has_value() || !m_sceneTargets)
    {
        throw std::runtime_error("No frame has been drawn to compare");
    }
    if (frame.view != GBufferDebugView::DdgiIrradiance || !frame.ddgiEnabled)
    {
        throw std::runtime_error("The DDGI reference needs the last frame to show the DDGI irradiance view (--debug-view 15) with DDGI on");
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
    stride = std::max(stride, 1u);
    samples = std::max(samples, 1u);
    vkDeviceWaitIdle(m_device->GetHandle());

    // What the probes sent each pixel: view 15 wrote irradiance / pi, pre-exposed, into SceneGi.
    ImageCaptureRequest request{};
    request.physicalDevice = m_device->GetPhysicalDevice();
    request.device = m_device->GetHandle();
    request.queueFamily = m_device->GetQueueFamilies().graphicsFamily.value();
    request.queue = m_device->GetGraphicsQueue();
    request.image = m_sceneTargets->GetImage(
        RenderTargetId::SceneGi,
        m_sceneTargets->ResolveIndex(RenderTargetId::SceneGi, *m_lastRecordedImageIndex, frame.frameSlot));
    request.format = m_sceneTargets->GetFormat(RenderTargetId::SceneGi);
    request.extent = m_sceneTargets->GetExtent();
    request.layout = m_layoutTracker.GetLayout(RenderTargetId::SceneGi);
    if (request.layout == VK_IMAGE_LAYOUT_UNDEFINED)
    {
        throw std::runtime_error("The last frame did not write the DDGI irradiance");
    }
    const std::vector<glm::vec4> texels = ReadImageHalfFloats(request);

    const RayScene scene = m_rayScene->CopyCpuScene();
    const std::vector<ReferenceMaterial> materials = m_rayScene->ReadMaterials();
    ReferenceSettings settings;
    settings.samples = samples;
    settings.skyRadiance = frame.skyRadiance;

    const uint32_t width = (request.extent.width + stride - 1) / stride;
    const uint32_t height = (request.extent.height + stride - 1) / stride;
    std::vector<glm::vec3> probes(static_cast<size_t>(width) * height, glm::vec3(0.0f));
    std::vector<glm::vec3> reference(probes.size(), glm::vec3(0.0f));
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
                // (deferred_lighting.frag); depth runs 0 to 1.
                const glm::vec2 ndc = (glm::vec2(x, y) + 0.5f) / glm::vec2(request.extent.width, request.extent.height) * 2.0f - 1.0f;
                const glm::vec4 far = inverseViewProjection * glm::vec4(ndc, 1.0f, 1.0f);
                const glm::vec3 direction = glm::normalize(glm::vec3(far) / far.w - frame.cameraPosition);
                const ReferenceSurface surface = ReferencePrimaryHit(scene, materials, frame.cameraPosition, direction);
                const size_t index = static_cast<size_t>(row) * width + column;
                probes[index] = glm::vec3(texels[static_cast<size_t>(y) * request.extent.width + x]) / frame.preExposure;
                if (!surface.valid)
                {
                    continue;
                }
                valid[index] = 1u;
                reference[index] = ReferenceIndirectIrradiance(scene, materials, frame.lights, surface.position, surface.normal, settings, index + 1);
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
    std::vector<std::thread> threads;
    const uint32_t threadCount = std::max(std::thread::hardware_concurrency(), 1u);
    for (uint32_t thread = 0; thread < threadCount; ++thread)
    {
        threads.emplace_back(work);
    }
    for (std::thread& thread : threads)
    {
        thread.join();
    }
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
            referenceSum += glm::dot(reference[index], kLuma);
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
            const float truth = glm::dot(reference[index], kLuma);
            errors.push_back(std::abs(probe - truth) / std::max(truth, 0.05f * referenceMean));
        }
    }
    std::sort(errors.begin(), errors.end());
    const auto percentile = [&](float share)
    {
        return errors[std::min(static_cast<size_t>(share * static_cast<float>(errors.size())), errors.size() - 1)];
    };
    const size_t within = static_cast<size_t>(std::upper_bound(errors.begin(), errors.end(), 0.25f) - errors.begin());

    WritePfm(prefix.string() + "_ddgi.pfm", probes, width, height);
    WritePfm(prefix.string() + "_reference.pfm", reference, width, height);

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
            put(1, reference[index] / (reference[index] + referenceMean));
            if (valid[index] != 0u)
            {
                const float probe = glm::dot(probes[index], kLuma);
                const float truth = glm::dot(reference[index], kLuma);
                const float stops = std::clamp(std::log2(std::max(probe, 1e-6f * referenceMean) / std::max(truth, 1e-6f * referenceMean)), -1.0f, 1.0f);
                put(2, stops > 0.0f ? glm::vec3(stops, 0.0f, 0.0f) : glm::vec3(0.0f, 0.0f, -stops));
            }
        }
    }
    const std::string comparePath = prefix.string() + "_compare.png";
    if (stbi_write_png(comparePath.c_str(), static_cast<int>(panelWidth), static_cast<int>(height), 3, pixels.data(), static_cast<int>(panelWidth) * 3) == 0)
    {
        throw std::runtime_error("Failed to write '" + comparePath + "'");
    }

    LOG_INFO(
        "DDGI against the reference ({} points, {} paths each, {:.1f} s): probes / reference {:.3f}; relative error median {:.3f}, "
        "90th percentile {:.3f}; {:.1f}% within 25%. Written to '{}'",
        count,
        samples,
        seconds,
        probeSum / referenceSum,
        percentile(0.5f),
        percentile(0.9f),
        100.0 * static_cast<double>(within) / static_cast<double>(count),
        comparePath);
}
}
