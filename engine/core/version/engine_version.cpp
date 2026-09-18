#include "engine_version.h"

namespace me
{

namespace EngineVersion
{
uint32_t Major()
{
    return MINIENGINE_VERSION_MAJOR;
}

uint32_t Minor()
{
    return MINIENGINE_VERSION_MINOR;
}

uint32_t Patch()
{
    return MINIENGINE_VERSION_PATCH;
}

const char* String()
{
    return MINIENGINE_VERSION_STRING;
}
}
}
