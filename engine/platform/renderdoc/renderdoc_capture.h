#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace me::platform::renderdoc
{

// RenderDoc's in-application API (third_party/renderdoc/renderdoc_app.h): frame captures the engine
// asks for itself (--renderdoc, the control channel's renderdoc.* commands), without the RenderDoc UI.

// Loads RenderDoc into the process: the copy already there when RenderDoc launched the engine, else
// MINIENGINE_RENDERDOC_DLL or the installed C:/Program Files/RenderDoc/renderdoc.dll. Must run before
// the graphics device is made, which RenderDoc hooks then. Captures go to `captureFolder`; its overlay
// is switched off. False with `error` saying why.
bool Load(const std::filesystem::path& captureFolder, std::string& error);
bool IsLoaded();

// Captures the next `frames` frames presented, into one capture file.
void TriggerCapture(uint32_t frames);
bool IsCapturing();

struct CaptureFile
{
    std::filesystem::path path;
    uint64_t timestamp = 0;
};
// Every capture made in this run, oldest first.
std::vector<CaptureFile> Captures();

// Opens the RenderDoc UI on a capture (connected to this process when `connect`).
bool OpenInUi(const std::filesystem::path& capture, bool connect);
}
