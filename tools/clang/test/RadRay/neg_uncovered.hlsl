// RUN: not %radray_probe %s 60 1 | FileCheck %s --check-prefix=DXIL
// RUN: %radray_probe %s 60 2 | FileCheck %s --check-prefix=SPIRV

// SPIRV-DAG: toolchain=0x0000000001090214
// DXIL-DAG: [2121] resource 'Extra' is not covered by the [RootSignature] policy
// DXIL-DAG: [2121] resource 'S' is not covered by the [RootSignature] policy
// SPIRV-DAG: A group=0 binding=0 kind=CBuffer count=1 stages=0x2 placement=RootDescriptor sampler=-1 flags=0x0 payload=SceneData
// SPIRV-DAG: Extra group=0 binding=1 kind=Texture count=1 stages=0x2 placement=Table sampler=-1 flags=0x0 payload=none
// SPIRV-DAG: S group=0 binding=2 kind=Sampler count=1 stages=0x2 placement=Table sampler=-1 flags=0x0 payload=none

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
