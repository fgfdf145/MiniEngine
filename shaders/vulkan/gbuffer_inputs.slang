#ifndef GBUFFER_INPUTS_SLANG
#define GBUFFER_INPUTS_SLANG

// Set 2 in every pipeline layout that samples the G-buffer. Binding N samples
// VulkanGBufferDescriptors::{engine/renderer/vulkan/gbuffer_inputs.h}; keep the two in
// the same order. Set 1 in those layouts is an empty placeholder, which is what lets both the
// lighting pass (which uses set 0 for the camera) and the tone mapping pass (which uses set 0 for
// its HDR sampler) put the G-buffer at the same index and include this file unchanged.
[[vk::binding(0, 2)]] Sampler2D gbufferAlbedo;
[[vk::binding(1, 2)]] Sampler2D gbufferNormal;
[[vk::binding(2, 2)]] Sampler2D gbufferSurface;
[[vk::binding(3, 2)]] Sampler2D gbufferEmissive;
[[vk::binding(4, 2)]] Sampler2D gbufferDepth;
[[vk::binding(5, 2)]] Sampler2D gbufferVelocity;
[[vk::binding(6, 2)]] Sampler2D sceneAo;
// GB5 specular, GB6 coat and anisotropy, GB7 sheen; each read only for the pixels whose
// SHADING_FLAG_* (gbuffer_common.slang) say they hold something.
[[vk::binding(7, 2)]] Sampler2D gbufferSpecular;
[[vk::binding(8, 2)]] Sampler2D gbufferCoat;
[[vk::binding(9, 2)]] Sampler2D gbufferSheen;
// Screen-space reflections: rgb radiance in HDR target units, a confidence (see ssr_resolve.comp).
[[vk::binding(10, 2)]] Sampler2D sceneReflections;
// One-bounce indirect diffuse: rgb pre-exposed radiance (see gi_resolve.comp). Written after the
// lighting pass, so only the composite and the debug view read anything meaningful from it.
[[vk::binding(11, 2)]] Sampler2D sceneGi;
// The ray traced sun shadow (rt_shadow_filter.comp): r visibility, g 1 where it ran this frame.
[[vk::binding(12, 2)]] Sampler2D sceneShadow;
// ReSTIR PT's shading (restir_pt_spatial.comp): rgb pre-exposed reflected radiance, read where the
// lighting pass's push constant debug.w says it ran.
[[vk::binding(13, 2)]] Sampler2D scenePathTrace;

#endif
