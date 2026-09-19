#ifndef GBUFFER_INPUTS_GLSL
#define GBUFFER_INPUTS_GLSL

// Set 2 in every pipeline layout that samples the G-buffer. Binding N samples
// VulkanGBufferDescriptors::kInputs[N] (engine/renderer/vulkan/gbuffer_inputs.h); keep the two in
// the same order. Set 1 in those layouts is an empty placeholder, which is what lets both the
// lighting pass (which uses set 0 for the camera) and the tone mapping pass (which uses set 0 for
// its HDR sampler) put the G-buffer at the same index and include this file unchanged.
layout(set = 2, binding = 0) uniform sampler2D gbufferAlbedo;
layout(set = 2, binding = 1) uniform sampler2D gbufferNormal;
layout(set = 2, binding = 2) uniform sampler2D gbufferSurface;
layout(set = 2, binding = 3) uniform sampler2D gbufferEmissive;
layout(set = 2, binding = 4) uniform sampler2D gbufferDepth;
layout(set = 2, binding = 5) uniform sampler2D gbufferVelocity;

#endif
