#pragma once

#include <spdlog/spdlog.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace me
{

class Log
{
  public:
    static void Init();

    template <typename... Args>
    static void InputInfo(fmt::format_string<Args...> formatString, Args&&... args)
    {
        WriteInputLine(fmt::format(formatString, std::forward<Args>(args)...));
    }

    // Copies the input messages into `messages` only when they changed since `revision`, then
    // updates `revision`. Pass revision 0 the first time. Returns whether it copied, so a per-frame
    // caller does not copy the whole log every frame.
    static bool RefreshInputMessagesSnapshot(std::vector<std::string>& messages, uint64_t& revision);
    static void ClearInputMessages();

  private:
    static void WriteInputLine(const std::string& message);
};

#define LOG_INFO(...) spdlog::info(__VA_ARGS__)
#define LOG_INPUT_INFO(...) Log::InputInfo(__VA_ARGS__)
#define LOG_WARN(...) spdlog::warn(__VA_ARGS__)
#define LOG_ERROR(...) spdlog::error(__VA_ARGS__)
}
