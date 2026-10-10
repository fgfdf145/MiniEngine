#include "display_hdr.h"

#include <SDL3/SDL.h>

#include <chrono>
#include <cstdlib>
#include <optional>
#include <utility>

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

#include <string>
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

HWND WindowHandle(SDL_Window* window)
{
    if (window == nullptr)
    {
        return nullptr;
    }
    return static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
}
}

class DisplayHdrQuery
{
  public:
    explicit DisplayHdrQuery(SDL_Window* window)
        : m_hwnd(WindowHandle(window))
    {
    }

    const DisplayHdrInfo& Info() const
    {
        return m_info;
    }

    // Whether the last answer may be out of date: the window is on another display, or the factory
    // it came from went stale (DXGI marks it when the adapters, the outputs or an output's HDR mode
    // change). Microseconds, no enumeration.
    bool Stale() const
    {
        return m_factory == nullptr || !m_factory->IsCurrent() || (m_hwnd != nullptr && MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST) != m_monitor);
    }

    // Everything again, on a new factory.
    void Full()
    {
        using Microsoft::WRL::ComPtr;

        m_info = {};
        m_factory = nullptr;
        m_gdiName.clear();
        if (m_hwnd == nullptr)
        {
            return;
        }
        m_monitor = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&m_factory))))
        {
            m_factory = nullptr;
            return;
        }
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT adapterIndex = 0; m_factory->EnumAdapters1(adapterIndex, &adapter) != DXGI_ERROR_NOT_FOUND; ++adapterIndex)
        {
            ComPtr<IDXGIOutput> output;
            for (UINT outputIndex = 0; adapter->EnumOutputs(outputIndex, &output) != DXGI_ERROR_NOT_FOUND; ++outputIndex)
            {
                ComPtr<IDXGIOutput6> output6;
                DXGI_OUTPUT_DESC1 desc{};
                if (FAILED(output.As(&output6)) || FAILED(output6->GetDesc1(&desc)) || desc.Monitor != m_monitor)
                {
                    continue;
                }
                m_info.known = true;
                m_info.hdrEnabled = desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                m_info.maxLuminance = desc.MaxLuminance;
                m_info.maxFullFrameLuminance = desc.MaxFullFrameLuminance;
                m_info.minLuminance = desc.MinLuminance;
                m_info.name = NarrowName(desc.DeviceName);
                m_gdiName = desc.DeviceName;
                SdrWhite();
                return;
            }
        }
    }

    // Windows' SDR content brightness only (no enumeration).
    void SdrWhite()
    {
        if (m_gdiName.empty())
        {
            return;
        }
        if (const std::optional<float> white = QuerySdrWhiteNits(m_gdiName.c_str()))
        {
            m_info.sdrWhiteNits = *white;
        }
    }

  private:
    HWND m_hwnd = nullptr;
    HMONITOR m_monitor = nullptr;
    Microsoft::WRL::ComPtr<IDXGIFactory1> m_factory;
    std::wstring m_gdiName;
    DisplayHdrInfo m_info;
};
#else
class DisplayHdrQuery
{
  public:
    explicit DisplayHdrQuery(SDL_Window* window)
    {
        static_cast<void>(window);
    }

    const DisplayHdrInfo& Info() const
    {
        return m_info;
    }

    bool Stale() const
    {
        return false;
    }

    void Full()
    {
    }

    void SdrWhite()
    {
    }

  private:
    DisplayHdrInfo m_info;
};
#endif

DisplayHdrInfo QueryDisplayHdrInfo(SDL_Window* window)
{
    DisplayHdrQuery query(window);
    query.Full();
    return query.Info();
}

namespace
{
// How often the thread checks whether the display changed, and reads the SDR content brightness.
constexpr std::chrono::milliseconds kStaleCheckInterval{250};
constexpr std::chrono::milliseconds kSdrWhiteInterval{1000};
}

DisplayHdrMonitor::DisplayHdrMonitor(SDL_Window* window)
    : m_query(std::make_unique<DisplayHdrQuery>(window))
{
    // The first answer before the first frame, so the swapchain starts in the right mode.
    m_query->Full();
    m_latest = m_query->Info();
    // MINIENGINE_NO_DISPLAY_MONITOR=1 asks once and never again (frame pacing comparisons).
    if (const char* off = std::getenv("MINIENGINE_NO_DISPLAY_MONITOR"); off != nullptr && off[0] == '1')
    {
        return;
    }
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
    if (m_thread.joinable())
    {
        m_thread.join();
    }
}

DisplayHdrInfo DisplayHdrMonitor::Latest() const
{
    std::lock_guard lock(m_mutex);
    return m_latest;
}

void DisplayHdrMonitor::Refresh()
{
    {
        std::lock_guard lock(m_mutex);
        m_refresh = true;
    }
    m_wake.notify_all();
}

void DisplayHdrMonitor::Run()
{
    auto lastSdrWhite = std::chrono::steady_clock::now();
    std::unique_lock lock(m_mutex);
    while (true)
    {
        m_wake.wait_for(lock, kStaleCheckInterval, [this]
                        {
                            return m_stop || m_refresh;
                        });
        if (m_stop)
        {
            return;
        }
        const bool refresh = std::exchange(m_refresh, false);
        lock.unlock();
        const auto now = std::chrono::steady_clock::now();
        if (refresh || m_query->Stale())
        {
            m_query->Full();
            lastSdrWhite = now;
        }
        else if (now - lastSdrWhite >= kSdrWhiteInterval)
        {
            m_query->SdrWhite();
            lastSdrWhite = now;
        }
        lock.lock();
        if (m_latest != m_query->Info())
        {
            m_latest = m_query->Info();
        }
    }
}
}
}
