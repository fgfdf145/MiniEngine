#include "editor_icons.h"

#include <IconsFontAwesome6.h>

#include <cstddef>

namespace me
{

// Generated from third_party/fontawesome/fa-solid-900.ttf by cmake/MiniEngineEmbedFile.cmake.
extern const unsigned char kFontAwesomeSolidTtf[];
extern const std::size_t kFontAwesomeSolidTtfSize;

void MergeEditorIconFont(ImFontAtlas& fonts, float sizePixels)
{
    static const ImWchar kIconRanges[] = {ICON_MIN_FA, ICON_MAX_16_FA, 0};

    ImFontConfig config{};
    config.MergeMode = true;
    config.PixelSnapH = true;
    // Every icon as wide as the text is tall, so icon buttons and menu icons line up.
    config.GlyphMinAdvanceX = sizePixels;
    // The data is static: the atlas must not free it.
    config.FontDataOwnedByAtlas = false;
    fonts.AddFontFromMemoryTTF(
        const_cast<unsigned char*>(kFontAwesomeSolidTtf),
        static_cast<int>(kFontAwesomeSolidTtfSize),
        sizePixels,
        &config,
        kIconRanges);
}
}
