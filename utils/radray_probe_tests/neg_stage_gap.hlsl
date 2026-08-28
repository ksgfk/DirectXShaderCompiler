struct SceneData { float4 Color; };
#define RS "CBV(b0, visibility=SHADER_VISIBILITY_PIXEL)"
[[vk::binding(0, 0)]] ConstantBuffer<SceneData> A : register(b0);

struct VsOut { float4 Position : SV_Position; };

[shader("vertex")]
[RootSignature(RS)]
VsOut VsMain(float3 position : POSITION) {
    VsOut output;
    output.Position = float4(position, 1.0f) + A.Color;
    return output;
}

[shader("pixel")]
[RootSignature(RS)]
float4 PsMain(VsOut input) : SV_Target0 {
    return A.Color;
}
