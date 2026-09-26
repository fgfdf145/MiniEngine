#include <engine/renderer/render_types.h>

#include <iostream>
#include <stdexcept>
#include <string>

// main() stays in the global namespace; everything it drives lives in me::.
using namespace me;

namespace
{
void Require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void Expect(RenderExtent extent, uint32_t width, uint32_t height, const std::string& what)
{
    Require(extent.width == width && extent.height == height,
            what + ": " + std::to_string(extent.width) + "x" + std::to_string(extent.height) + ", not " +
                std::to_string(width) + "x" + std::to_string(height));
}
}

int main()
{
    try
    {
        Expect(ScaleViewportExtent(667.0f, 541.0f, 1.0f, 1.0f), 667, 541, "one pixel per point is the point size");
        Expect(ScaleViewportExtent(667.0f, 541.0f, 2.0f, 1.0f), 1334, 1082, "a Retina display doubles both axes");
        Expect(ScaleViewportExtent(667.0f, 541.0f, 2.0f, 0.5f), 667, 541, "half render scale undoes it");
        Expect(ScaleViewportExtent(100.4f, 99.6f, 1.0f, 1.0f), 100, 100, "fractional sizes round");
        Expect(ScaleViewportExtent(0.0f, -5.0f, 2.0f, 1.0f), 1, 1, "an empty panel still renders one pixel");
        Expect(ScaleViewportExtent(640.0f, 480.0f, 2.0f, 0.0f), 1, 1, "a zero scale renders one pixel");
    }
    catch (const std::exception& error)
    {
        std::cerr << "viewport extent tests failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "viewport extent tests passed\n";
    return 0;
}
