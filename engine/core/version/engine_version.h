#pragma once

#include <cstdint>

namespace me
{

namespace EngineVersion
{
// All four come from project(VERSION ...) in the root CMakeLists.txt, which is the only place the
// version is written down.
uint32_t Major();
uint32_t Minor();
uint32_t Patch();

// "major.minor.patch".
const char* String();
}
}
