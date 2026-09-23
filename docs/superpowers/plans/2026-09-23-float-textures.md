# Float Textures Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Decode `.hdr` and `.exr` as linear float images and upload them to any material slot as `R16G16B16A16_SFLOAT`, instead of truncating them to RGBA8.

**Architecture:** `TextureLoader` gains a float path (`LoadRGBA32F`, stb for `.hdr`, tinyexr for `.exr`) that returns lossless RGBA32F. `PrepareTexture` routes float files around the BC compressor and disk cache and packs them to RGBA16F on the worker thread. `VulkanTexture` gains a half-float constructor that shares the RGBA8 upload routine (staging copy plus GPU blit mips).

**Tech Stack:** C++20, Vulkan, stb_image / stb_image_write, tinyexr 3.1.0 (vcpkg), CMake + CTest.

**Spec:** `docs/superpowers/specs/2026-09-23-float-textures-design.md`

## Global Constraints

- Commit directly to `main`. Never stage `miniengine.settings.json` or `docs/superpowers/plans/2026-09-13-gbuffer-phase2-deferred-shading.md`.
- Configure: `cmake --preset vs2026-x64` (needed once after `vcpkg.json` changes; it installs tinyexr). Build: `cmake --build --preset vs2026-x64-debug --parallel`. Tests: `ctest --test-dir out/build/vs2026-x64 -C Debug --output-on-failure`. Format check: `powershell -File scripts/check-format.ps1`; fix with `powershell -File scripts/format-code.ps1`.
- Validation run: `out\build\vs2026-x64\app\Debug\miniengine_app.exe --backend vulkan --frames 300` must print zero `[error]`/`[warning]` lines from the validation layer.
- Float detection is by extension only, case-insensitive: `.hdr`, `.exr`.
- Half packing rule: NaN -> 0; everything else clamps to [-65504, 65504]; sign kept.
- Float textures are never BC-compressed and never written to the texture disk cache.
- GPU format for float material textures: `VK_FORMAT_R16G16B16A16_SFLOAT`, regardless of `TextureUsage`.
- Commit messages end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

---

### Task 1: tinyexr dependency and float decoding

**Files:**
- Modify: `vcpkg.json`, `cmake/MiniEngineDependencies.cmake:133-134`, `engine/asset/CMakeLists.txt`
- Modify: `engine/asset/texture_loader.h`, `engine/asset/texture_loader.cpp`
- Test: `tests/float_texture_tests.cpp` (new), `tests/CMakeLists.txt`

**Interfaces:**
- Produces: `me::FloatTextureData { int width; int height; std::vector<float> pixels; bool IsValid() const; }`,
  `static bool me::TextureLoader::IsFloatImageFile(const std::filesystem::path&)`,
  `static me::FloatTextureData me::TextureLoader::LoadRGBA32F(const std::string& path)` (throws `std::runtime_error`),
  `me::TextureLoader::LoadRGBA8` now also accepts `.hdr`/`.exr` (clamped, sRGB-encoded).

- [ ] **Step 1: Add the dependency.**

`vcpkg.json`, in `dependencies` after `"stb"` (keep alphabetical order):

```json
    "stb",
    "tinyexr",
    "tinygltf",
```

`cmake/MiniEngineDependencies.cmake`, after `find_package(unofficial-bc7enc-rdo CONFIG REQUIRED)`:

```cmake
find_package(tinyexr CONFIG REQUIRED)
```

`engine/asset/CMakeLists.txt`, in the `PRIVATE` block of `target_link_libraries(engine_asset ...)`:

```cmake
    PRIVATE
        unofficial::bc7enc-rdo::bc7enc-rdo
        unofficial::tinyexr::tinyexr
```

Run `cmake --preset vs2026-x64`. Expected: vcpkg installs `miniz` and `tinyexr`, configure succeeds.
**If the tinyexr download fails with a SHA512 mismatch** (the README, line ~126, records this happening to `tinygltf` because GitHub re-compresses archives): copy `.deps/vcpkg/ports/tinyexr/` to `cmake/vcpkg-overlay-ports/tinyexr/`, replace `vcpkg_from_github(...)` with `vcpkg_from_git(OUT_SOURCE_PATH SOURCE_PATH URL https://github.com/syoyo/tinyexr.git REF <commit of tag v3.1.0> PATCHES fixtargets.patch)` (get the commit with `git ls-remote https://github.com/syoyo/tinyexr.git refs/tags/v3.1.0`), and add a README paragraph next to the `bc7enc-rdo` one explaining it, matching the `tinygltf` note.

- [ ] **Step 2: Write the failing tests.** Create `tests/float_texture_tests.cpp`:

