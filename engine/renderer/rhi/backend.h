#pragma once

#include <engine/core/render_backend_type.h>
#include <engine/renderer/render_types.h>
#include <SDL3/SDL_events.h>

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace me
{

struct RenderBackendDescriptor
{
    RenderBackendType type = RenderBackendType::Vulkan;
    const char* name = "Unknown";
    bool isSupported = false;
    const char* unsupportedReason = nullptr;
};

class IRenderBackend
{
  public:
    virtual ~IRenderBackend() = default;

    virtual RenderBackendType GetBackendType() const = 0;
    virtual void HandleEvent(const SDL_Event& event) = 0;
    virtual void DrawFrame() = 0;
    // Writes the viewport of the last drawn frame to a PNG. For verification runs (--capture).
    virtual void CaptureViewport(const std::filesystem::path& path)
    {
        (void)path;
        throw std::runtime_error("This render backend cannot capture the viewport");
    }
    // Compares the DDGI probes' irradiance on every stride-th pixel's surface (Graphics Debug view 15,
    // which the last frame must have shown) with a CPU path tracer's over the same ray scene, and
    // writes prefix_ddgi.pfm, prefix_reference.pfm, prefix_compare.png and prefix_probes.csv. For
    // verification runs (--reference).
    struct DdgiReferenceRequest
    {
        std::filesystem::path prefix;
        uint32_t samples = 256;
        uint32_t stride = 8;
        // A point of the comparison grid (column, row) whose probe lookup is logged probe by probe;
        // negative for none.
        int explainColumn = -1;
        int explainRow = -1;
    };
    virtual void CaptureDdgiReference(const DdgiReferenceRequest& request)
    {
        (void)request;
        throw std::runtime_error("This render backend cannot compare DDGI with a reference");
    }
    // Records the viewport to an MJPEG AVI (Tools > Record Viewport, --record). Its size is the
    // viewport's when it starts, which stays fixed until it stops.
    struct VideoRecordingRequest
    {
        std::filesystem::path path;
        uint32_t framesPerSecond = 30;
        // Every frame drawn is one video frame (scripted runs, whose frames step a fixed time); else
        // the video plays at the speed the frames were shown.
        bool everyFrame = false;
    };
    virtual bool StartVideoRecording(const VideoRecordingRequest& request, std::string& error)
    {
        (void)request;
        error = "This render backend cannot record video";
        return false;
    }
    // Writes the frames still on their way and closes the file. Does nothing when not recording.
    virtual void StopVideoRecording()
    {
    }
    // Films the car being driven (else the selected entity) from four sides at once, its cameras as
    // the Quad Recording window sets them, composed into one video as it goes (Tools > Record Quad
    // Cameras, --quad-record; docs/design/2026-10-07-quad-vehicle-recording-design.md). Fails when
    // there is nothing to film.
    virtual bool StartQuadRecording(const VideoRecordingRequest& request, std::string& error)
    {
        (void)request;
        error = "This render backend cannot record video";
        return false;
    }
    virtual void StopQuadRecording()
    {
    }
    // Photo Mode (docs/design/2026-10-09-photo-mode-design.md): a still from the viewport's camera at
    // width x height, rendered by a view of its own beside the viewport's for warmupFrames frames and
    // then written to path as a PNG. The viewport keeps its size. Fails while another photo renders.
    struct PhotoRequest
    {
        std::filesystem::path path;
        uint32_t width = 3840;
        uint32_t height = 2160;
        uint32_t warmupFrames = 32;
        // The most pixels one view may have before the photo is tiled; 0 decides from the free GPU
        // memory (PhotoMaxViewPixels). For comparing tiled photos with whole ones.
        uint64_t maxViewPixels = 0;
        // How it is rendered (docs/design/2026-10-09-photo-offline-path-tracing-design.md): the path
        // tracer's offline mode, samplesPerPixel a frame until each tile has targetSamples, then
        // warmupFrames more for the resolve to settle; and DLSS of its own at this mode (Off: the
        // engine's TAA), with ray reconstruction where it runs.
        bool offlinePathTracing = false;
        uint32_t samplesPerPixel = 2;
        uint32_t targetSamples = 1024;
        DlssMode dlssMode = DlssMode::Off;
        bool dlssRayReconstruction = false;
    };
    virtual bool TakePhoto(const PhotoRequest& request, std::string& error)
    {
        (void)request;
        error = "This render backend cannot take photos";
        return false;
    }
    // Whether a photo is still rendering or being written.
    virtual bool IsTakingPhoto() const
    {
        return false;
    }
    // Waits for a photo whose tiles are all rendered to be written, and reports it. A photo still
    // rendering is left as it is.
    virtual void WaitForPhotoWrite()
    {
    }
    // Logs the recent frames' average CPU and per-pass GPU times. For verification runs (--frames).
    virtual void LogFrameTimings() const
    {
    }
};
}
