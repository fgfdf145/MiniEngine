#pragma once

#include "tyre_magic_formula.h"

#include <filesystem>
#include <istream>
#include <string>
#include <vector>

namespace me::tyre
{

// A Magic Formula data set read from a tyre property file (.tir, the MSC ADAMS / MF-Tyre text
// format): "[SECTION]" headers and "KEY = value" lines, comments after '$' or '!'. Keys are
// matched case-insensitively and regardless of section. Keys this model does not use (SWIFT,
// pressure, turn slip, ...) land in ignoredKeys.
struct TirReadResult
{
    MagicFormulaParameters parameters;
    std::vector<std::string> ignoredKeys;
};

// Throws std::runtime_error when FNOMIN, UNLOADED_RADIUS or LONGVL is missing or not positive.
TirReadResult ReadTir(std::istream& in);
TirReadResult ReadTirFile(const std::filesystem::path& path);

}
