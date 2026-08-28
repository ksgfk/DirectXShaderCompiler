struct SceneData { float4 Color; };
#define RS_A "CBV(b0)"
#define RS_B "DescriptorTable(CBV(b0))"
[[vk::binding(0, 0)]] ConstantBuffer<SceneData> A : register(b0);

struct VsOut { float4 Position : SV_Position; float2 Uv : TEXCOORD0; };

[shader("vertex")]
[RootSignature(RS_A)]
VsOut VsMain(float3 position : POSITION, float2 uv : TEXCOORD0) {
    VsOut output;
    output.Position = float4(position, 1.0f);
    output.Uv = uv;
    return output;
}

[shader("pixel")]
[RootSignature(RS_B)]
float4 PsMain(VsOut input) : SV_Target0 {
    return A.Color;
}
