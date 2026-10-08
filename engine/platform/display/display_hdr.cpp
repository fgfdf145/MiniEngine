#include "display_hdr.h"

#include <SDL3/SDL.h>

#include <chrono>
#include <optional>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <vector>
#endif

namespace me
{

namespace platform::display
{
#if defined(_WIN32)
namespace
{
// Windows' SDR content brightness, in cd/m^2, for the display whose GDI name is gdiName (the DXGI
// output's DeviceName); nullopt when the display configuration does not list it.
std::optional<float> QuerySdrWhiteNits(const wchar_t* gdiName)
{
    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
    {
        return std::nullopt;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS)
    {
        return std::nullopt;
    }
    for (UINT32 index = 0; index < pathCount; ++index)
    {
        const DISPLAYCONFIG_PATH_INFO& path = paths[index];
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS || wcscmp(source.viewGdiDeviceName, gdiName) != 0)
        {
            continue;
        }
        DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
        white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        white.header.size = sizeof(white);
        white.header.adapterId = path.targetInfo.adapterId;
        white.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&white.header) != ERROR_SUCCESS)
        {
            return std::nullopt;
        }
        // In thousandths of 80 cd/m^2 (scRGB's 1.0).
        return static_cast<float>(white.SDRWhiteLevel) / 1000.0f * 80.0f;
    }
    return std::nullopt;
}

std::string NarrowName(const wchar_t* name)
{
    std::string narrow;
    for (const wchar_t* c = name; *c != 0; ++c)
    {
        narrow.push_back(*c < 128 ? static_cast<char>(*c) : '?');
    }
    return narrow;
}
}

DisplayHdrInfo QueryDisplayHdrInfo(SDL_Window* window)
{
    using Microsoft::WRL::ComPtr;

    DisplayHdrInfo info;
    if (window == nullptr)
    {
        return info;
    }
    const auto hwnd = static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (hwnd == nullptr)
    {
        return info;
    }
    const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);

    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
    {
        return info;
    }
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT adapterIndex = 0; factory->EnumAdapters1(adapterIndex, &adapter) != DXGI_ERROR_NOT_FOUND; ++adapterIndex)
    {
        ComPtr<IDXGIOutput> output;
        for (UINT outputIndex = 0; adapter->EnumOutputs(outputIndex, &output) != DXGI_ERROR_NOT_FOUND; ++outputIndex)
        {
            ComPtr<IDXGIOutput6> output6;
            DXGI_OUTPUT_DESC1 desc{};
            if (FAILED(output.As(&output6)) || FAILED(output6->GetDesc1(&desc)) || desc.Monitor != monitor)
            {
                continue;
            }
            info.known = true;
            info.hdrEnabled = desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
            info.maxLuminance = desc.MaxLuminance;
            info.maxFullFrameLuminance = desc.MaxFullFrameLuminance;
            info.minLuminance = desc.MinLuminance;
            info.name = NarrowName(desc.DeviceName);
            if (const std::optional<float> white = QuerySdrWhiteNits(desc.DeviceName))
            {
                info.sdrWhiteNits = *white;
            }
            return info;
        }
    }
    return info;
}
#else
DisplayHdrInfo QueryDisplayHdrInfo(SDL_Window* window)
{
    static_cast<void>(window);
    return {};
}
#endif

DisplayHdrMonitor::DisplayHdrMonitor(SDL_Window* window, int intervalMs)
    : m_window(window)
    , m_intervalMs(intervalMs)
{
    // The first answer before the first frame, so the swapchain starts in the right mode.
    m_latest = QueryDisplayHdrInfo(m_window);
    m_thread = std::thread([this]
                           {
                               Run();
                           });
}

DisplayHdrMonitor::~DisplayHdrMonitor()
{
    {
        std::lock_guard lock(m_mutex);
        m_stop = true;
    }
    m_wake.notify_all();
    m_thread.join();
}

DisplayHdrInfo DisplayHdrMonitor::Latest() const
{
    std::lock_guard lock(m_mutex);
    return m_latest;
}

void DisplayHdrMonitor::Run()
{
    std::unique_lock lock(m_mutex);
    while (!m_wake.wait_for(lock, std::chrono::milliseconds(m_intervalMs), [this]
                            {
                                return m_stop;
                            }))
    {
        lock.unlock();
        DisplayHdrInfo info = QueryDisplayHdrInfo(m_window);
        lock.lock();
        m_latest = std::move(info);
    }
}
}
}
