# Block-Compressed Textures Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Upload file-backed material textures as BC7/BC5, compressed on first load and cached on disk, so NewSponza plus its curtains fits in 8 GB of GPU memory.

**Architecture:** A pure CPU unit in `engine_asset` builds mip chains and encodes them with bc7enc_rdo; a second unit caches the result under `CacheRoot()/textures/`. The renderer asks the device whether BC is usable, prepares each texture on the existing parallel prefetch path, and uploads either the compressed levels or today's RGBA8 image.

**Tech Stack:** C++20, Vulkan 1.3, vcpkg overlay port, bc7enc_rdo, stb_image / stb_image_resize2 / stb_image_write, GLSL.

**Spec:** [docs/superpowers/specs/2026-09-19-block-compressed-textures-design.md](../specs/2026-09-19-block-compressed-textures-design.md)

## Global Constraints

- C++20, Allman braces, `namespace me`; `scripts/check-format.ps1` passes before each commit (stage new files first: the check covers tracked files).
- Build `cmake --build --preset vs2026-x64-debug --parallel`; tests `ctest --test-dir out/build/vs2026-x64 -C Debug --output-on-failure`. A new vcpkg dependency needs `cmake --preset vs2026-x64` first.
- Dependencies come only through vcpkg; bc7enc_rdo is pinned to commit `b9438627eef73a1157e84201b6fa6eb2ffd6d9f0` by an overlay port under `cmake/vcpkg-overlay-ports/`.
- `engine_asset` stays free of Vulkan; `CompressedTextureFormat` is mapped to `VkFormat` only in the renderer.
- Zero validation messages in Debug; `ObjectPushConstants` unchanged.
- Commits go straight to `main`, ending with `Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>`. Never stage `miniengine.settings.json` or `docs/superpowers/plans/2026-09-13-gbuffer-phase2-deferred-shading.md`.

## File Structure

| File | Change | Responsibility |
|---|---|---|
| `cmake/vcpkg-overlay-ports/bc7enc-rdo/{vcpkg.json,portfile.cmake,CMakeLists.txt}` | Create | Builds and installs the encoder as `unofficial::bc7enc-rdo::bc7enc-rdo` |
| `vcpkg.json`, `cmake/MiniEngineDependencies.cmake` | Modify | Depend on and find it |
| `engine/asset/texture_compression.{h,cpp}` | Create | Usage, formats, mip chain, encoding |
| `engine/asset/compressed_texture_cache.{h,cpp}` | Create | Cache key, file format, load-or-compress |
| `engine/asset/CMakeLists.txt` | Modify | New sources, encoder link |
| `tests/texture_compression_tests.cpp`, `tests/CMakeLists.txt` | Create/Modify | `miniengine.texture_compression` |
| `engine/renderer/vulkan/device.{h,cpp}` | Modify | Enable and report BC support |
| `engine/renderer/vulkan/texture.{h,cpp}` | Modify | Compressed upload path |
| `engine/renderer/vulkan/renderer.cpp` | Modify | Usage per slot, prepare/upload, stats |
| `shaders/vulkan/normal_map.glsl` (new), `triangle.frag`, `gbuffer.frag`, `engine/renderer/CMakeLists.txt` | Modify | Normal z reconstruction |
| `README.md` | Modify | Record the capability |

---

### Task 1: Encoder dependency and `texture_compression`

**Files:** create the port, `engine/asset/texture_compression.{h,cpp}`, `tests/texture_compression_tests.cpp`; modify `vcpkg.json`, `cmake/MiniEngineDependencies.cmake`, `engine/asset/CMakeLists.txt`, `tests/CMakeLists.txt`.

**Interfaces — Produces:**
`enum class TextureUsage : uint32_t { Color = 0, Normal = 1, Data = 2 }`;
`enum class CompressedTextureFormat : uint32_t { Bc7Srgb = 1, Bc7Unorm = 2, Bc5Unorm = 3 }`;
`struct CompressedTextureLevel { uint32_t width; uint32_t height; std::vector<uint8_t> blocks; }`;
`struct CompressedTexture { CompressedTextureFormat format; std::vector<CompressedTextureLevel> levels; }`;
`inline constexpr uint32_t kCompressedBlockBytes = 16`;
`CompressedTextureFormat FormatForUsage(TextureUsage)`; `uint32_t BlockCount(uint32_t pixels)`;
`std::vector<TextureData> BuildMipChain(const TextureData&, TextureUsage)`;
`CompressedTexture CompressTexture(const TextureData&, TextureUsage)`.

