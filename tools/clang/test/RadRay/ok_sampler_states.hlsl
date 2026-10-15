// RUN: %radray_probe %s 60 3 | FileCheck %s --check-prefix=CHECK

// CHECK-DAG: toolchain=0x0000000001090214
// CHECK-DAG: toolchain=0x0000000001090214
// CHECK-DAG: Shadow group=0 binding=0 kind=Sampler count=1 stages=0x2 placement=StaticSampler sampler=-1 flags=0x0 payload=none
// CHECK-DAG: Shadow group=0 binding=1 kind=Sampler count=1 stages=0x2 placement=Table sampler=0 flags=0x0 payload=none
// CHECK-DAG: MinPoint group=0 binding=1 kind=Sampler count=1 stages=0x2 placement=StaticSampler sampler=-1 flags=0x0 payload=none
// CHECK-DAG: MinPoint group=0 binding=2 kind=Sampler count=1 stages=0x2 placement=Table sampler=1 flags=0x0 payload=none
// CHECK-DAG: samplers (2):
// CHECK-DAG: mag=1 min=1 mip=1 addr=1/4/3 bias=1.5 aniso=1/8.0 cmp=1/4 lod=0.0..1000.0 border=2 reduction=0 flags=0x0
// CHECK-DAG: mag=0 min=0 mip=0 addr=0/0/0 bias=0.0 aniso=0/1.0 cmp=0/3 lod=0.0..1000.0 border=4 reduction=1 flags=0x0

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
