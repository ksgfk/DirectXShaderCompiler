struct SceneData { float4 Color; };
[[vk::binding(0, 0)]] ConstantBuffer<SceneData> A : register(b0);
[[vk::binding(1, 0)]] Texture2D Albedo : register(t0);
[[vk::binding(2, 0)]] SamplerState S : register(s0);
[[vk::binding(3, 0)]] ByteAddressBuffer Raw : register(t1);
[[vk::binding(4, 0)]] RWBuffer<float4> Typed : register(u0);

struct VsOut { float4 Position : SV_Position; float2 Uv : TEXCOORD0; };

[shader("vertex")]

VsOut VsMain(float3 position : POSITION, float2 uv : TEXCOORD0) {
    VsOut output;
    output.Position = float4(position, 1.0f);
    output.Uv = uv;
    return output;
}

[shader("pixel")]

float4 PsMain(VsOut input) : SV_Target0 {
    return A.Color + Albedo.Sample(S, input.Uv) + asfloat(Raw.Load4(0)) + Typed[0];
}