- [ ] **Step 1: Port.** `cmake/vcpkg-overlay-ports/bc7enc-rdo/vcpkg.json`:

```json
{
  "name": "bc7enc-rdo",
  "version-string": "2026-07-30",
  "description": "BC1-5 and BC7 CPU texture encoders by Richard Geldreich (bc7enc.cpp, rgbcx.cpp, bc7decomp.cpp only).",
  "homepage": "https://github.com/richgel999/bc7enc_rdo",
  "license": "MIT",
  "dependencies": [
    { "name": "vcpkg-cmake", "host": true },
    { "name": "vcpkg-cmake-config", "host": true }
  ]
}
```

`portfile.cmake`:

```cmake
# The upstream repository ships a sample executable, an ISPC encoder and an RDO encoder; the
# engine needs only the three portable encoders and decoder, built here as one static library by
# the CMakeLists.txt beside this file. Fetched by commit so the sources cannot drift.
vcpkg_from_git(
    OUT_SOURCE_PATH SOURCE_PATH
    URL https://github.com/richgel999/bc7enc_rdo.git
    REF b9438627eef73a1157e84201b6fa6eb2ffd6d9f0
    HEAD_REF master
)

file(COPY "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt" DESTINATION "${SOURCE_PATH}/miniengine")

vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}/miniengine")
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME unofficial-bc7enc-rdo)
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
```

`CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.20)
project(bc7enc_rdo LANGUAGES CXX)

set(BC7ENC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

add_library(bc7enc-rdo STATIC
    "${BC7ENC_ROOT}/bc7enc.cpp"
    "${BC7ENC_ROOT}/rgbcx.cpp"
    "${BC7ENC_ROOT}/bc7decomp.cpp")
target_include_directories(bc7enc-rdo PUBLIC
    "$<BUILD_INTERFACE:${BC7ENC_ROOT}>"
    "$<INSTALL_INTERFACE:include/bc7enc_rdo>")
target_compile_features(bc7enc-rdo PUBLIC cxx_std_11)

install(TARGETS bc7enc-rdo EXPORT unofficial-bc7enc-rdo-targets
    ARCHIVE DESTINATION lib LIBRARY DESTINATION lib RUNTIME DESTINATION bin)
install(FILES "${BC7ENC_ROOT}/bc7enc.h" "${BC7ENC_ROOT}/rgbcx.h" "${BC7ENC_ROOT}/bc7decomp.h"
    DESTINATION include/bc7enc_rdo)
install(EXPORT unofficial-bc7enc-rdo-targets
    NAMESPACE unofficial::bc7enc-rdo::
    DESTINATION share/unofficial-bc7enc-rdo)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/unofficial-bc7enc-rdo-config.cmake"
    "include(\"\${CMAKE_CURRENT_LIST_DIR}/unofficial-bc7enc-rdo-targets.cmake\")\n")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/unofficial-bc7enc-rdo-config.cmake"
    DESTINATION share/unofficial-bc7enc-rdo)
```

