#pragma once

#include <glm/glm.hpp>

#include <string>

namespace me
{

// A top-down picture of the scene's ground (a game's radar map) the viewport shows as a minimap at its
// bottom left. The picture is laid flat over the world: its left edge at worldMin.x and right edge at
// worldMax.x, its top edge at worldMin.y and bottom edge at worldMax.y, both as world Z (north, -Z, is
// up the picture).
struct SceneMinimap
{
    // The picture's path, as model paths are written (relative to the project root); empty: no minimap.
    std::string image;
    // World (x, z) under the picture's top-left and bottom-right corners, in metres.
    glm::vec2 worldMin{0.0f};
    glm::vec2 worldMax{0.0f};

    bool IsValid() const
    {
        return !image.empty() && worldMax.x > worldMin.x && worldMax.y > worldMin.y;
    }

    bool operator==(const SceneMinimap&) const = default;
};
}
