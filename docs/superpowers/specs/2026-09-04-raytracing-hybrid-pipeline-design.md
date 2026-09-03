# Ray Tracing Hybrid Pipeline — Decomposition Design

## Goal

Add a hybrid rendering path to MiniEngine: keep rasterization as the primary
visibility pass and use hardware ray tracing for effects rasterization cannot
express. The first shipped effect is ray-traced hard and soft shadows.

This document is the umbrella. It records the measured baseline, the decisions
that constrain every later step, and the decomposition into sub-projects. It
specifies no implementation. Each sub-project gets its own design document,
implementation plan, and verification cycle.

## Measured Baseline

### Hardware (vulkaninfo, this machine, 2026-09-04)

| Device | Type | Ray tracing extensions |
| --- | --- | --- |
| NVIDIA GeForce RTX 4070 Laptop (driver 616.56, apiVersion 1.4.351) | Discrete | `VK_KHR_acceleration_structure`, `VK_KHR_ray_query`, `VK_KHR_ray_tracing_pipeline`, `VK_KHR_deferred_host_operations`, `VK_KHR_ray_tracing_maintenance1`, `VK_KHR_ray_tracing_position_fetch`, `VK_NV_ray_tracing_invocation_reorder` |
| Intel RaptorLake-S Mobile Graphics (apiVersion 1.4.311) | Integrated | None |

The NVIDIA device reports `maxMemoryAllocationCount = 4294967295`. The
per-submesh `vkAllocateMemory` count recorded in the 2026-07-30 review is
therefore a portability and overhead problem, not a correctness gate for
acceleration structures on this hardware. This corrects the assumption that a
suballocator is a hard prerequisite for ray tracing.

### Code facts blocking ray tracing

- `engine/renderer/vulkan/instance.cpp` requests `VK_API_VERSION_1_3`, but
  `engine/renderer/vulkan/device.cpp` requires only `VK_KHR_swapchain`, passes
  a plain `VkPhysicalDeviceFeatures`, and leaves `pNext` empty. No Vulkan 1.2
  or 1.3 feature is enabled anywhere.
- `VulkanDevice::IsSuitable` selects the first device that passes, with no
  scoring. Enumeration order placing the discrete GPU first is not guaranteed
  by the specification.
- `engine/renderer/vulkan/buffer.cpp` creates vertex and index buffers with
  only `VERTEX_BUFFER` / `INDEX_BUFFER` plus `TRANSFER_DST`. Acceleration
  structure builds additionally require `SHADER_DEVICE_ADDRESS` and
  `ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_KHR`, and the allocation needs
  `VkMemoryAllocateFlagsInfo` with `VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT`.
- Materials bind one descriptor set of 13 combined image samplers per draw,
  with transform and material factors in a 128-byte push constant block that
  is already at the Vulkan minimum. Hit shaders have no per-draw binding point,
  so any ray tracing path that shades at the hit needs bindless resources.
- `engine/renderer/vulkan/texture.cpp` creates one `VkSampler` per texture with
  identical parameters, differing only in `maxLod`.
- The scene viewport depth attachment has no `SAMPLED_BIT`; the color target is
  the swapchain `B8G8R8A8_SRGB` format and Reinhard tone mapping is hard-coded
  in `shaders/vulkan/triangle.frag`. There is no HDR intermediate target.
- `RecordSceneLayer` is a single pass in a single function. There is no pass or
  barrier abstraction to host additional passes.
- `engine/renderer/CMakeLists.txt` compiles exactly two hard-coded shader
  files. Ray tracing stages need generalized compilation and a SPIR-V 1.4 or
  later target.

## Locked Decisions

1. **First effect: ray-traced shadows.** Visibility only. Reflections, ambient
   occlusion and global illumination are later work and are not designed here.
2. **Foundations first.** Memory and descriptor foundations land before any
   acceleration structure work, on the rasterization path, with no change to
   the rendered image. This costs two sub-projects with no visible result, and
   avoids reworking the frame structure when later effects arrive.
