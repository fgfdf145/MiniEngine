#pragma once

// What the operating system says about the display a window is on: whether it is in HDR, the
// luminances it reports and how bright it shows SDR content. Windows reports all of it (DXGI and
// the display configuration API); other platforms report nothing.

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

struct SDL_Window;

namespace me
{

namespace platform::display
{
struct DisplayHdrInfo
{
    bool operator==(const DisplayHdrInfo&) const = default;

    // False when the platform reports nothing; every other field is then its default.
    bool known = false;
    // The display is in HDR (Windows: "Use HDR" on, the desktop in BT.2100 PQ).
    bool hdrEnabled = false;
    // In cd/m^2: the peak for a small highlight, the peak for a full white screen and the black level,
    // from the display's EDID or a Windows HDR Calibration profile. 0 when the display reports none.
    float maxLuminance = 0.0f;
    float maxFullFrameLuminance = 0.0f;
    float minLuminance = 0.0f;
    // How bright SDR content (1.0 white) shows in HDR, in cd/m^2: Windows' "SDR content brightness".
    float sdrWhiteNits = 80.0f;
    // The OS's name for the display, e.g. \\.\DISPLAY1.
    std::string name;
};

// Asks now. A few milliseconds on Windows; call it off the frame path (see DisplayHdrMonitor).
DisplayHdrInfo QueryDisplayHdrInfo(SDL_Window* window);

// Polls QueryDisplayHdrInfo on its own thread, because Windows sends no event when the user changes
// the SDR content brightness or turns HDR on; Latest() returns the newest answer.
class DisplayHdrMonitor
{
  public:
    explicit DisplayHdrMonitor(SDL_Window* window, int intervalMs = 1000);
    ~DisplayHdrMonitor();
    DisplayHdrMonitor(const DisplayHdrMonitor&) = delete;
    DisplayHdrMonitor& operator=(const DisplayHdrMonitor&) = delete;

    DisplayHdrInfo Latest() const;

  private:
    void Run();

    SDL_Window* m_window = nullptr;
    int m_intervalMs = 1000;
    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    bool m_stop = false;
    DisplayHdrInfo m_latest;
    std::thread m_thread;
};
}
}
