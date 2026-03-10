#pragma once
#include <spdlog/spdlog.h>

class Log
{
public:
    static void Init();
};

#define LOG_INFO(...) spdlog::info(__VA_ARGS__)
#define LOG_WARN(...) spdlog::warn(__VA_ARGS__)
#define LOG_ERROR(...) spdlog::error(__VA_ARGS__)
