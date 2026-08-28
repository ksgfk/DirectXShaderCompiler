struct SceneData { float4 Color; };
#define RS "CBV(b0)"
[[vk::binding(0, 0)]] ConstantBuffer<SceneData> A : register(b0);
[[vk::binding(1, 0)]] Texture2D Extra : register(t0);
[[vk::binding(2, 0)]] SamplerState S : register(s0);

struct VsOut { float4 Position : SV_Position; float2 Uv : TEXCOORD0; };

[shader("vertex")]
[RootSignature(RS)]
VsOut VsMain(float3 position : POSITION, float2 uv : TEXCOORD0) {
    VsOut output;
    output.Position = float4(position, 1.0f);
    output.Uv = uv;
    return output;
}

[shader("pixel")]
[RootSignature(RS)]
float4 PsMain(VsOut input) : SV_Target0 {
    return A.Color + Extra.Sample(S, input.Uv);
}
