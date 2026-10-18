// Depth resolve: depth-stencil formats cannot be copied into a colour texture, so the depth target is read through its bindless descriptor
// and written as R32F into the texture the guest resolved to. Same bindless layout as blit.hlsl.
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
#define g_Origin vk::RawBufferLoad<int2>(g_PushConstants.SharedConstants + 8)

#else

cbuffer DepthResolveConstants : register(b2, space4) {
    uint g_TextureIndex;
    uint g_Padding;
    // Added to the destination pixel to find the source texel: the source rectangle's corner less the destination point.
    int2 g_Origin;
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

float PSMain(VSOutput input) : SV_Target {
    return g_Texture2DDescriptorHeap[g_TextureIndex].Load(int3(int2(input.position.xy) + g_Origin, 0)).x;
}
