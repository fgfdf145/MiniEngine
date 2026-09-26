#pragma once

#include <imgui.h>

namespace me
{

// Merges the Font Awesome 6 solid icons into the font added last, so ICON_FA_* strings
// (IconsFontAwesome6.h) draw in any text. Call right after adding the UI font. The font is built
// into the executable.
void MergeEditorIconFont(ImFontAtlas& fonts, float sizePixels);
}
