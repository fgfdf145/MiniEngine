#ifndef GBUFFER_INPUTS_SLANG
#define GBUFFER_INPUTS_SLANG

// Set 2 in every pipeline layout that samples the G-buffer. Binding N samples
// VulkanGBufferDescriptors::{engine/renderer/vulkan/gbuffer_inputs.h}; keep the two in
// the same order. Set 1 in those layouts is an empty placeholder, which is what lets both the
// lighting pass (which uses set 0 for the camera) and the tone mapping pass (which uses set 0 for
// its HDR sampler) put the G-buffer at the same index and include this file unchanged.
[[vk::binding(0, 2)]] Texture2D gbufferAlbedo : register(t0, space2);
[[vk::binding(1, 2)]] Texture2D gbufferNormal : register(t1, space2);
[[vk::binding(2, 2)]] Texture2D gbufferSurface : register(t2, space2);
[[vk::binding(3, 2)]] Texture2D gbufferEmissive : register(t3, space2);
[[vk::binding(4, 2)]] Texture2D gbufferDepth : register(t4, space2);
[[vk::binding(5, 2)]] Texture2D gbufferVelocity : register(t5, space2);
[[vk::binding(6, 2)]] Texture2D sceneAo : register(t6, space2);
// GB5 specular, GB6 coat and anisotropy, GB7 sheen; each read only for the pixels whose
// SHADING_FLAG_* (gbuffer_common.slang) say they hold something.
[[vk::binding(7, 2)]] Texture2D gbufferSpecular : register(t7, space2);
[[vk::binding(8, 2)]] Texture2D gbufferCoat : register(t8, space2);
[[vk::binding(9, 2)]] Texture2D gbufferSheen : register(t9, space2);
// Screen-space reflections: rgb radiance in HDR target units, a confidence (see ssr_resolve.comp).
[[vk::binding(10, 2)]] Texture2D sceneReflections : register(t10, space2);
// One-bounce indirect diffuse: rgb pre-exposed radiance (see gi_resolve.comp). Written after the
// lighting pass, so only the composite and the debug view read anything meaningful from it.
[[vk::binding(11, 2)]] Texture2D sceneGi : register(t11, space2);
// The ray traced sun shadow (rt_shadow_filter.comp): r visibility, g 1 where it ran this frame.
[[vk::binding(12, 2)]] Texture2D sceneShadow : register(t12, space2);
// ReSTIR PT's shading (restir_pt_spatial.comp): rgb pre-exposed reflected radiance, read where the
// lighting pass's push constant debug.w says it ran.
[[vk::binding(13, 2)]] Texture2D scenePathTrace : register(t13, space2);
// The one sampler every input is read with (VulkanGBufferDescriptors::kSamplerBinding): nearest,
// one texel per pixel.
[[vk::binding(64, 2)]] SamplerState gbufferSampler : register(s64, space2);

#endif