3. **VMA for device memory.** Add `vulkan-memory-allocator` to vcpkg rather
   than writing a suballocator. The alignment rules that acceleration
   structure and scratch buffers impose on buffer device addresses are already
   handled there.
4. **Separate sampled image and sampler arrays for bindless.** One global
   sampled image array using `VARIABLE_DESCRIPTOR_COUNT`, plus a small sampler
   array deduplicated by wrap mode. This removes the duplicate samplers and
   leaves room for the glTF sampler wrap and `KHR_texture_transform` gaps that
   are still open.

## Sub-Projects

| # | Sub-project | Delivery boundary | Acceptance |
| --- | --- | --- | --- |
| P0 | Device capability foundation | Split required and optional device extensions; add the `VkPhysicalDeviceFeatures2` pNext chain; replace first-match device selection with scoring; expose `VulkanDevice::GetCapabilities()` | Startup log shows the RTX 4070 selected with each feature bit enabled individually; an integrated-GPU-only machine still starts and reports no ray tracing |
| P1 | VMA memory foundation | Add the vcpkg dependency; a `VulkanAllocator` living as long as the logical device; migrate all four hand-written `vkAllocateMemory` sites (`buffer.cpp`, `texture.cpp`, `scene_viewport.cpp`, `uniform_buffer.cpp`); enable the buffer device address flag | Image unchanged; logged VMA statistics show allocation count dropping from roughly 810 to block-level; CTest and a 60-frame smoke run pass |
| P2 | Bindless materials | Global sampled image array plus a wrap-deduplicated sampler array; material SSBO holding 13 texture indices and factors; instance SSBO holding model matrix, normal matrix and material index; push constants reduced to an instance index; rasterization path switched over | Image unchanged; descriptor set count drops from materials × frames to a constant; duplicate samplers gone; one `vkCmdBindDescriptorSets` per frame |
| P3 | Acceleration structures | One BLAS per submesh reusing P1 buffers; TLAS built from the instance list; rebuild and update policy tied to `ApplyRenderContent` and entity add / remove / transform; scratch buffer reuse | TLAS instance count matches submesh count; acceleration structure state stays correct across scene load, delete and transform; no validation errors |
| P4 | Hybrid frame structure | HDR intermediate target (`R16G16B16A16_SFLOAT`); `SAMPLED_BIT` on depth and normal attachments; multi-pass and barrier abstraction extracted from `RecordSceneLayer`; tone mapping moved out of `triangle.frag` into its own pass | Image equivalent to the current output |
| P5 | Ray query shadows | A visibility ray per non-ambient light from the fragment shader; per-light-type ray shapes; soft shadows by cone sampling scaled to light size, plus spatial filtering | Manual GUI visual acceptance |

## Sequencing

P0 precedes P1 and P2: the VMA allocator cannot set
`VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT` until the device enables
`bufferDeviceAddress`, and bindless needs the descriptor indexing feature bits.
Both are P0. P1 and P2 are independent of each other and follow the order the
2026-07-30 review recommended. P3 depends on P1 for buffer usage flags and
allocation. P4 depends on nothing but is done in full before P5 so that later
effects do not force the frame to be restructured again.

Because P2 lands before P5, the ray query loop can read bindless material data
and perform the alpha test directly. Shadows from alpha-mask materials are
therefore correct from the start rather than being recorded as a known
deviation.

## Out of Scope

Deliberately excluded from all six sub-projects, to be picked up separately if
wanted: frustum culling; IBL and specular ambient; MSAA and TAA; the temporal
denoising foundation, including camera jitter, motion vectors and history
buffers; ray-traced reflections, ambient occlusion and global illumination; a
ray tracing abstraction in `rhi/backend.h`, since this work lands only in the
Vulkan backend; and the imported-alpha squaring fix and light-count sorting
warning recorded in the 2026-07-30 review.
