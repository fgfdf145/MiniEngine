#include "thread_setup.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <string>

namespace me
{

void ConfigureCurrentThread(const char* name)
{
#if defined(_WIN32)
    const std::string narrow(name);
    const std::wstring wide(narrow.begin(), narrow.end());
    SetThreadDescription(GetCurrentThread(), wide.c_str());

    THREAD_POWER_THROTTLING_STATE throttling{};
    throttling.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    throttling.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    throttling.StateMask = 0;
    SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &throttling, sizeof(throttling));
#else
    (void)name;
#endif
}
}
