#pragma once

#include <spdlog/spdlog.h>

#include <string>
#include <utility>
#include <vector>

class Log
{
public:
    static void Init();

    template <typename... Args>
    static void InputInfo(fmt::format_string<Args...> formatString, Args&&... args)
    {
        WriteInputLine(fmt::format(formatString, std::forward<Args>(args)...));
    }

    static std::vector<std::string> GetInputMessagesSnapshot();
    static void ClearInputMessages();

private:
    static void WriteInputLine(const std::string& message);
};

#define LOG_INFO(...) spdlog::info(__VA_ARGS__)
#define LOG_INPUT_INFO(...) Log::InputInfo(__VA_ARGS__)
#define LOG_WARN(...) spdlog::warn(__VA_ARGS__)
#define LOG_ERROR(...) spdlog::error(__VA_ARGS__)
