# Float Textures Design

## Goal

Load `.hdr` and `.exr` images as floating-point textures instead of truncating them to RGBA8, and
sample them from any material slot as `R16G16B16A16_SFLOAT`. An emissive texture can then carry
radiance above 1.0, and the loader becomes the input stage of image-based lighting.

## Roadmap

This is phase 1 of four toward image-based lighting; each later phase gets its own spec.

1. **Float textures** (this spec): decode, CPU representation, upload, material slots.
2. **Environment and sky:** a scene-level equirectangular environment, converted to a cubemap on
   the GPU, shown as the background, with an intensity in physical units, saved with the scene
   and edited in the editor.
3. **Diffuse IBL:** irradiance from the environment (SH9), replacing the constant ambient term,
   attenuated by the existing VBAO.
4. **Specular IBL:** a GGX-prefiltered cubemap mip chain and a BRDF LUT (split sum).

## Current State

- `TextureLoader::LoadRGBA8` (`engine/asset/texture_loader.h`) decodes every file with
  `stbi_load`. A `.hdr` file is tone-clamped to 8 bits by stb; `.exr` cannot be read at all.
- The asset browser, the registry and the texture file dialog already list `.hdr`, not `.exr`.
- `PrepareTexture` (`engine/asset/texture_preparation.cpp`) runs on worker threads and returns a
  `PreparedTexture`: BC-compressed through the disk cache when the device supports BC, RGBA8
  otherwise. `VulkanRenderer::UploadPreparedTexture` uploads it on the frame thread.
- `VulkanTexture` uploads RGBA8 into `R8G8B8A8_SRGB` or `_UNORM` and builds mips with GPU blits,
  or uploads a prepared BC mip chain.
- Emissive is `texel * emissiveColor * emissiveIntensity`, added directly to radiance in cd/m^2
  (`triangle.frag`, `gbuffer.frag` into the `B10G11R11_UFLOAT` GB3, then `deferred_lighting.frag`).
  Values above 1.0 already flow through unchanged.
- `editor_model_preview.cpp` software-samples material textures through `LoadRGBA8`.

## Locked Decisions

1. **Formats:** `.hdr` (Radiance RGBE) through stb's `stbi_loadf`; `.exr` through **tinyexr 3.1.0**
   (vcpkg port, BSD-3-Clause, pulls in miniz), linked privately into `engine_asset`.
2. **Detection by extension**, case-insensitive: `.hdr` and `.exr` are float images, everything
   else is not. The asset lists already classify files by extension, so the two stay in step.
3. **Lossless on the CPU, half on the GPU.** The loader returns linear RGBA32F
   (`FloatTextureData`). Material textures are packed to RGBA16F on the worker thread and uploaded
   as `R16G16B16A16_SFLOAT`. Half floats cover up to 65504 with an 11-bit mantissa, enough for
   material textures; a later phase that needs more range (an unclipped sun in an environment map)
   takes the RGBA32F data and chooses its own GPU format.
4. **Packing rule:** NaN becomes 0, and finite values and infinities clamp to [-65504, 65504]. The
   sign is kept, since a data or normal EXR may be signed; negative colour is the author's problem.
5. **Float textures are never block-compressed** and never go through the disk cache: there is no
   BC6H encoder in the project, and decoding `.hdr`/`.exr` needs no encode step worth caching. They
   are uploaded uncompressed whether or not the device supports BC.
6. **Usage does not change a float texture's format.** Float images are scene-linear by
   definition, so `Color` gets no sRGB decode, and `Normal` and `Data` sample the values as stored.
   The usage still separates live textures (`path|color` and `path|data` are different textures),
   exactly as today.
7. **Mips on the GPU** with the existing blit path. Linear-filtered blits of `R16G16B16A16_SFLOAT`
   are mandatory in Vulkan, and the existing `FormatSupportsLinearBlit` check still guards it.
8. **Channels:** `.hdr` has no alpha and gets 1.0. For `.exr`, tinyexr's `LoadEXR` supplies RGBA
   (a missing alpha is 1.0, a single-channel image is replicated to RGB).
9. **`LoadRGBA8` still reads every supported file.** For `.hdr`/`.exr` it decodes to float, clamps
   to [0, 1] and applies the sRGB transfer function to RGB (alpha stays linear), so the software
   preview shows these files the way the renderer's sRGB path would show an LDR copy.
10. **Rows top-down**, like every other texture (stb and tinyexr both return top-down rows).

## Components

### `engine/asset/texture_loader.{h,cpp}`

```cpp
// Linear RGBA, four floats per texel, rows top-down.
struct FloatTextureData
{
    int width = 0;
    int height = 0;
    std::vector<float> pixels;
    bool IsValid() const;  // positive size and width * height * 4 floats
};

// Linear RGBA16F, four IEEE half floats per texel, rows top-down: what the GPU samples.
struct HalfFloatTextureData
{
    int width = 0;
    int height = 0;
    std::vector<std::uint16_t> texels;
    bool IsValid() const;
};

class TextureLoader
{
  public:
    static bool IsFloatImageFile(const std::filesystem::path& path);   // .hdr / .exr
    static FloatTextureData LoadRGBA32F(const std::string& path);      // throws for other files
    static TextureData LoadRGBA8(const std::string& path, bool flipVertically = false);
};

// Float to half with the packing rule of decision 4.
std::uint16_t PackHalfFloat(float value);
HalfFloatTextureData PackRgba16Float(const FloatTextureData& image);
```

