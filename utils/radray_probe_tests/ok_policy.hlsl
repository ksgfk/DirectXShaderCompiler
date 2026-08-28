#define RS \
    "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT)," \
    "RootConstants(num32BitConstants=4, b0)," \
    "CBV(b1)," \
    "DescriptorTable(SRV(t0, numDescriptors=2), UAV(u0))," \
    "SRV(t2)," \
    "StaticSampler(s0, filter=FILTER_MIN_MAG_MIP_LINEAR, addressU=TEXTURE_ADDRESS_CLAMP," \
    "              addressV=TEXTURE_ADDRESS_BORDER, comparisonFunc=COMPARISON_LESS_EQUAL," \
    "              borderColor=STATIC_BORDER_COLOR_OPAQUE_WHITE, maxLOD=8.0)"

struct PushData {
    float4 Tint;
};

[[vk::push_constant]] ConstantBuffer<PushData> Push : register(b0);

struct SceneData {
    float4x4 ViewProjection;
};

[[vk::binding(0, 0)]] ConstantBuffer<SceneData> Scene : register(b1);
[[vk::binding(1, 0)]] Texture2D Albedo[2] : register(t0);
[[vk::binding(3, 0)]] RWStructuredBuffer<float4> Output : register(u0);
[[vk::binding(4, 0)]] StructuredBuffer<float4> Points : register(t2);
[[vk::binding(5, 0)]] SamplerState LinearClamp : register(s0);

struct VsOut {
    float4 Position : SV_Position;
    float2 Uv : TEXCOORD0;
};

[shader("vertex")]
[RootSignature(RS)]
VsOut VsMain(float3 position : POSITION, float2 uv : TEXCOORD0) {
    VsOut output;
    output.Position = mul(Scene.ViewProjection, float4(position, 1.0f));
    output.Uv = uv;
    return output;
}

[shader("pixel")]
[RootSignature(RS)]
float4 PsMain(VsOut input) : SV_Target0 {
    float4 color = Albedo[0].Sample(LinearClamp, input.Uv);
    color += Albedo[1].Sample(LinearClamp, input.Uv * 0.5f);
    color *= Push.Tint;
    color += Points[0];
    Output[0] = color;
    return color;
}