```cpp
#include <engine/asset/texture_loader.h>

#include <stb_image_write.h>
#include <tinyexr.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

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

bool NearlyEqual(float actual, float expected, float relativeTolerance)
{
    return std::fabs(actual - expected) <= relativeTolerance * std::max(std::fabs(expected), 1e-6f);
}

// A fresh directory per test, removed when the object goes out of scope.
class ScratchDirectory
{
  public:
    ScratchDirectory()
    {
        std::random_device random;
        m_path = std::filesystem::temp_directory_path() / ("miniengine_float_textures_" + std::to_string(random()));
        std::filesystem::create_directories(m_path);
    }
    ~ScratchDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(m_path, ignored);
    }
    const std::filesystem::path& Path() const
    {
        return m_path;
    }

  private:
    std::filesystem::path m_path;
};

// 3x2, row 0 first. Every value is exact in RGBE: each pixel's channels share its largest
// channel's exponent, and the smaller channels here are that channel over a power of two.
const std::vector<float> kHdrRgb = {
    0.25f, 0.25f, 0.25f, 1.0f, 1.0f, 1.0f, 3.5f, 3.5f, 3.5f,
    1000.0f, 1000.0f, 1000.0f, 0.5f, 2.0f, 8.0f, 0.0f, 0.0f, 0.0f};

std::filesystem::path WriteHdr(const std::filesystem::path& directory)
{
    const std::filesystem::path path = directory / "fixture.hdr";
    Require(stbi_write_hdr(path.string().c_str(), 3, 2, 3, kHdrRgb.data()) != 0, "could not write the .hdr fixture");
    return path;
}

const std::vector<float> kExrRgba = {
    0.25f, 0.5f, 0.75f, 1.0f, 3.5f, 1000.0f, 12345.5f, 0.5f, 0.0f, 0.0f, 0.0f, 0.0f,
    1.0f, 1.0f, 1.0f, 1.0f, 2.0f, 4.0f, 8.0f, 0.25f, 65504.0f, 0.125f, 0.0625f, 1.0f};

std::filesystem::path WriteExr(const std::filesystem::path& directory)
{
    const std::filesystem::path path = directory / "fixture.exr";
    const char* error = nullptr;
    const int result = SaveEXR(kExrRgba.data(), 3, 2, 4, 0, path.string().c_str(), &error);
    if (result != TINYEXR_SUCCESS)
    {
        const std::string reason = error != nullptr ? error : "unknown";
        FreeEXRErrorMessage(error);
        throw std::runtime_error("could not write the .exr fixture: " + reason);
    }
    return path;
}

template <typename Function>
bool Throws(Function&& function)
{
    try
    {
        function();
    }
    catch (const std::runtime_error&)
    {
        return true;
    }
    return false;
}

void DetectsFloatFilesByExtension()
{
    Require(TextureLoader::IsFloatImageFile("a/b.hdr"), ".hdr is a float image");
    Require(TextureLoader::IsFloatImageFile("a/b.HDR"), ".HDR is a float image");
    Require(TextureLoader::IsFloatImageFile("b.exr"), ".exr is a float image");
    Require(TextureLoader::IsFloatImageFile("b.Exr"), ".Exr is a float image");
    Require(!TextureLoader::IsFloatImageFile("b.png"), ".png is not a float image");
    Require(!TextureLoader::IsFloatImageFile("b.jpg"), ".jpg is not a float image");
    Require(!TextureLoader::IsFloatImageFile("hdr"), "no extension is not a float image");
}

void LoadsRadianceHdr()
{
    ScratchDirectory directory;
    const FloatTextureData image = TextureLoader::LoadRGBA32F(WriteHdr(directory.Path()).string());
    Require(image.IsValid() && image.width == 3 && image.height == 2, ".hdr keeps its size");
    for (size_t texel = 0; texel < 6; ++texel)
    {
        for (size_t channel = 0; channel < 3; ++channel)
        {
            const float expected = kHdrRgb[texel * 3 + channel];
            const float actual = image.pixels[texel * 4 + channel];
            Require(NearlyEqual(actual, expected, 0.01f),
                    ".hdr texel " + std::to_string(texel) + " channel " + std::to_string(channel) + " is " +
                        std::to_string(actual) + ", expected " + std::to_string(expected));
        }
        Require(image.pixels[texel * 4 + 3] == 1.0f, ".hdr alpha is 1");
    }
}

void LoadsOpenExr()
{
    ScratchDirectory directory;
    const FloatTextureData image = TextureLoader::LoadRGBA32F(WriteExr(directory.Path()).string());
    Require(image.IsValid() && image.width == 3 && image.height == 2, ".exr keeps its size");
    for (size_t index = 0; index < kExrRgba.size(); ++index)
    {
        Require(NearlyEqual(image.pixels[index], kExrRgba[index], 1e-3f),
                ".exr value " + std::to_string(index) + " is " + std::to_string(image.pixels[index]) + ", expected " +
                    std::to_string(kExrRgba[index]));
    }
}

void LoadRgba8ShowsFloatFilesClampedAndEncoded()
{
    ScratchDirectory directory;
    const TextureData image = TextureLoader::LoadRGBA8(WriteHdr(directory.Path()).string());
    Require(image.width == 3 && image.height == 2 && image.pixels.size() == 3 * 2 * 4, "RGBA8 view keeps the size");
    Require(image.pixels[0] == 137, "linear 0.25 is sRGB 137, got " + std::to_string(image.pixels[0]));
    Require(image.pixels[4] == 255, "linear 1.0 is 255");
    Require(image.pixels[3 * 4] == 255, "1000 clamps to 255");
    Require(image.pixels[5 * 4] == 0, "0 stays 0");
    Require(image.pixels[3] == 255, "alpha is 255");
}

void RejectsWhatIsNotAFloatImage()
{
    ScratchDirectory directory;

    const std::filesystem::path png = directory.Path() / "plain.png";
    const std::uint8_t pixel[4] = {10, 20, 30, 255};
    Require(stbi_write_png(png.string().c_str(), 1, 1, 4, pixel, 4) != 0, "could not write the .png fixture");
    Require(Throws([&]() { TextureLoader::LoadRGBA32F(png.string()); }), "a .png is not a float image");

    // A PNG under an .hdr name: stbi_loadf would convert it from LDR, which is not what .hdr means.
    const std::filesystem::path disguised = directory.Path() / "disguised.hdr";
    std::filesystem::copy_file(png, disguised);
    Require(Throws([&]() { TextureLoader::LoadRGBA32F(disguised.string()); }), "a PNG named .hdr is refused");

    const std::filesystem::path exr = WriteExr(directory.Path());
    std::filesystem::resize_file(exr, 20);
    Require(Throws([&]() { TextureLoader::LoadRGBA32F(exr.string()); }), "a truncated .exr throws");

    Require(Throws([&]() { TextureLoader::LoadRGBA32F((directory.Path() / "missing.exr").string()); }),
            "a missing file throws");
}
}

int main()
{
    try
    {
        DetectsFloatFilesByExtension();
        LoadsRadianceHdr();
        LoadsOpenExr();
        LoadRgba8ShowsFloatFilesClampedAndEncoded();
        RejectsWhatIsNotAFloatImage();
    }
    catch (const std::exception& error)
    {
        std::cerr << "float texture tests failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "float texture tests passed\n";
    return 0;
}
```

