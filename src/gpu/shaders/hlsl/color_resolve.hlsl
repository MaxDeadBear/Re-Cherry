// Scaled colour resolve: a resolve with an exponent bias (GPU_COPYDESTINFO.CopyDestExpBias, from the high six bits of the Resolve flags, together with the
// source surface's GPU_COLORINFO.ColorExpBias) multiplies every texel by a power of two, which a copy cannot do. An MSAA surface resolved over the
// tiles of a single-sample one averages each block of samples, `g_Samples` texels of that surface. Same bindless layout as blit.hlsl.
// Compiled offline by tools/shaders/build_builtin_shaders.py.

Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);

#ifdef __spirv__

struct PushConstants {
    uint64_t VertexShaderConstants;
    uint64_t PixelShaderConstants;
    uint64_t SharedConstants;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants;

#define g_TextureIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 0)
#define g_Scale vk::RawBufferLoad<float>(g_PushConstants.SharedConstants + 4)
#define g_Origin vk::RawBufferLoad<int2>(g_PushConstants.SharedConstants + 8)
#define g_Samples vk::RawBufferLoad<uint2>(g_PushConstants.SharedConstants + 16)

#else

cbuffer ColorResolveConstants : register(b2, space4) {
    uint g_TextureIndex;
    float g_Scale;
    // Added to the destination pixel to find the source texel: the source rectangle's corner less the destination point.
    int2 g_Origin;
    uint2 g_Samples;
};

#endif

struct VSOutput {
    float4 position : SV_Position;
};

VSOutput VSMain(uint id : SV_VertexID) {
    VSOutput output;
    const float2 uv = float2((id << 1) & 2, id & 2);
    output.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}

float4 PSMain(VSOutput input) : SV_Target {
    const int2 corner = (int2(input.position.xy) + g_Origin) * int2(g_Samples);
    float4 sum = 0.0;
    for (uint y = 0; y < g_Samples.y; ++y) {
        for (uint x = 0; x < g_Samples.x; ++x) sum += g_Texture2DDescriptorHeap[g_TextureIndex].Load(int3(corner + int2(x, y), 0));
    }
    return sum * (g_Scale / float(g_Samples.x * g_Samples.y));
}
