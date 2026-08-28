struct SceneData { float4 Color; };
#define RS "CBV(b0), StaticSampler(s0, addressU=TEXTURE_ADDRESS_MIRRORONCE)"
[[vk::binding(0, 0)]] ConstantBuffer<SceneData> A : register(b0);

[shader("vertex")]
[RootSignature(RS)]
float4 VsMain(float3 position : POSITION) : SV_Position {
    return float4(position, 1.0f) + A.Color;
}
