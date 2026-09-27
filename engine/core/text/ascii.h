#pragma once

#include <algorithm>
#include <cctype>
#include <string>

namespace me
{

// ASCII lower case, for comparing file names and extensions the way Windows does.
inline std::string ToLowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character)
                   {
                       return static_cast<char>(std::tolower(character));
                   });
    return value;
}
}