Register it in `tests/CMakeLists.txt` directly after the `miniengine_texture_preparation_tests` block (`stb_image_write`'s implementation is compiled into `engine_asset` by `gltf_model_loader.cpp`, so only its header path is needed):

```cmake
add_executable(miniengine_float_texture_tests
    float_texture_tests.cpp
)
miniengine_group_target_sources(miniengine_float_texture_tests)

# tinyexr is linked directly as well: the tests write the EXR fixtures engine_asset reads.
target_link_libraries(miniengine_float_texture_tests
    PRIVATE
        engine_asset
        unofficial::tinyexr::tinyexr
)
target_include_directories(miniengine_float_texture_tests PRIVATE "${Stb_INCLUDE_DIR}")

add_test(
    NAME miniengine.float_textures
    COMMAND miniengine_float_texture_tests
)

if(WIN32)
    add_custom_command(TARGET miniengine_float_texture_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:miniengine_float_texture_tests>
            $<TARGET_FILE_DIR:miniengine_float_texture_tests>
        COMMAND_EXPAND_LISTS
    )
endif()

set_target_properties(miniengine_float_texture_tests PROPERTIES FOLDER "Tests")
```

- [ ] **Step 3: Run to verify it fails.** Build. Expected: compile errors, `FloatTextureData`, `IsFloatImageFile` and `LoadRGBA32F` are not members of `me` / `TextureLoader`.

- [ ] **Step 4: Implement.** In `engine/asset/texture_loader.h` add `#include <filesystem>` and, after `TextureData`:

```cpp
// Linear RGBA, four floats per texel, rows top-down. What .hdr and .exr files decode to, before any
// consumer picks a GPU format.
struct FloatTextureData
{
    int width = 0;
    int height = 0;
    std::vector<float> pixels;

    bool IsValid() const
    {
        return width > 0 && height > 0 &&
               pixels.size() == static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    }
};
```

and in `TextureLoader`, before `LoadRGBA8`:

```cpp
    // .hdr and .exr, by extension and ignoring case: the files LoadRGBA32F reads.
    static bool IsFloatImageFile(const std::filesystem::path& path);
    // Scene-linear RGBA32F, rows top-down. A missing alpha is 1. Throws std::runtime_error for a file
    // that is not a float image or cannot be decoded.
    static FloatTextureData LoadRGBA32F(const std::string& path);
```

Also extend the `LoadRGBA8` comment: "A float image is clamped to [0, 1] and its colour sRGB-encoded, for previews."

In `engine/asset/texture_loader.cpp` add `#include <tinyexr.h>`, `#include <cmath>`, `#include <cstdlib>`, and in the anonymous namespace:

```cpp
std::string LowerExtension(const std::filesystem::path& path)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c)
                   {
                       return static_cast<char>(std::tolower(c));
                   });
    return ext;
}

void FlipRows(TextureData& texture)
{
    const size_t rowSize = static_cast<size_t>(texture.width) * 4;
    std::vector<std::uint8_t> flipped(texture.pixels.size());
    for (int y = 0; y < texture.height; ++y)
    {
        const size_t sourceOffset = static_cast<size_t>(y) * rowSize;
        const size_t destinationOffset = static_cast<size_t>(texture.height - 1 - y) * rowSize;
        std::memcpy(flipped.data() + destinationOffset, texture.pixels.data() + sourceOffset, rowSize);
    }
    texture.pixels = std::move(flipped);
}

FloatTextureData LoadRadianceHdr(const std::string& path)
{
    // stbi_loadf also accepts LDR files and converts them with a gamma curve, which would make a PNG
    // renamed to .hdr load as something it is not.
    if (!stbi_is_hdr(path.c_str()))
    {
        throw std::runtime_error("'" + path + "' is not a Radiance HDR image");
    }
    int width = 0;
    int height = 0;
    int channelCount = 0;
    float* rawPixels = stbi_loadf(path.c_str(), &width, &height, &channelCount, STBI_rgb_alpha);
    if (rawPixels == nullptr)
    {
        const char* reason = stbi_failure_reason();
        throw std::runtime_error("Failed to load texture '" + path + "': " + (reason ? reason : "unknown error"));
    }
    FloatTextureData texture{};
    texture.width = width;
    texture.height = height;
    texture.pixels.assign(rawPixels, rawPixels + static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    stbi_image_free(rawPixels);
    return texture;
}

FloatTextureData LoadOpenExr(const std::string& path)
{
    float* rawPixels = nullptr;
    int width = 0;
    int height = 0;
    const char* error = nullptr;
    const int result = LoadEXR(&rawPixels, &width, &height, path.c_str(), &error);
    if (result != TINYEXR_SUCCESS)
    {
        const std::string reason = error != nullptr ? error : "error code " + std::to_string(result);
        FreeEXRErrorMessage(error);
        throw std::runtime_error("Failed to load texture '" + path + "': " + reason);
    }
    FloatTextureData texture{};
    texture.width = width;
    texture.height = height;
    texture.pixels.assign(rawPixels, rawPixels + static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    std::free(rawPixels);
    return texture;
}

std::uint8_t ToUnorm8(float value)
{
    // NaN fails both comparisons and ends up 0.
    const float clamped = value > 0.0f ? std::min(value, 1.0f) : 0.0f;
    return static_cast<std::uint8_t>(std::lround(clamped * 255.0f));
}

float LinearToSrgb(float value)
{
    return value <= 0.0031308f ? value * 12.92f : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
}

// What a float image looks like through the RGBA8 path: clamped to [0, 1], colour sRGB-encoded.
TextureData ToDisplayRgba8(const FloatTextureData& image)
{
    TextureData texture{};
    texture.width = image.width;
    texture.height = image.height;
    texture.channelCount = 4;
    texture.pixels.resize(image.pixels.size());
    for (size_t index = 0; index < image.pixels.size(); ++index)
    {
        const float value = image.pixels[index];
        const bool isAlpha = index % 4 == 3;
        const float clamped = value > 0.0f ? std::min(value, 1.0f) : 0.0f;
        texture.pixels[index] = ToUnorm8(isAlpha ? clamped : LinearToSrgb(clamped));
    }
    return texture;
}
```

Change `IsPortableMapExtension` to use `LowerExtension(path)`. Add the members:

```cpp
bool TextureLoader::IsFloatImageFile(const std::filesystem::path& path)
{
    const std::string ext = LowerExtension(path);
    return ext == ".hdr" || ext == ".exr";
}

FloatTextureData TextureLoader::LoadRGBA32F(const std::string& path)
{
    if (!IsFloatImageFile(path))
    {
        throw std::runtime_error("'" + path + "' is not a floating-point image (.hdr or .exr)");
    }
    return LowerExtension(path) == ".exr" ? LoadOpenExr(path) : LoadRadianceHdr(path);
}
```

At the top of `LoadRGBA8`, before `stbi_set_flip_vertically_on_load`:

```cpp
    if (IsFloatImageFile(path))
    {
        TextureData texture = ToDisplayRgba8(LoadRGBA32F(path));
        if (flipVertically)
        {
            FlipRows(texture);
        }
        return texture;
    }
```

and replace the inline flip loop of the portable-pixmap branch with `FlipRows(texture);`.

- [ ] **Step 5: Run to verify it passes.** Build, then `ctest --test-dir out/build/vs2026-x64 -C Debug -R "float_textures|texture_" --output-on-failure`. Expected: `miniengine.float_textures`, `miniengine.texture_compression`, `miniengine.texture_preparation` PASS. If `LoadsOpenExr` fails on channel order or value, print the loaded values and read tinyexr's `LoadEXR` documentation in `.deps/vcpkg_installed/x64/x64-windows/include/tinyexr.h` before changing anything. Run the format check.

- [ ] **Step 6: Commit.**

```bash
git add vcpkg.json cmake/MiniEngineDependencies.cmake engine/asset/CMakeLists.txt engine/asset/texture_loader.h engine/asset/texture_loader.cpp tests/float_texture_tests.cpp tests/CMakeLists.txt
git commit -m "feat(asset): decode .hdr and .exr as float images

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

(Include `cmake/vcpkg-overlay-ports/tinyexr/` and `README.md` if Step 1 needed the overlay.)

---

### Task 2: Half-float packing and preparation routing

**Files:**
- Modify: `engine/asset/texture_loader.h`, `engine/asset/texture_loader.cpp`
- Modify: `engine/asset/texture_preparation.h`, `engine/asset/texture_preparation.cpp`
- Test: `tests/float_texture_tests.cpp`, `tests/CMakeLists.txt` (nothing new to link)

**Interfaces:**
- Consumes: `FloatTextureData`, `TextureLoader::IsFloatImageFile`, `TextureLoader::LoadRGBA32F` (Task 1).
- Produces: `me::HalfFloatTextureData { int width; int height; std::vector<std::uint16_t> texels; bool IsValid() const; }`,
  `std::uint16_t me::PackHalfFloat(float)`, `me::HalfFloatTextureData me::PackRgba16Float(const FloatTextureData&)`,
  `me::PreparedTexture::halfFloat` (`std::optional<HalfFloatTextureData>`).

- [ ] **Step 1: Write the failing tests.** In `tests/float_texture_tests.cpp` add `#include <engine/asset/texture_preparation.h>` and `#include <limits>`, then these functions before the closing `}` of the anonymous namespace:

```cpp
void PacksHalfFloats()
{
    const auto hex = [](std::uint16_t value)
    {
        char text[8] = {};
        std::snprintf(text, sizeof(text), "0x%04X", value);
        return std::string(text);
    };
    const auto expect = [&](float input, std::uint16_t expected, const std::string& label)
    {
        const std::uint16_t actual = PackHalfFloat(input);
        Require(actual == expected, label + " packs to " + hex(actual) + ", expected " + hex(expected));
    };
    expect(0.0f, 0x0000, "0");
    expect(1.0f, 0x3C00, "1");
    expect(-2.0f, 0xC000, "-2");
    expect(65504.0f, 0x7BFF, "65504");
    expect(1.0e6f, 0x7BFF, "1e6");
    expect(std::numeric_limits<float>::infinity(), 0x7BFF, "+inf");
    expect(-std::numeric_limits<float>::infinity(), 0xFBFF, "-inf");
    expect(std::numeric_limits<float>::quiet_NaN(), 0x0000, "NaN");
    expect(std::ldexp(1.0f, -15), 0x0200, "the subnormal 2^-15");
}

void PacksWholeImages()
{
    FloatTextureData image{};
    image.width = 2;
    image.height = 1;
    image.pixels = {1.0f, -2.0f, 1.0e6f, 1.0f, 0.0f, 0.5f, 0.25f, 0.0f};
    const HalfFloatTextureData packed = PackRgba16Float(image);
    Require(packed.IsValid() && packed.width == 2 && packed.height == 1, "packing keeps the size");
    const std::vector<std::uint16_t> expected = {0x3C00, 0xC000, 0x7BFF, 0x3C00, 0x0000, 0x3800, 0x3400, 0x0000};
    Require(packed.texels == expected, "packing converts every channel in order");
}

void PreparesFloatFilesUncompressed()
{
    ScratchDirectory images;
    ScratchDirectory cache;
    const std::filesystem::path hdr = WriteHdr(images.Path());
    const PreparedTexture prepared = PrepareTexture(hdr.string(), TextureUsage::Color, true, cache.Path());
    Require(prepared.halfFloat.has_value() && prepared.halfFloat->IsValid(), "a float file prepares as half floats");
    Require(!prepared.compressed.has_value(), "a float file is never block-compressed");
    Require(prepared.rgba.pixels.empty(), "a float file has no RGBA8 form");
    Require(prepared.halfFloat->texels[3 * 4] == 0x63D0, "1000 survives as half 1000 (0x63D0)");
    Require(std::filesystem::is_empty(cache.Path()), "a float file writes nothing to the texture cache");
}
```

Add `#include <cstdio>` for `snprintf`. Add the three calls to `main()` after `RejectsWhatIsNotAFloatImage();`.

- [ ] **Step 2: Run to verify it fails.** Build. Expected: compile errors for `PackHalfFloat`, `PackRgba16Float`, `HalfFloatTextureData` and `PreparedTexture::halfFloat`.

- [ ] **Step 3: Implement packing.** In `engine/asset/texture_loader.h`, after `FloatTextureData`:

```cpp
// Linear RGBA16F, four IEEE 754 half floats per texel, rows top-down: the texels a float material
// texture uploads.
struct HalfFloatTextureData
{
    int width = 0;
    int height = 0;
    std::vector<std::uint16_t> texels;

    bool IsValid() const
    {
        return width > 0 && height > 0 &&
               texels.size() == static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    }
};
```

and after the `TextureLoader` class:

```cpp
// Float to IEEE half. NaN becomes 0; everything else, infinities included, clamps to
// [-65504, 65504], half's largest finite value, keeping its sign.
std::uint16_t PackHalfFloat(float value);
HalfFloatTextureData PackRgba16Float(const FloatTextureData& image);
```

In `texture_loader.cpp` add `#include <glm/gtc/packing.hpp>` and:

```cpp
std::uint16_t PackHalfFloat(float value)
{
    if (std::isnan(value))
    {
        return 0;
    }
    constexpr float kHalfMax = 65504.0f;
    return glm::packHalf1x16(std::clamp(value, -kHalfMax, kHalfMax));
}

HalfFloatTextureData PackRgba16Float(const FloatTextureData& image)
{
    HalfFloatTextureData packed{};
    packed.width = image.width;
    packed.height = image.height;
    packed.texels.resize(image.pixels.size());
    std::transform(image.pixels.begin(), image.pixels.end(), packed.texels.begin(), PackHalfFloat);
    return packed;
}
```

If `glm::packHalf1x16` rounds a value differently from the expectations in `PacksHalfFloats` (glm truncates some subnormals), keep the test and replace the call with a hand-written round-to-nearest-even conversion; the test is the contract.

- [ ] **Step 4: Implement routing.** In `engine/asset/texture_preparation.h` replace the `PreparedTexture` comment and struct:

```cpp
// A material texture file ready to upload. Exactly one form is filled: block-compressed when the
// device samples BC formats, half floats for .hdr/.exr files, RGBA8 otherwise.
struct PreparedTexture
{
    std::optional<CompressedTexture> compressed;
    std::optional<HalfFloatTextureData> halfFloat;
    TextureData rgba;
    bool fromCache = false;
    double compressSeconds = 0.0;
};
```

Extend the `PrepareTexture` comment with: "A float image (.hdr, .exr) is packed to RGBA16F and never compressed or cached." In `texture_preparation.cpp`, at the top of `PrepareTexture` after `PreparedTexture prepared{};`:

```cpp
    // No BC6H encoder exists here, and decoding a float file is cheap next to encoding, so float
    // files skip both the compressor and the disk cache whatever the device supports.
    if (TextureLoader::IsFloatImageFile(path))
    {
        prepared.halfFloat = PackRgba16Float(TextureLoader::LoadRGBA32F(path));
        return prepared;
    }
```

- [ ] **Step 5: Run to verify it passes.** Build, `ctest --test-dir out/build/vs2026-x64 -C Debug -R "float_textures|texture_" --output-on-failure`: all PASS. Format check passes.

- [ ] **Step 6: Commit.**

```bash
git add engine/asset/texture_loader.h engine/asset/texture_loader.cpp engine/asset/texture_preparation.h engine/asset/texture_preparation.cpp tests/float_texture_tests.cpp
git commit -m "feat(asset): prepare float textures as RGBA16F

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: Half-float GPU upload, stats, `.exr` in asset lists

**Files:**
- Modify: `engine/renderer/vulkan/texture.h`, `engine/renderer/vulkan/texture.cpp`
- Modify: `engine/renderer/vulkan/renderer.h:177-183`, `engine/renderer/vulkan/renderer.cpp` (`UploadPreparedTexture` ~1253, summary log ~1099)
- Modify: `engine/asset/asset_manager.cpp:58-62`, `engine/asset/asset_registry.cpp:78-83`, `engine/platform/file_dialog/file_dialog_backend.cpp:96`
- Modify: `README.md` (after the block-compression paragraph, line ~156)

**Interfaces:**
- Consumes: `HalfFloatTextureData`, `PreparedTexture::halfFloat` (Task 2).
- Produces: `VulkanTexture(VkPhysicalDevice, VkDevice, const HalfFloatTextureData&, VulkanUploadBatch&)`; `TextureUploadStats::floatTextures`.

No unit test: `VulkanTexture` needs a device and there is no GPU test harness. Verification is the build, the full suite and the validation run here, plus Task 4.

- [ ] **Step 1: Generalise the upload routine.** In `texture.h` replace the declaration `void UploadTexture(const TextureData& textureData, VulkanUploadBatch& uploadBatch);` with:

```cpp
    // Uploads level 0 from tightly packed texels and builds the mip chain with linear blits when the
    // format supports them, else keeps a single level. Shared by the RGBA8 and half-float paths.
    void UploadTexels(const void* texels, VkDeviceSize byteCount, uint32_t width, uint32_t height, VkFormat vkFormat, VulkanUploadBatch& uploadBatch);
```

and add the constructor after the `TextureData` one:

```cpp
    // Uploads a linear RGBA16F image as R16G16B16A16_SFLOAT with GPU-built mips. The format has no
    // sRGB variant and needs none: float images are scene-linear whatever slot samples them.
    VulkanTexture(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        const HalfFloatTextureData& textureData,
        VulkanUploadBatch& uploadBatch);
```

In `texture.cpp`, turn the body of `UploadTexture` into `UploadTexels`: drop the `IsValid` check and the `imageSize` computation (use `byteCount`), take `vkFormat` from the parameter instead of `GetVkFormat()`, use `width`/`height` for the mip count, `CreateImage`, `CopyBufferToImage` and `GenerateMipmaps` (cast to `int32_t` where `GenerateMipmaps` takes `int32_t`). The `TextureData` constructor and the path constructor then call:

```cpp
// in VulkanTexture(..., const TextureData& textureData, ...) and the path constructor
if (!textureData.IsValid())
{
    throw std::runtime_error("Cannot create Vulkan texture from invalid pixel data");
}
UploadTexels(
    textureData.pixels.data(),
    static_cast<VkDeviceSize>(textureData.pixels.size()),
    static_cast<uint32_t>(textureData.width),
    static_cast<uint32_t>(textureData.height),
    GetVkFormat(),
    uploadBatch);
```

(the path constructor loads into a local `const TextureData textureData = TextureLoader::LoadRGBA8(path);` first). Note `pixels.size()` equals `width * height * 4` for valid RGBA8 data, which is what the old code computed. New constructor:

```cpp
VulkanTexture::VulkanTexture(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    const HalfFloatTextureData& textureData,
    VulkanUploadBatch& uploadBatch)
    : m_physicalDevice(physicalDevice),
      m_device(device),
      m_textureFormat(VulkanTextureFormat::LinearData)
{
    try
    {
        if (!textureData.IsValid())
        {
            throw std::runtime_error("Cannot create Vulkan texture from invalid half-float data");
        }
        UploadTexels(
            textureData.texels.data(),
            static_cast<VkDeviceSize>(textureData.texels.size() * sizeof(std::uint16_t)),
            static_cast<uint32_t>(textureData.width),
            static_cast<uint32_t>(textureData.height),
            VK_FORMAT_R16G16B16A16_SFLOAT,
            uploadBatch);
    }
    catch (...)
    {
        DestroyHandles();
        throw;
    }
}
```

Keep the existing try/catch shape in the other constructors (the `IsValid` throw goes inside their `try`).

- [ ] **Step 2: Upload prepared half floats and count them.** `renderer.h`, in `TextureUploadStats` after `uncompressed`: `size_t floatTextures = 0;`. `renderer.cpp`, at the top of `UploadPreparedTexture` after the `stats` reference:

```cpp
    if (prepared.halfFloat)
    {
        ++stats.floatTextures;
        return std::make_unique<VulkanTexture>(
            m_device->GetPhysicalDevice(), m_device->GetHandle(),
            *prepared.halfFloat, uploadBatch);
    }
```

and the summary log:

```cpp
    if (stats.fromCache + stats.compressedNow + stats.uncompressed + stats.floatTextures > 0)
    {
        LOG_INFO(
            "Texture files: {} block-compressed from the cache, {} compressed now ({:.1f} s of encoding across threads), {} uncompressed, {} float",
            stats.fromCache,
            stats.compressedNow,
            stats.compressSeconds,
            stats.uncompressed,
            stats.floatTextures);
    }
```

- [ ] **Step 3: List `.exr` everywhere `.hdr` is listed.** `asset_manager.cpp` `IsTextureExt` and `asset_registry.cpp` `HasRegistrableExtension`: `ext == ".hdr" || ext == ".exr" || ext == ".dds"`. `file_dialog_backend.cpp`: `*.hdr;*.exr;*.dds` in the texture filter. Check `git grep -n '"\.hdr"'` shows no other list.

- [ ] **Step 4: README.** After the block-compressed paragraph (line ~156) add:

```markdown
- `.hdr`（stb）与 `.exr`（tinyexr）按扩展名识别为浮点贴图：解码为线性 RGBA32F，在工作线程打包成 RGBA16F，以 `R16G16B16A16_SFLOAT` 上传并在 GPU 上生成 mip；不做块压缩、不进磁盘缓存，任何材质槽都按线性值采样（不做 sRGB 解码），自发光因此可以取大于 1 的值。软件预览经 `LoadRGBA8` 看到的是截断到 [0, 1] 并 sRGB 编码后的版本。设计见 [docs/superpowers/specs/2026-09-23-float-textures-design.md](docs/superpowers/specs/2026-09-23-float-textures-design.md)。
```

- [ ] **Step 5: Verify.** Build Debug; full `ctest` passes; format check passes; validation run (`--frames 300`) prints zero validation messages; then again with `--model assets\Sponza\<the Sponza .gltf>` (find it with `git ls-files assets/Sponza | grep gltf`) and confirm the log line now ends with `, 0 float` and nothing else changed.

- [ ] **Step 6: Commit.**

```bash
git add engine/renderer/vulkan/texture.h engine/renderer/vulkan/texture.cpp engine/renderer/vulkan/renderer.h engine/renderer/vulkan/renderer.cpp engine/asset/asset_manager.cpp engine/asset/asset_registry.cpp engine/platform/file_dialog/file_dialog_backend.cpp README.md
git commit -m "feat(renderer): upload float textures as RGBA16F

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 4: Float texture acceptance scene

Throwaway fixtures under the session scratchpad; nothing here is committed.

**Files:**
- Create (scratchpad): `make_float_fixtures.py`, output directory `float_fixture/` holding `emissive.hdr`, `emissive.exr`, `quad_hdr.gltf`, `quad_exr.gltf`, `quad_ldr.gltf`, `white.png`.

- [ ] **Step 1: Write the generator.** A Python 3 script (standard library only) that writes:
  - `emissive.hdr`: 64x64 Radiance file: header `#?RADIANCE
FORMAT=32-bit_rle_rgbe

-Y 64 +X 64
`, then
    64 * 64 flat RGBE texels, row 0 first. stb reads a scanline as flat unless it starts with the
    RLE marker `2, 2, <high byte>`, so flat data is valid as long as no scanline starts with two 2s
    (the ramp starts at 0, which encodes as `0, 0, 0, 0`). RGBE of a grey value v > 0: `frexp(v)` gives
    mantissa m and exponent e; bytes `int(m * 256)` three times, then `e + 128`. Content: a horizontal
    ramp from 0 to 16 in linear radiance, grey.
  - `emissive.exr`: same ramp, 64x64, uncompressed scanline OpenEXR with channels `B`, `G`, `R` (alphabetical, as the format requires) in FLOAT (pixel type 2), attributes `channels`, `compression` (0), `dataWindow`, `displayWindow`, `lineOrder` (0), `pixelAspectRatio`, `screenWindowCenter`, `screenWindowWidth`, the offset table, then one block per scanline (`int32 y`, `int32 byteCount`, then B, G, R rows).
  - `white.png`: 64x64 white (use `zlib` + `struct` to write it).
  - Three glTF files, each one 1x1 quad (two triangles, positions, normals, UVs in an embedded base64 buffer) with a material whose `baseColorFactor` is black, `emissiveFactor` [1, 1, 1], and `emissiveTexture` pointing at the `.hdr`, the `.exr` and the `.png` respectively.
  Run it; then verify the `.hdr` and `.exr` load through the engine by temporarily pointing a scratch copy of the Task 1 test at them, or simply by Step 2's log.

- [ ] **Step 2: Load each quad.** For each glTF: `out\build\vs2026-x64\app\Debug\miniengine_app.exe --backend vulkan --frames 300 --model <scratchpad>\float_fixture\quad_hdr.gltf`. Expected in the log: the import copy under `assets/models/`, `Texture files: ... 1 float` for the `.hdr` and `.exr` quads and `... 0 float` for the PNG quad, no `Failed to load model texture`, zero validation messages. Then delete the imported copies under `assets/models/` so nothing leaks into the repository (`git status` must show only the two files the constraints name).

- [ ] **Step 3: Report the manual checks to the user** (the acceptance list of the spec) as a checklist in Chinese: open each quad in the editor; the emissive debug view of the float quads reaches well above the PNG quad's white at the bright end of the ramp; the gradient shows no banding; assigning `emissive.hdr` in the material editor shows it in the model preview. Record anything measured.
