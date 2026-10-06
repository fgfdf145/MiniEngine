#pragma once

#include <imgui.h>

namespace me
{

// Merges the Phosphor icons (regular weight, the Claude desktop app's) into the font added last, so
// ICON_PH_* strings (IconsPhosphor.h) draw in any text. Call right after adding the UI font. The font is built
// into the executable.
void MergeEditorIconFont(ImFontAtlas& fonts, float sizePixels);
}
