#pragma once

#include <cstdint>

struct RenderExtent
{
    uint32_t width = 0;
    uint32_t height = 0;

    bool IsValid() const
    {
        return width > 0 && height > 0;
    }
};
