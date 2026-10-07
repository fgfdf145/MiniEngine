// Precompiled header for the engine libraries and tests (cmake/MiniEnginePch.cmake).
//
// The standard library and stable third-party headers only. An engine header here would
// rebuild every translation unit whenever it changed, and a header that needs a macro defined
// before it (windows.h, stb, tinygltf, Vulkan) would see the wrong configuration.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <numbers>
#include <numeric>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

// Targets without the vcpkg include directory get the standard library alone.
#if __has_include(<glm/glm.hpp>)
#include <glm/glm.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#endif

#if __has_include(<entt/entt.hpp>)
#include <entt/entt.hpp>
#endif

#if __has_include(<imgui.h>)
#include <imgui.h>
#endif

// Needs no macro before it (SDL_main.h, which does, is separate); core's input.h, the window and
// the editor UI reach it from most engine files.
#if __has_include(<SDL3/SDL.h>)
#include <SDL3/SDL.h>
#endif

// spdlog without SPDLOG_COMPILED_LIB is its header-only build; only a target that links the
// compiled library may have it precompiled.
#if defined(SPDLOG_COMPILED_LIB) && __has_include(<spdlog/spdlog.h>)
#include <spdlog/spdlog.h>
#endif
