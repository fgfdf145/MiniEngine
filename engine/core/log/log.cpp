#include "log.h"

#include <spdlog/sinks/stdout_color_sinks.h>

void Log::Init()
{
    auto logger = spdlog::stdout_color_mt("MiniEngine");
    spdlog::set_default_logger(logger);
    spdlog::set_pattern("[%T] [%^%l%$] %v");
    spdlog::set_level(spdlog::level::trace);
}