`LoadRGBA32F` throws `std::runtime_error` with the file name and the decoder's reason on any
failure (missing file, corrupt data, unsupported EXR layout such as deep or tiled multipart that
`LoadEXR` rejects).

### `engine/asset/texture_preparation.{h,cpp}`

`PreparedTexture` gains `std::optional<HalfFloatTextureData> halfFloat`. `PrepareTexture` checks
`IsFloatImageFile` first: a float file is loaded with `LoadRGBA32F`, packed with
`PackRgba16Float`, and returned without touching the compressor or the disk cache. Exactly one of
`compressed`, `halfFloat` or `rgba` is filled.

### `engine/renderer/vulkan/texture.{h,cpp}`

- A fourth constructor takes a `HalfFloatTextureData` and uploads it as `R16G16B16A16_SFLOAT`.
- `UploadTexture` becomes a shared routine over raw texel bytes, width, height and `VkFormat`
  (staging buffer, level-0 copy, blit mips or a single level), used by the RGBA8 and the half-float
  constructors alike.
- Same sampler as every other material texture.

### `engine/renderer/vulkan/renderer.cpp`

`UploadPreparedTexture` uploads `halfFloat` when present and counts it in a new `floatTextures`
field of `TextureUploadStats`, which the per-upload summary log reports next to the cache hits,
fresh compressions and uncompressed textures.

### Asset lists and dialogs

`.exr` joins `.hdr` in `IsTextureExt` (`asset_manager.cpp`), `HasRegistrableExtension`
(`asset_registry.cpp`) and the texture filter of `file_dialog_backend.cpp`.

### Build

`vcpkg.json` adds `tinyexr`; `cmake/MiniEngineDependencies.cmake` adds
`find_package(tinyexr CONFIG REQUIRED)`; `engine_asset` links `unofficial::tinyexr::tinyexr`
privately. The tests that write EXR fixtures link it too.

## Failure Handling

- A float file that fails to decode throws in `PrepareTexture`, like any undecodable texture: the
  slot falls back to its default texture and the error is logged once.
- Running out of GPU memory: unchanged, the upload is abandoned and the previous scene stays.
- Memory cost is real: a 4096x4096 RGBA16F texture with mips is about 170 MB, twice an RGBA8 one
  and eight times BC7. That is accepted for this phase; float material textures are rare.

## Automated Verification

New test target `miniengine.float_textures` (`tests/float_texture_tests.cpp`):

1. `IsFloatImageFile`: `.hdr`, `.HDR`, `.exr`, `.Exr` are float; `.png`, `.jpg`, no extension are
   not.
2. `.hdr` round trip: a 3x2 image written with `stbi_write_hdr`, holding 0.25, 1.0, 3.5 and 1000,
   reads back within RGBE precision (1% relative), alpha 1, row 0 first.
3. `.exr` round trip: a 3x2 RGBA float image written with tinyexr's `SaveEXR`, with values above 1
   and alpha 0.5, reads back within half precision (the saver may store half).
4. `LoadRGBA8` of the `.hdr` fixture: 1.0 is 255, 0.25 is sRGB 137, 1000 clamps to 255, alpha 255.
5. `PackHalfFloat`: 1.0 is `0x3C00`, -2.0 is `0xC000`, 65504 is `0x7BFF`, 1e6 and +infinity are
   `0x7BFF`, -infinity is `0xFBFF`, NaN is 0, and 0.5 * 2^-14 (a subnormal) is `0x0200`.
6. `PrepareTexture` of the `.hdr` fixture with compression on: `halfFloat` is set, `compressed`
   and `rgba` are empty, and the cache directory stays empty.
7. `LoadRGBA32F` of a truncated `.exr` and of a `.png` both throw.

Plus the existing suite, and zero validation messages in Debug.

## Manual and Measured Acceptance

1. A `.hdr` and an `.exr` assigned as the emissive texture of a material light the viewport above
   the white of an equivalent LDR texture: the emissive debug view and the exposure histogram both
   show values above 1.0 times the emissive intensity.
2. A float texture as base colour shows no banding in a smooth gradient.
3. The upload summary log counts float textures.
4. Assigning a `.hdr` in the material editor shows it in the model preview.
5. No validation messages while loading, reassigning and removing float textures.

## Out of Scope

- BC6H compression and disk caching of float textures.
- Environment maps, cubemaps, sky and IBL (phases 2 to 4).
- HDR display output (HDR10, scRGB).
- The pre-existing UNORM swapchain fallback without an sRGB encode, and ImGui colours on the sRGB
  swapchain.
