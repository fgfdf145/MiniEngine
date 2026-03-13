#pragma once

#include <optional>
#include <string>

std::optional<std::string> OpenModelFileDialog();
std::optional<std::string> OpenSceneFileDialog();
std::optional<std::string> SaveSceneFileDialog();
