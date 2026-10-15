// RUN: %radray_probe %s 60 3 | FileCheck %s --check-prefix=CHECK --implicit-check-not=placement=RootDescriptor --implicit-check-not=placement=StaticSampler

// CHECK-DAG: toolchain=0x0000000001090214
// CHECK-DAG: toolchain=0x0000000001090214
// CHECK-DAG: rootSignature=0 bytes
// CHECK-DAG: A group=0 binding=0 kind=CBuffer count=1 stages=0x2 placement=Table sampler=-1 flags=0x0 payload=SceneData
// CHECK-DAG: Raw group=0 binding=1 kind=RawBuffer count=1 stages=0x2 placement=Table sampler=-1 flags=0x0 payload=none
// CHECK-DAG: Typed group=0 binding=0 kind=RWTypedBuffer count=1 stages=0x2 placement=Table sampler=-1 flags=0x0 payload=none
// CHECK-DAG: root constants (0):
// CHECK-DAG: samplers (0):

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