Add `"bc7enc-rdo"` to `vcpkg.json` dependencies (alphabetical, first); in `MiniEngineDependencies.cmake` after `find_package(Stb REQUIRED)`: `find_package(unofficial-bc7enc-rdo CONFIG REQUIRED)`. In `engine/asset/CMakeLists.txt` add the four new files (Task 2's included) and `PRIVATE unofficial::bc7enc-rdo::bc7enc-rdo` plus `"${Stb_INCLUDE_DIR}"` is already private there.

- [ ] **Step 2: Failing tests** `tests/texture_compression_tests.cpp` (full file in Task 1 of the executed code; cases):
  - `MipChainHalvesToOnePixel`: 4096x4096 gives 13 levels ending 1x1; 5x3 gives 5x3, 2x1, 1x1; 1x1 gives one level.
  - `SrgbDownsampleAveragesInLinearLight`: 2x2 black/white checker, `Color` usage, level 1 RGB within 2 of 188; the same with `Data` usage within 2 of 128.
  - `Bc7RoundTripIsAccurate`: 64x64 smooth RGB gradient, `Color`; decode level 0 with `bc7decomp::unpack_bc7`; PSNR over RGB > 40 dB.
  - `Bc7KeepsAlpha`: 16x16 with alpha 0 on the left half and 255 on the right; decoded alpha within 8.
  - `Bc5RoundTripIsAccurate`: 64x64 gradient in R and G, `Normal`; decode with `rgbcx::unpack_bc5`; max error <= 2.
  - `OddSizesEncodeWholeBlocks`: 5x3 `Data` gives level 0 of `2*1*16` bytes and three levels overall.
  Register `miniengine.texture_compression` linking `engine_asset` and `unofficial::bc7enc-rdo::bc7enc-rdo`, with the Windows runtime DLL copy step the other test targets use. Build: fails, `texture_compression.h` missing.

- [ ] **Step 3: Implement** `texture_compression.h/.cpp` as in the Interfaces block: `BuildMipChain` copies level 0, then halves with `stbir_resize_uint8_srgb(..., STBIR_RGBA)` for `Color` and `stbir_resize_uint8_linear(..., STBIR_4CHANNEL)` for `Normal`/`Data` (no alpha weighting: their fourth channel is data), throwing `std::runtime_error` if stb returns null. `CompressTexture` initialises both encoders once (`std::call_once`), uses `bc7enc_compress_block_params_init` with perceptual weights for `Color` and linear weights for `Data`, uber level 0, gathers each 4x4 block clamping reads to the last row/column, and calls `bc7enc_compress_block` or `rgbcx::encode_bc5_hq(dst, block, 0, 1, 4)`. `STB_IMAGE_RESIZE_IMPLEMENTATION` is defined in this `.cpp`, the only one that uses it.

- [ ] **Step 4: Run** `ctest -R texture_compression`: PASS. Full ctest, format check.
- [ ] **Step 5: Commit** `feat(asset): encode textures to BC7 and BC5 with bc7enc_rdo`.

### Task 2: `compressed_texture_cache`

**Interfaces — Consumes** Task 1. **Produces:**
`inline constexpr uint32_t kTextureCacheVersion = 1`;
`std::string BuildCompressedTextureKey(const std::filesystem::path& imagePath, TextureUsage usage)`;
`std::filesystem::path CompressedTextureCacheFile(const std::filesystem::path& cacheDirectory, const std::string& key)`;
`std::optional<CompressedTexture> ReadCompressedTexture(const std::filesystem::path& file, const std::string& key)`;
`bool WriteCompressedTexture(const std::filesystem::path& file, const std::string& key, const CompressedTexture& texture)`;
`struct CompressedTextureLoad { CompressedTexture texture; bool cacheHit = false; }`;
`CompressedTextureLoad LoadOrCompressTexture(const std::filesystem::path& imagePath, TextureUsage usage, const std::filesystem::path& cacheDirectory)`.

- [ ] **Step 1: Failing tests** in the same test file (the cache lives in `engine_asset`):
  - `CacheMissThenHit`: writes a 20x12 PNG with `stbi_write_png` into a fresh temp directory; the first `LoadOrCompressTexture` misses and creates exactly one `.metex`; the second hits and returns identical levels.
  - `ChangedFileMisses`: moving `last_write_time` forward by an hour changes the key and misses.
  - `UsageIsPartOfTheKey`: `Color` and `Data` for the same file give two cache files.
  - `DamagedFilesMiss`: a truncated file and a file written under a different key each read as `std::nullopt`.
- [ ] **Step 2:** build fails (header missing).
- [ ] **Step 3: Implement** per the spec's cache section: key = `weakly_canonical(path).generic_string() | size | last_write_time ticks | usage | version`; file name = 16 hex digits of 64-bit FNV-1a of the key + `.metex`; layout `METX`, version, format, level count, key length, key, then per level width, height, byte count, blocks; validation of every field (format 1-3, 1-32 levels, byte count equal to `BlockCount(w)*BlockCount(h)*16`); writes go to `<file>.tmp-<unique>` then `std::filesystem::rename`, removing the temporary on failure; `LoadOrCompressTexture` logs and ignores a failed write.
- [ ] **Step 4:** tests pass; format check.
- [ ] **Step 5: Commit** `feat(asset): cache compressed textures on disk`.

### Task 3: Device support and compressed upload

**Interfaces — Produces:** `bool VulkanDevice::SupportsBlockCompression() const`;
`VulkanTexture(VkPhysicalDevice, VkDevice, const CompressedTexture&, VulkanUploadBatch&)`.

- [ ] **Step 1:** `VulkanDevice` enables `textureCompressionBC` when the physical device reports it, and sets `m_supportsBlockCompression` when it does and `BC7_SRGB_BLOCK`, `BC7_UNORM_BLOCK` and `BC5_UNORM_BLOCK` each have `SAMPLED_IMAGE_BIT | SAMPLED_IMAGE_FILTER_LINEAR_BIT` in `optimalTilingFeatures`. Log the outcome once.
- [ ] **Step 2:** `VulkanTexture` compressed constructor: one staging buffer holding every level back to back (tracked immediately), an image in the mapped `VkFormat` with `TRANSFER_DST | SAMPLED` and the level count, a transition of all levels to `TRANSFER_DST_OPTIMAL`, one `vkCmdCopyBufferToImage` with a region per level (offsets are multiples of 16), a transition of all levels to `SHADER_READ_ONLY_OPTIMAL`, then the view and the sampler. Sampler creation moves into a `CreateSampler()` helper both paths call. Constructor failure releases through `DestroyHandles`, as the RGBA8 constructors do.
- [ ] **Step 3:** build; Debug run of the default scene; zero validation messages (nothing uses the path yet, this checks the device change).
- [ ] **Step 4: Commit** `feat(vulkan): upload block-compressed textures`.

### Task 4: Renderer integration and normal reconstruction

- [ ] **Step 1: Shaders.** New `shaders/vulkan/normal_map.glsl`:

```glsl
#ifndef NORMAL_MAP_GLSL
#define NORMAL_MAP_GLSL

// Tangent-space normal from a normal map texel. Only red and green are read: BC5 stores nothing
// else, and for an uncompressed map z is exactly what a unit vector implies, so both paths agree.
vec3 DecodeNormalMap(vec4 texel)
{
    vec2 xy = texel.rg * 2.0 - 1.0;
    return vec3(xy, sqrt(max(1.0 - dot(xy, xy), 0.0)));
}

#endif
```

  Include it in `triangle.frag` and `gbuffer.frag`, replace both `texture(...).xyz * 2.0 - 1.0` normal samples with `DecodeNormalMap(texture(...))`, and add `normal_map.glsl` to `MINIENGINE_SHADER_INCLUDES`.
- [ ] **Step 2: Renderer.** In `renderer.cpp`:
  - `BuildTextureCacheKey(path, TextureUsage)` gives `path|color`, `path|normal` or `path|data`; `ToVulkanTextureFormat(TextureUsage)` maps `Color` to `SrgbColor`, the others to `LinearData`.
  - A `PreparedTexture { std::optional<CompressedTexture> compressed; TextureData rgba; bool cacheHit; std::string error; }` and a thread-safe `PrepareTexture(path, usage, bool compress)` that tries `LoadOrCompressTexture(path, usage, EnginePaths::CacheRoot() / "textures")` when `compress` is set, logs a failure and falls back to `TextureLoader::LoadRGBA8`.
  - `loadTextureIndex` takes a `TextureUsage`; the material slots pass `Color` for base color and emissive, `Normal` for the normal maps and `Data` for metallic, roughness, occlusion and blend mask, secondaries included.
  - Prefetch collects `(path, usage)` pairs, runs `PrepareTexture` in chunks of `max(4, hardware_concurrency)`, uploads through the compressed constructor when `compressed` is set, and logs a per-upload summary: compressed from cache, compressed now (with total encode time), uncompressed.
  - The inline miss path in `loadTextureIndex` uses the same `PrepareTexture`.
- [ ] **Step 3: Verify.** Build Debug and Release; full ctest; Debug Sponza run with zero validation messages; first-load and second-load times from the log; `nvidia-smi` Sponza memory before (6.2 GB baseline) and after.
- [ ] **Step 4: Commit** `feat(vulkan): upload material textures block-compressed`.

### Task 5: Acceptance measurements and record

- [ ] Local capture harness (never committed, as for motion vectors): shaded Sponza with compression and with `MINIENGINE_CACHE_DIR` pointing at an empty directory while BC is forced off by a local patch; tone-mapped PSNR > 38 dB.
- [ ] Sponza plus curtains loads (scene file with `selected_entity: -1` plus `--model` curtains, as in the out-of-memory investigation) without the out-of-memory report.
- [ ] README section 7: textures are BC7/BC5 compressed on first load and cached in `CacheRoot()/textures`; memory figures.
- [ ] Commit `docs(readme): record block-compressed textures`.
