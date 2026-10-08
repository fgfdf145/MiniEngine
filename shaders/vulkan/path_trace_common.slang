// The path tracer's set and push constants (VulkanPathTracePass, docs/design/
// 2026-10-07-path-tracing-design.md), shared by its trace, temporal and filter shaders. Every image the
// three pass between them is RGBA16F at the render size and holds pre-exposed radiance, demodulated:
// the light that reaches the surface through its diffuse lobe, or through its specular lobes (base and
// coat), per unit of that lobe's directional albedo (PathTraceLobesOf in pbr_common.slang), which the
// lighting pass multiplies back.
//
// Include after scene_common.slang and gbuffer_common.slang. PATH_TRACE_SET is the set's index: 2 in the
// trace (set 1 is the ray scene), 1 in the other two.

#ifndef PATH_TRACE_COMMON_SLANG
#define PATH_TRACE_COMMON_SLANG

#ifndef PATH_TRACE_SET
#define PATH_TRACE_SET 1
#endif

// The G-buffer, nearest.
[[vk::binding(0, PATH_TRACE_SET)]] Sampler2D ptDepth;
[[vk::binding(1, PATH_TRACE_SET)]] Sampler2D ptNormal;   // GB1
[[vk::binding(2, PATH_TRACE_SET)]] Sampler2D ptAlbedo;   // GB0
[[vk::binding(3, PATH_TRACE_SET)]] Sampler2D ptSurface;  // GB2
[[vk::binding(4, PATH_TRACE_SET)]] Sampler2D ptCoat;     // GB6
[[vk::binding(5, PATH_TRACE_SET)]] Sampler2D ptVelocity; // rg motion, ba a mapped coat's normal
[[vk::binding(6, PATH_TRACE_SET)]] Sampler2D ptSpecular; // GB5
[[vk::binding(7, PATH_TRACE_SET)]] Sampler2D ptSheen;    // GB7
// Three pairs of images, diffuse then specular: the trace's raw paths; the accumulation this frame
// writes (a: the frames it averages); the result the lighting pass reads (SceneGi and
// SceneReflections, which path tracing mode takes over).
[[vk::binding(8, PATH_TRACE_SET)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> ptRawDiffuse;
[[vk::binding(9, PATH_TRACE_SET)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> ptRawSpecular;
[[vk::binding(10, PATH_TRACE_SET)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> ptHistoryDiffuse;
[[vk::binding(11, PATH_TRACE_SET)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> ptHistorySpecular;
[[vk::binding(12, PATH_TRACE_SET)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> ptFinalDiffuse;
[[vk::binding(13, PATH_TRACE_SET)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> ptFinalSpecular;
// Last frame's accumulation and the surfaces it was made on (x view depth, yz the shading normal,
// octahedral; x 0 where there was none), nearest; and this frame's surfaces.
[[vk::binding(14, PATH_TRACE_SET)]] Sampler2D ptPreviousDiffuse;
[[vk::binding(15, PATH_TRACE_SET)]] Sampler2D ptPreviousSpecular;
[[vk::binding(16, PATH_TRACE_SET)]] Sampler2D ptPreviousSurface;
[[vk::binding(17, PATH_TRACE_SET)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> ptHistorySurface;
// The atmosphere's multiple-scattering LUT (GENERAL), with which the trace integrates the air along
// its rays.
[[vk::binding(18, PATH_TRACE_SET)]] Sampler2D ptMultiScatteringLut;

// PathTracePushConstants in engine/renderer/vulkan/path_trace_pass.cpp.
struct PathTraceConstants
{
    float2 extent;
    float2 invExtent;
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
    // The G-buffer is this many times 2 larger each way than the grid traced (extent): the layer at
    // half resolution reads its top-left pixel of each 2 x 2 block (PtGBufferPixel).
    uint gbufferShift;
};
[[vk::push_constant]] ConstantBuffer<PathTraceConstants> pathTrace;

// Must match kFlag* in path_trace_pass.cpp.
static const uint PT_FLAG_ACCUMULATE = 1u;
static const uint PT_FLAG_DENOISE = 2u;
static const uint PT_FLAG_HISTORY_VALID = 4u;
// PathTracingSettings::rayMedia: the paths' rays pass through the air and the height fog.
static const uint PT_FLAG_RAY_MEDIA = 8u;
// PathTracingSettings::forwardSurfaces: the paths meet Blend surfaces by their coverage and refract
// through transmissive ones.
static const uint PT_FLAG_FORWARD_SURFACES = 16u;
// PathTracingSettings::emissiveLights, and the frame has emissive triangles: next event estimation
// picks one too (emissive_lights_common.slang), against the paths' hits by MIS.
static const uint PT_FLAG_EMISSIVE_LIGHTS = 32u;
// PathTracingSettings::lightGrid, and the frame built it: the local light candidates come from the
// light grid's cells (light_grid_common.slang).
static const uint PT_FLAG_LIGHT_GRID = 64u;
// DLSS ray reconstruction denoises the raw paths: the specular result's alpha is the specular path's
// first hit distance (0 where there was no specular path), for its guides.
static const uint PT_FLAG_HIT_DISTANCE = 128u;

static const uint PT_IMAGE_RAW = 0u;
static const uint PT_IMAGE_HISTORY = 1u;
static const uint PT_IMAGE_FINAL = 2u;

void LoadPathTracePair(uint pair, int2 pixel, out float4 diffuse, out float4 specular)
{
    if (pair == PT_IMAGE_RAW)
    {
        diffuse = ptRawDiffuse[pixel];
        specular = ptRawSpecular[pixel];
    }
    else if (pair == PT_IMAGE_HISTORY)
    {
        diffuse = ptHistoryDiffuse[pixel];
        specular = ptHistorySpecular[pixel];
    }
    else
    {
        diffuse = ptFinalDiffuse[pixel];
        specular = ptFinalSpecular[pixel];
    }
}

void StorePathTracePair(uint pair, int2 pixel, float4 diffuse, float4 specular)
{
    if (pair == PT_IMAGE_RAW)
    {
        ptRawDiffuse[pixel] = diffuse;
        ptRawSpecular[pixel] = specular;
    }
    else if (pair == PT_IMAGE_HISTORY)
    {
        ptHistoryDiffuse[pixel] = diffuse;
        ptHistorySpecular[pixel] = specular;
    }
    else
    {
        ptFinalDiffuse[pixel] = diffuse;
        ptFinalSpecular[pixel] = specular;
    }
}

// The G-buffer pixel a traced pixel reads.
int2 PtGBufferPixel(int2 pixel)
{
    return pixel << pathTrace.gbufferShift;
}

// The world position of a traced pixel's surface, depth read at PtGBufferPixel(pixel).
float3 PathTraceWorldPosition(int2 pixel, float depth)
{
    float2 uv = (float2(PtGBufferPixel(pixel)) + 0.5) * pathTrace.invExtent / float(1u << pathTrace.gbufferShift);
    float4 world = mul(ubo.invViewProj, float4(uv * 2.0 - 1.0, depth, 1.0));
    return world.xyz / world.w;
}

float PathTraceLuminance(float3 color)
{
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

// The pixel's opaque deferred surface: false where the lighting pass shades nothing of it (the sky,
// unlit and forward-shaded materials), which the path tracer leaves black.
bool PathTraceSurfaceShaded(int2 pixel, float depth)
{
    if (IsFarDepth(depth))
    {
        return false;
    }
    uint flags = DecodeShadingFlags(ptSurface.Load(int3(PtGBufferPixel(pixel), 0)).a);
    return (flags & (SHADING_FLAG_UNLIT | SHADING_FLAG_FORWARD)) == 0u;
}

// The roughness the specular channel's history is limited by: the smoother of the base and the coat,
// whichever reflects the sharper image.
float PathTraceSpecularRoughness(int2 pixel)
{
    float4 surface = ptSurface.Load(int3(PtGBufferPixel(pixel), 0));
    float roughness = clamp(surface.g, 0.04, 1.0);
    if ((DecodeShadingFlags(surface.a) & SHADING_FLAG_CLEARCOAT) != 0u)
    {
        float4 coat = ptCoat.Load(int3(PtGBufferPixel(pixel), 0));
        if (coat.r > 0.0)
        {
            roughness = min(roughness, clamp(coat.g, 0.04, 1.0));
        }
    }
    return roughness;
}

#endif
