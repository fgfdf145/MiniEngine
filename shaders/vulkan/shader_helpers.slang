// Small helpers for operations whose Slang spelling differs from the GLSL the shaders were first
// written in (docs/design/2026-10-08-slang-shader-migration-design.md).

#ifndef SHADER_HELPERS_SLANG
#define SHADER_HELPERS_SLANG

// A vertex shader's clip space position as the API's rasterizer reads it. The shaders are written for
// Vulkan's, y down; Direct3D's is y up, so the DXIL build (MINIENGINE_DXIL) mirrors y, and the same
// triangles land on the same pixels with the same winding (both APIs judge it in framebuffer space).
// Every vertex shader writes SV_Position through this.
float4 ClipPosition(float4 position)
{
#ifdef MINIENGINE_DXIL
    return float4(position.x, -position.y, position.z, position.w);
#else
    return position;
#endif
}

// GLSL's mod: x - y * floor(x / y), which keeps the sign of y. fmod truncates instead, so it
// differs for negative x.
float GlslMod(float x, float y)
{
    return x - y * floor(x / y);
}
float2 GlslMod(float2 x, float y)
{
    return x - y * floor(x / y);
}
float3 GlslMod(float3 x, float y)
{
    return x - y * floor(x / y);
}
float2 GlslMod(float2 x, float2 y)
{
    return x - y * floor(x / y);
}
float3 GlslMod(float3 x, float3 y)
{
    return x - y * floor(x / y);
}

// Column i of a matrix. Slang's m[i] is row i (GLSL's m[i] is column i).
float3 ColumnOf(float3x3 m, int i)
{
    return float3(m[0][i], m[1][i], m[2][i]);
}
float4 ColumnOf(float4x4 m, int i)
{
    return float4(m[0][i], m[1][i], m[2][i], m[3][i]);
}

// A matrix from its columns, as GLSL's matN(c0, c1, ...) builds it, and from its rows, as Slang's
// floatNxN(r0, r1, ...) does. Shared C++ code names one of them instead of a constructor, whose
// order GLM and Slang disagree on (engine/renderer/shader_cpp_compat.h defines both for GLM).
float3x3 MatrixFromColumns(float3 c0, float3 c1, float3 c2)
{
    return transpose(float3x3(c0, c1, c2));
}
float4x4 MatrixFromColumns(float4 c0, float4 c1, float4 c2, float4 c3)
{
    return transpose(float4x4(c0, c1, c2, c3));
}
float3x3 MatrixFromRows(float3 r0, float3 r1, float3 r2)
{
    return float3x3(r0, r1, r2);
}

// The element count of a runtime-sized buffer (GLSL's .length()).
uint BufferLength<T>(StructuredBuffer<T> buffer)
{
    uint count, stride;
    buffer.GetDimensions(count, stride);
    return count;
}

// GLSL's inverse (GLSL.std.450 MatrixInverse). Other targets (the DXIL build) take the adjugate:
// the rows' pairwise cross products are the inverse's columns times the determinant.
float3x3 Inverse(float3x3 m)
{
    __target_switch
    {
    case spirv:
        return spirv_asm { result:$$float3x3 = OpExtInst glsl450 MatrixInverse $m };
    default:
        float3 c0 = cross(m[1], m[2]);
        float3 c1 = cross(m[2], m[0]);
        float3 c2 = cross(m[0], m[1]);
        return transpose(float3x3(c0, c1, c2)) / dot(m[0], c0);
    }
}

// GLSL's packing functions, as the same GLSL.std.450 instructions; elsewhere written out as the GLSL
// specification defines them (round to nearest, the first component in the low bits).
uint PackUnorm4x8(float4 v)
{
    __target_switch
    {
    case spirv:
        return spirv_asm { result:$$uint = OpExtInst glsl450 PackUnorm4x8 $v };
    default:
        uint4 u = uint4(round(saturate(v) * 255.0));
        return u.x | (u.y << 8) | (u.z << 16) | (u.w << 24);
    }
}
float4 UnpackUnorm4x8(uint p)
{
    __target_switch
    {
    case spirv:
        return spirv_asm { result:$$float4 = OpExtInst glsl450 UnpackUnorm4x8 $p };
    default:
        return float4((uint4(p) >> uint4(0u, 8u, 16u, 24u)) & 0xffu) / 255.0;
    }
}
uint PackSnorm2x16(float2 v)
{
    __target_switch
    {
    case spirv:
        return spirv_asm { result:$$uint = OpExtInst glsl450 PackSnorm2x16 $v };
    default:
        int2 i = int2(round(clamp(v, -1.0, 1.0) * 32767.0));
        return (uint(i.x) & 0xffffu) | (uint(i.y) << 16);
    }
}
float2 UnpackSnorm2x16(uint p)
{
    __target_switch
    {
    case spirv:
        return spirv_asm { result:$$float2 = OpExtInst glsl450 UnpackSnorm2x16 $p };
    default:
        int2 i = int2(p << 16, p) >> 16;
        return clamp(float2(i) / 32767.0, -1.0, 1.0);
    }
}
uint PackHalf2x16(float2 v)
{
    __target_switch
    {
    case spirv:
        return spirv_asm { result:$$uint = OpExtInst glsl450 PackHalf2x16 $v };
    default:
        return f32tof16(v.x) | (f32tof16(v.y) << 16);
    }
}
float2 UnpackHalf2x16(uint p)
{
    __target_switch
    {
    case spirv:
        return spirv_asm { result:$$float2 = OpExtInst glsl450 UnpackHalf2x16 $p };
    default:
        return float2(f16tof32(p), f16tof32(p >> 16));
    }
}

// textureSize / imageSize. The shaders take separate textures and samplers (no combined image
// samplers, which DXIL lacks), so these are over the texture types.
int2 TextureSize(Texture2D t, int lod)
{
    uint width, height, levels;
    t.GetDimensions(uint(lod), width, height, levels);
    return int2(width, height);
}
int2 TextureSize(Texture2DArray t, int lod)
{
    uint width, height, elements, levels;
    t.GetDimensions(uint(lod), width, height, elements, levels);
    return int2(width, height);
}
int TextureLevels(Texture2D t)
{
    uint width, height, levels;
    t.GetDimensions(0, width, height, levels);
    return int(levels);
}
int2 ImageSize<T : ITexelElement>(RWTexture2D<T> image)
{
    uint width, height;
    image.GetDimensions(width, height);
    return int2(width, height);
}
int3 ImageSize<T : ITexelElement>(RWTexture3D<T> image)
{
    uint width, height, depth;
    image.GetDimensions(width, height, depth);
    return int3(width, height, depth);
}

#endif
