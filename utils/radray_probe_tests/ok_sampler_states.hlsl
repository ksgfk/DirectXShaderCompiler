#define RS "DescriptorTable(SRV(t0)), StaticSampler(s0, filter=FILTER_COMPARISON_ANISOTROPIC, maxAnisotropy=8, addressU=TEXTURE_ADDRESS_MIRROR, addressV=TEXTURE_ADDRESS_MIRROR_ONCE, addressW=TEXTURE_ADDRESS_BORDER, comparisonFunc=COMPARISON_GREATER, borderColor=STATIC_BORDER_COLOR_OPAQUE_BLACK, mipLODBias=1.5), StaticSampler(s1, filter=FILTER_MINIMUM_MIN_MAG_MIP_POINT)"
[[vk::binding(0, 0)]] Texture2D Albedo : register(t0);
[[vk::binding(1, 0)]] SamplerComparisonState Shadow : register(s0);
[[vk::binding(2, 0)]] SamplerState MinPoint : register(s1);

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
    return Albedo.SampleCmp(Shadow, input.Uv, 0.5f) + Albedo.Sample(MinPoint, input.Uv);
}
