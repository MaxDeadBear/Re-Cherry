// The ImGui overlays (src/gpu/ui/ui_pass.cpp): clip-space vertices from the immediate drawer, one RGBA texture per draw, alpha blended over the presented image.
// Its own layout, not the bindless one: plume numbers bindings flat across descriptor types and the D3D register must equal the binding, hence t0 and s1.
// Compiled offline by tools/shaders/build_builtin_shaders.py.

struct VSOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 colour : COLOR0;
};

VSOutput VSMain(float2 position : POSITION, float2 uv : TEXCOORD0, float4 colour : COLOR0) {
    VSOutput output;
    output.position = float4(position, 0.0, 1.0);
    output.uv = uv;
    output.colour = colour;
    return output;
}

[[vk::binding(0, 0)]] Texture2D<float4> g_Texture : register(t0, space0);
[[vk::binding(1, 0)]] SamplerState g_Sampler : register(s1, space0);

float4 PSMain(VSOutput input) : SV_Target {
    return input.colour * g_Texture.Sample(g_Sampler, input.uv);
}
