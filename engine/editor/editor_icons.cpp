#include "editor_icons.h"

#include <IconsPhosphor.h>

#include <cstddef>

namespace me
{

// Generated from third_party/phosphor/Phosphor.ttf by cmake/MiniEngineEmbedFile.cmake.
extern const unsigned char kPhosphorRegularTtf[];
extern const std::size_t kPhosphorRegularTtfSize;

void MergeEditorIconFont(ImFontAtlas& fonts, float sizePixels)
{
    static const ImWchar kIconRanges[] = {ICON_MIN_PH, ICON_MAX_PH, 0};

    // The Claude desktop app draws Phosphor's regular icons a little larger than its text: their ink
    // stands about 1.25 times the text's capitals.
    const float iconSizePixels = sizePixels * (14.0f / 13.0f);

    ImFontConfig config{};
    config.MergeMode = true;
    config.PixelSnapH = true;
    // Every icon as wide as it is tall, so icon buttons and menu icons line up.
    config.GlyphMinAdvanceX = iconSizePixels;
    // The data is static: the atlas must not free it.
    config.FontDataOwnedByAtlas = false;
    fonts.AddFontFromMemoryTTF(
        const_cast<unsigned char*>(kPhosphorRegularTtf),
        static_cast<int>(kPhosphorRegularTtfSize),
        iconSizePixels,
        &config,
        kIconRanges);
}
}
