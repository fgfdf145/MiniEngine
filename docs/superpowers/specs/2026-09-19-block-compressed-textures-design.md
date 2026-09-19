# Block-Compressed Textures Design

## Goal

Store material textures on the GPU in BC formats instead of RGBA8, cutting their memory by four
(BC7, BC5) so a scene like NewSponza plus its curtains fits in an 8 GB GPU. Compression happens the
first time a texture file is loaded, and the result is cached on disk so later loads skip both the
PNG/JPEG decode and the encode.

## Motivation

Measured on an RTX 4070 Laptop GPU (8188 MiB): NewSponza alone occupies about 6.2 GB, nearly all of
it 79 uncompressed textures with GPU-generated mip chains (a 4096x4096 RGBA8 texture with mips is
about 89 MB). Adding NewSponza_Curtains ran the process out of device memory; commit `69c77f5` made
that failure recoverable, and this design removes its cause for scenes of this size.

## Current State

- `TextureLoader::LoadRGBA8` (`engine/asset/texture_loader.h`) decodes any image to RGBA8 with stb.
- `VulkanTexture` (`engine/renderer/vulkan/texture.*`) uploads one RGBA8 level into an
  `R8G8B8A8_SRGB` or `_UNORM` image and builds the mip chain on the GPU with linear blits.
- `VulkanRenderer::UploadSceneResources` decodes every uncached material texture in parallel
  chunks (`std::async`), then uploads them. Live textures are reused by the cache key
  `path|srgb` or `path|linear`.
- Material shaders (`triangle.frag`, `gbuffer.frag`) sample normals as `texture(n).xyz * 2 - 1`.
- The device enables only `samplerAnisotropy` among the core features.
- `EnginePaths::CacheRoot()` is an existing, disposable per-build cache directory.

## Locked Decisions

1. **Compress on first load, cache under `CacheRoot()/textures/`.** Assets stay untouched; the
   cache can be deleted at any time and rebuilds itself.
2. **Encoder: bc7enc_rdo** (richgel999, MIT or public domain), pinned to commit
   `b9438627eef73a1157e84201b6fa6eb2ffd6d9f0` through a vcpkg overlay port, like tinygltf. Only
   `bc7enc.cpp` (BC7), `rgbcx.cpp` (BC4/BC5) and `bc7decomp.cpp` (BC7 decode, for tests) are built,
   as a static library; the ISPC encoder, the RDO encoder and the sample executable are not.
3. **Format by usage**, where the usage comes from the material slot:

   | Usage | Slots | Format | Encoder settings |
   |---|---|---|---|
   | `Color` | base color, emissive (and their secondaries) | `BC7_SRGB_BLOCK` | perceptual, uber level 0, alpha kept |
   | `Normal` | normal (and secondary) | `BC5_UNORM_BLOCK` | R and G only |
   | `Data` | metallic, roughness, occlusion, blend mask (and secondaries) | `BC7_UNORM_BLOCK` | linear weights, uber level 0 |

   Data textures stay four-channel because glTF packs metallic and roughness (and often
   occlusion) into one file that different slots sample different channels of.
4. **Normals are reconstructed in the shader:** `xy = n.rg * 2 - 1`,
   `z = sqrt(max(1 - dot(xy, xy), 0))`. The same code is exact for the uncompressed fallback,
   since stored normal maps hold unit vectors, so there is one shader path, not two.
5. **Mip chains are built on the CPU** before encoding, down to 1x1, with stb_image_resize2:
   sRGB-correct filtering for `Color`, linear for `Normal` and `Data`. BC images cannot be blit.
6. **Fallback:** if the device lacks `textureCompressionBC`, or any of the three formats lacks
   sampled-image and linear-filter support, every texture keeps today's RGBA8 path. The decision is
   made once at device creation and logged.
7. **Built-in textures** (the 1x1 defaults and any `TextureData` created in memory) stay RGBA8.

## Components

### `engine/asset/texture_compression.{h,cpp}` (pure CPU, no Vulkan)

```cpp
enum class TextureUsage { Color, Normal, Data };
enum class CompressedTextureFormat : uint32_t { Bc7Srgb = 1, Bc7Unorm = 2, Bc5Unorm = 3 };

struct CompressedTextureLevel { uint32_t width, height; std::vector<uint8_t> blocks; };
struct CompressedTexture { CompressedTextureFormat format; std::vector<CompressedTextureLevel> levels; };

CompressedTextureFormat FormatForUsage(TextureUsage usage);
// Mip chain of an RGBA8 image, level 0 first, down to 1x1.
std::vector<TextureData> BuildMipChain(const TextureData& image, TextureUsage usage);
// Encodes every level. Partial edge blocks repeat the last row and column.
CompressedTexture CompressTexture(const TextureData& image, TextureUsage usage);
```

