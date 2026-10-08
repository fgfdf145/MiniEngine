// The path tracer's set and push constants (VulkanPathTracePass, docs/design/
// 2026-10-07-path-tracing-design.md), shared by its trace, temporal and filter shaders. Every image the
// three pass between them is RGBA16F at the render size and holds pre-exposed radiance, demodulated:
// the light that reaches the surface through its diffuse lobe, or through its specular lobes (base and
// coat), per unit of that lobe's directional albedo (PathTraceLobesOf in pbr_common.glsl), which the
// lighting pass multiplies back.
//
// Include after scene_common.glsl and gbuffer_common.glsl. PATH_TRACE_SET is the set's index: 2 in the
// trace (set 1 is the ray scene), 1 in the other two.

#ifndef PATH_TRACE_COMMON_GLSL
#define PATH_TRACE_COMMON_GLSL

#ifndef PATH_TRACE_SET
#define PATH_TRACE_SET 1
#endif

// The G-buffer, nearest.
layout(set = PATH_TRACE_SET, binding = 0) uniform sampler2D ptDepth;
layout(set = PATH_TRACE_SET, binding = 1) uniform sampler2D ptNormal;   // GB1
layout(set = PATH_TRACE_SET, binding = 2) uniform sampler2D ptAlbedo;   // GB0
layout(set = PATH_TRACE_SET, binding = 3) uniform sampler2D ptSurface;  // GB2
layout(set = PATH_TRACE_SET, binding = 4) uniform sampler2D ptCoat;     // GB6
layout(set = PATH_TRACE_SET, binding = 5) uniform sampler2D ptVelocity; // rg motion, ba a mapped coat's normal
layout(set = PATH_TRACE_SET, binding = 6) uniform sampler2D ptSpecular; // GB5
layout(set = PATH_TRACE_SET, binding = 7) uniform sampler2D ptSheen;    // GB7
// Three pairs of images, diffuse then specular: the trace's raw paths; the accumulation this frame
// writes (a: the frames it averages); the result the lighting pass reads (SceneGi and
// SceneReflections, which path tracing mode takes over).
layout(set = PATH_TRACE_SET, binding = 8, rgba16f) uniform image2D ptRawDiffuse;
layout(set = PATH_TRACE_SET, binding = 9, rgba16f) uniform image2D ptRawSpecular;
layout(set = PATH_TRACE_SET, binding = 10, rgba16f) uniform image2D ptHistoryDiffuse;
layout(set = PATH_TRACE_SET, binding = 11, rgba16f) uniform image2D ptHistorySpecular;
layout(set = PATH_TRACE_SET, binding = 12, rgba16f) uniform image2D ptFinalDiffuse;
layout(set = PATH_TRACE_SET, binding = 13, rgba16f) uniform image2D ptFinalSpecular;
// Last frame's accumulation and the surfaces it was made on (x view depth, yz the shading normal,
// octahedral; x 0 where there was none), nearest; and this frame's surfaces.
layout(set = PATH_TRACE_SET, binding = 14) uniform sampler2D ptPreviousDiffuse;
layout(set = PATH_TRACE_SET, binding = 15) uniform sampler2D ptPreviousSpecular;
layout(set = PATH_TRACE_SET, binding = 16) uniform sampler2D ptPreviousSurface;
layout(set = PATH_TRACE_SET, binding = 17, rgba16f) uniform writeonly image2D ptHistorySurface;
// The atmosphere's multiple-scattering LUT (GENERAL), with which the trace integrates the air along
// its rays.
layout(set = PATH_TRACE_SET, binding = 18) uniform sampler2D ptMultiScatteringLut;

// PathTracePushConstants in engine/renderer/vulkan/path_trace_pass.cpp.
layout(push_constant) uniform PathTraceConstants
{
    vec2 extent;
    vec2 invExtent;
    uint frameIndex;
    uint flags;
    // Surfaces a path visits after the G-buffer's, and the local lights its next event estimation
    // resamples.
    uint maxBounces;
    uint lightCandidates;
    // A path vertex's most, in HDR target units; 0 none.
    float fireflyClamp;
    // Last frame's accumulation times this is in this frame's units.
    float historyScale;
    // The longest history a pixel averages this frame, and the one glossy lobes keep while anything
    // moves (PathTracingSettings::motionFrames).
    float historyCap;
    float motionFrames;
    // The filter's iteration: its taps' spacing in pixels (0 copies), and the PT_IMAGE_* pair it reads
    // and the one it writes.
    uint stepSize;
    uint source;
    uint target;
    uint unused;
}
pathTrace;

