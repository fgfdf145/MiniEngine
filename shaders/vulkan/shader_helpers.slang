// Small helpers for operations whose Slang spelling differs from the GLSL the shaders were first
// written in (docs/design/2026-10-08-slang-shader-migration-design.md).

#ifndef SHADER_HELPERS_SLANG
#define SHADER_HELPERS_SLANG

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

// GLSL's inverse (GLSL.std.450 MatrixInverse).
float3x3 Inverse(float3x3 m)
{
    return spirv_asm { result:$$float3x3 = OpExtInst glsl450 MatrixInverse $m };
}

// GLSL's packing functions, as the same GLSL.std.450 instructions.
uint PackUnorm4x8(float4 v)
{
    return spirv_asm { result:$$uint = OpExtInst glsl450 PackUnorm4x8 $v };
}
float4 UnpackUnorm4x8(uint p)
{
    return spirv_asm { result:$$float4 = OpExtInst glsl450 UnpackUnorm4x8 $p };
}
uint PackSnorm2x16(float2 v)
{
    return spirv_asm { result:$$uint = OpExtInst glsl450 PackSnorm2x16 $v };
}
float2 UnpackSnorm2x16(uint p)
{
    return spirv_asm { result:$$float2 = OpExtInst glsl450 UnpackSnorm2x16 $p };
}
uint PackHalf2x16(float2 v)
{
    return spirv_asm { result:$$uint = OpExtInst glsl450 PackHalf2x16 $v };
}
float2 UnpackHalf2x16(uint p)
{
    return spirv_asm { result:$$float2 = OpExtInst glsl450 UnpackHalf2x16 $p };
}

// textureSize / imageSize.
int2 TextureSize(Sampler2D t, int lod)
{
    uint width, height, levels;
    t.GetDimensions(uint(lod), width, height, levels);
    return int2(width, height);
}
int2 TextureSize(Sampler2DArray t, int lod)
{
    uint width, height, elements, levels;
    t.GetDimensions(uint(lod), width, height, elements, levels);
    return int2(width, height);
}
int3 TextureSize(Sampler3D t, int lod)
{
    uint width, height, depth, levels;
    t.GetDimensions(uint(lod), width, height, depth, levels);
    return int3(width, height, depth);
}
int2 TextureSize(SamplerCube t, int lod)
{
    uint width, height, levels;
    t.GetDimensions(uint(lod), width, height, levels);
    return int2(width, height);
}
// The frame set's textures, whose samplers are bindings of their own.
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
int TextureLevels(Sampler2D t)
{
    uint width, height, levels;
    t.GetDimensions(0, width, height, levels);
    return int(levels);
}
int TextureLevels(SamplerCube t)
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
