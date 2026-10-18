// Scaling blit through the same bindless layout as the translated guest shaders: the source texture comes from the 2D heap, its index and
// the source rectangle from the shared-constants block (root CBV b2 on D3D12, the third pushed buffer address on Vulkan).
// Compiled offline by tools/shaders/build_builtin_shaders.py.

Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
SamplerState g_SamplerDescriptorHeap[] : register(s0, space3);

#ifdef __spirv__

struct PushConstants {
    uint64_t VertexShaderConstants;
    uint64_t PixelShaderConstants;
    uint64_t SharedConstants;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants;

#define g_TextureIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 0)
#define g_SamplerIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 4)
#define g_Fxaa vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 8)
#define g_SourceRect vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 16)

#else

cbuffer BlitConstants : register(b2, space4) {
    uint g_TextureIndex;
    uint g_SamplerIndex;
    // 1 runs FXAA over the source while it is scaled.
    uint g_Fxaa;
    float g_Padding;
    // Normalised source rectangle: xy the top-left corner, zw the size.
    float4 g_SourceRect;
};

#endif

struct VSOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

// A full-screen triangle from the vertex id, with no vertex buffer.
VSOutput VSMain(uint id : SV_VertexID) {
    VSOutput output;
    const float2 uv = float2((id << 1) & 2, id & 2);
    output.uv = uv;
    output.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}

float Luma(float3 color) {
    return dot(color, float3(0.299, 0.587, 0.114));
}

// FXAA in the manner of Lottes' FXAA 3.11 quality path: find the local edge from the luma of the source texel's neighbours,
// walk along it to its ends, and blend across it in proportion to how far the pixel is from the nearer end.
float4 Fxaa(Texture2D<float4> source, SamplerState samplerState, float2 uv) {
    uint width, height;
    source.GetDimensions(width, height);
    const float2 texel = 1.0 / float2(width, height);
    const float4 centre = source.SampleLevel(samplerState, uv, 0);
    const float lumaM = Luma(centre.rgb);
    const float lumaN = Luma(source.SampleLevel(samplerState, uv + float2(0, -1) * texel, 0).rgb);
    const float lumaS = Luma(source.SampleLevel(samplerState, uv + float2(0, 1) * texel, 0).rgb);
    const float lumaW = Luma(source.SampleLevel(samplerState, uv + float2(-1, 0) * texel, 0).rgb);
    const float lumaE = Luma(source.SampleLevel(samplerState, uv + float2(1, 0) * texel, 0).rgb);
    const float rangeMax = max(lumaM, max(max(lumaN, lumaS), max(lumaW, lumaE)));
    const float rangeMin = min(lumaM, min(min(lumaN, lumaS), min(lumaW, lumaE)));
    const float range = rangeMax - rangeMin;
    if (range < max(0.0312, rangeMax * 0.125)) return centre;

    const float lumaNW = Luma(source.SampleLevel(samplerState, uv + float2(-1, -1) * texel, 0).rgb);
    const float lumaNE = Luma(source.SampleLevel(samplerState, uv + float2(1, -1) * texel, 0).rgb);
    const float lumaSW = Luma(source.SampleLevel(samplerState, uv + float2(-1, 1) * texel, 0).rgb);
    const float lumaSE = Luma(source.SampleLevel(samplerState, uv + float2(1, 1) * texel, 0).rgb);
    const float edgeH = abs(lumaNW + lumaNE - 2.0 * lumaN) + 2.0 * abs(lumaW + lumaE - 2.0 * lumaM) + abs(lumaSW + lumaSE - 2.0 * lumaS);
    const float edgeV = abs(lumaNW + lumaSW - 2.0 * lumaW) + 2.0 * abs(lumaN + lumaS - 2.0 * lumaM) + abs(lumaNE + lumaSE - 2.0 * lumaE);
    const bool horizontal = edgeH >= edgeV;

    // The neighbour across the edge with the larger contrast decides which side the edge lies on.
    const float lumaPos = horizontal ? lumaS : lumaE;
    const float lumaNeg = horizontal ? lumaN : lumaW;
    const float gradientPos = abs(lumaPos - lumaM);
    const float gradientNeg = abs(lumaNeg - lumaM);
    const bool negative = gradientNeg >= gradientPos;
    const float gradient = max(gradientPos, gradientNeg) * 0.25;
    const float lumaEdge = 0.5 * (lumaM + (negative ? lumaNeg : lumaPos));
    const float2 across = horizontal ? float2(0, texel.y) : float2(texel.x, 0);
    const float2 along = horizontal ? float2(texel.x, 0) : float2(0, texel.y);
    const float2 edgeUv = uv + across * (negative ? -0.5 : 0.5);

    // Walk both ways until the luma along the edge leaves the edge's.
    static const float kSteps[10] = {1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0};
    float2 uvPos = edgeUv + along;
    float2 uvNeg = edgeUv - along;
    float endPos = Luma(source.SampleLevel(samplerState, uvPos, 0).rgb) - lumaEdge;
    float endNeg = Luma(source.SampleLevel(samplerState, uvNeg, 0).rgb) - lumaEdge;
    bool donePos = abs(endPos) >= gradient;
    bool doneNeg = abs(endNeg) >= gradient;
    [unroll] for (int i = 0; i < 10; ++i) {
        if (donePos && doneNeg) break;
        if (!donePos) {
            uvPos += along * kSteps[i];
            endPos = Luma(source.SampleLevel(samplerState, uvPos, 0).rgb) - lumaEdge;
            donePos = abs(endPos) >= gradient;
        }
        if (!doneNeg) {
            uvNeg -= along * kSteps[i];
            endNeg = Luma(source.SampleLevel(samplerState, uvNeg, 0).rgb) - lumaEdge;
            doneNeg = abs(endNeg) >= gradient;
        }
    }
    const float distPos = horizontal ? uvPos.x - uv.x : uvPos.y - uv.y;
    const float distNeg = horizontal ? uv.x - uvNeg.x : uv.y - uvNeg.y;
    const bool nearerPos = distPos < distNeg;
    const float dist = min(distPos, distNeg);
    const float span = distPos + distNeg;
    // Only blend when the nearer end is on the far side of the edge from this pixel.
    const bool correct = ((nearerPos ? endPos : endNeg) < 0.0) != (lumaM - lumaEdge < 0.0);
    float offset = correct ? 0.5 - dist / span : 0.0;

    // Sub-pixel aliasing: a pixel far from its neighbourhood's average gets pulled towards it.
    const float average = (2.0 * (lumaN + lumaS + lumaW + lumaE) + lumaNW + lumaNE + lumaSW + lumaSE) / 12.0;
    const float subpixel = saturate(abs(average - lumaM) / range);
    const float subpixelBlend = (-2.0 * subpixel + 3.0) * subpixel * subpixel;
    offset = max(offset, subpixelBlend * subpixelBlend * 0.75);

    return source.SampleLevel(samplerState, uv + across * (negative ? -offset : offset), 0);
}

float4 PSMain(VSOutput input) : SV_Target {
    const float4 rect = g_SourceRect;
    const float2 uv = rect.xy + input.uv * rect.zw;
    if (g_Fxaa != 0) return Fxaa(g_Texture2DDescriptorHeap[g_TextureIndex], g_SamplerDescriptorHeap[g_SamplerIndex], uv);
    return g_Texture2DDescriptorHeap[g_TextureIndex].Sample(g_SamplerDescriptorHeap[g_SamplerIndex], uv);
}