// Must match kFlag* in path_trace_pass.cpp.
const uint PT_FLAG_ACCUMULATE = 1u;
const uint PT_FLAG_DENOISE = 2u;
const uint PT_FLAG_HISTORY_VALID = 4u;
// PathTracingSettings::rayMedia: the paths' rays pass through the air and the height fog.
const uint PT_FLAG_RAY_MEDIA = 8u;
// PathTracingSettings::forwardSurfaces: the paths meet Blend surfaces by their coverage and refract
// through transmissive ones.
const uint PT_FLAG_FORWARD_SURFACES = 16u;

const uint PT_IMAGE_RAW = 0u;
const uint PT_IMAGE_HISTORY = 1u;
const uint PT_IMAGE_FINAL = 2u;

void LoadPathTracePair(uint pair, ivec2 pixel, out vec4 diffuse, out vec4 specular)
{
    if (pair == PT_IMAGE_RAW)
    {
        diffuse = imageLoad(ptRawDiffuse, pixel);
        specular = imageLoad(ptRawSpecular, pixel);
    }
    else if (pair == PT_IMAGE_HISTORY)
    {
        diffuse = imageLoad(ptHistoryDiffuse, pixel);
        specular = imageLoad(ptHistorySpecular, pixel);
    }
    else
    {
        diffuse = imageLoad(ptFinalDiffuse, pixel);
        specular = imageLoad(ptFinalSpecular, pixel);
    }
}

void StorePathTracePair(uint pair, ivec2 pixel, vec4 diffuse, vec4 specular)
{
    if (pair == PT_IMAGE_RAW)
    {
        imageStore(ptRawDiffuse, pixel, diffuse);
        imageStore(ptRawSpecular, pixel, specular);
    }
    else if (pair == PT_IMAGE_HISTORY)
    {
        imageStore(ptHistoryDiffuse, pixel, diffuse);
        imageStore(ptHistorySpecular, pixel, specular);
    }
    else
    {
        imageStore(ptFinalDiffuse, pixel, diffuse);
        imageStore(ptFinalSpecular, pixel, specular);
    }
}

vec3 PathTraceWorldPosition(ivec2 pixel, float depth)
{
    vec2 uv = (vec2(pixel) + 0.5) * pathTrace.invExtent;
    vec4 world = ubo.invViewProj * vec4(uv * 2.0 - 1.0, depth, 1.0);
    return world.xyz / world.w;
}

float PathTraceLuminance(vec3 color)
{
    return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

// The pixel's opaque deferred surface: false where the lighting pass shades nothing of it (the sky,
// unlit and forward-shaded materials), which the path tracer leaves black.
bool PathTraceSurfaceShaded(ivec2 pixel, float depth)
{
    if (IsFarDepth(depth))
    {
        return false;
    }
    uint flags = DecodeShadingFlags(texelFetch(ptSurface, pixel, 0).a);
    return (flags & (SHADING_FLAG_UNLIT | SHADING_FLAG_FORWARD)) == 0u;
}

// The roughness the specular channel's history is limited by: the smoother of the base and the coat,
// whichever reflects the sharper image.
float PathTraceSpecularRoughness(ivec2 pixel)
{
    vec4 surface = texelFetch(ptSurface, pixel, 0);
    float roughness = clamp(surface.g, 0.04, 1.0);
    if ((DecodeShadingFlags(surface.a) & SHADING_FLAG_CLEARCOAT) != 0u)
    {
        vec4 coat = texelFetch(ptCoat, pixel, 0);
        if (coat.r > 0.0)
        {
            roughness = min(roughness, clamp(coat.g, 0.04, 1.0));
        }
    }
    return roughness;
}

#endif