`CompressTexture` calls `bc7enc_compress_block_init` and `rgbcx::init` once per process
(`std::call_once`); both encoders are otherwise thread-safe per block, so textures encode in
parallel.

### `engine/asset/compressed_texture_cache.{h,cpp}` (pure CPU)

```cpp
// Loads the cached compressed form of an image file, or builds and stores it.
CompressedTexture LoadOrCompressTexture(const std::filesystem::path& imagePath, TextureUsage usage,
                                        const std::filesystem::path& cacheDirectory);
```

- **Key:** the canonical absolute path, file size, last write time, usage and
  `kTextureCacheVersion` (bumped whenever encoder settings or the file layout change), joined into
  a string. The file name is the 64-bit FNV-1a hash of the key in hex plus `.metex`.
- **File layout** (little endian): magic `METX`, version, format, level count, key length, key
  bytes, then per level width, height, byte count and the blocks. The stored key must equal the
  requested one, which turns a hash collision into a miss.
- **Writes are atomic:** a temporary file in the same directory, then a rename. Two editors
  compressing the same texture at once cannot leave a torn file.
- **Any read problem** (missing, short, wrong magic, version or key) is a miss: the texture is
  compressed again and the file replaced. A failed write is logged and ignored; the texture is
  still returned.

### Renderer

- `VulkanDevice` enables `textureCompressionBC` when supported and exposes
  `SupportsBlockCompression()` (feature plus format support for all three formats).
- `VulkanTexture` gains a constructor taking a `CompressedTexture`: one image with the BC format
  and the given level count, one staging buffer holding every level, one
  `vkCmdCopyBufferToImage` with a region per level, then a transition to shader read. No blits.
- `UploadSceneResources`: the material slot decides the usage. The cache key becomes
  `path|color`, `path|normal` or `path|data` (formerly `|srgb`/`|linear`), so a normal map and
  a data texture from the same file are separate textures. The prefetch step calls
  `LoadOrCompressTexture` instead of `LoadRGBA8` when compression is supported; the parallel
  chunk size drops from twice to once the hardware thread count, because each task now holds a
  mip chain as well as the decoded image. A texture that fails to compress falls back to the RGBA8
  path for that texture alone, with a logged error.
- Progress: one log line per compressed (cache-missed) texture with its encode time, and a summary
  of hits and misses per upload.

### Shaders

`triangle.frag` and `gbuffer.frag` reconstruct the normal's z from its xy as in decision 4, for the
primary and secondary normal maps alike. Nothing else in shading changes.

## Failure Handling

- Unsupported BC: RGBA8 path for everything (decision 6).
- Image decode failure: as today, the slot falls back to its default texture.
- Compression or cache failure for one texture: RGBA8 path for that texture.
- Running out of GPU memory: unchanged from `69c77f5`, the upload is abandoned and the previous
  scene stays.

## Automated Verification

New test target `miniengine.texture_compression`:

1. `BuildMipChain`: level counts and sizes for 4096x4096, 5x3 and 1x1; an sRGB 2x2 of black and
   white averages to sRGB 188, not 128.
2. BC7 round trip (decode with `bc7decomp`) of a smooth synthetic image: PSNR above 40 dB; alpha
   preserved in a half-transparent image.
3. BC5 round trip (decode with `rgbcx::unpack_bc5`): channel error below 2/255 on a gradient.
4. Non-multiple-of-four sizes encode the right block count and decode without reading past the
   image.
5. Cache: a miss writes the file and a second call hits without encoding; a changed last write
   time, a different usage, a truncated file or a wrong key each produce a miss; the file name is
   stable for the same key.

Plus the existing suite, and zero validation messages in Debug.

## Manual and Measured Acceptance

1. Sponza GPU memory drops from about 6.2 GB to under 2.5 GB (`nvidia-smi` before and after).
2. The first Sponza load reports its compression time; the second is no slower than today.
3. Sponza plus curtains loads without the out-of-memory report.
4. Side-by-side captures of Sponza, compressed and uncompressed, show no visible difference in
   the shaded image; tone-mapped PSNR above 38 dB.
5. Mask materials (foliage, railings) keep their cutouts.

## Out of Scope

- Incremental upload (re-uploading only what changed) and the double-geometry peak.
- Compressing in the background while the editor stays interactive.
- GPU-side encoding, BC1/BC4 for suitable textures, RDO, and KTX2 or DDS output.
- Compressing built-in or procedurally created textures.
