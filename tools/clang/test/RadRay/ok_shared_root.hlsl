// RUN: %radray_probe %s 60 3 | FileCheck %s --check-prefix=CHECK

// CHECK-DAG: toolchain=0x0000000001090214
// CHECK-DAG: toolchain=0x0000000001090214
// CHECK-DAG: schema=8 headerSize=152 total={{[0-9]+}} target=0 stageMask=0x3
// CHECK-DAG: First group=0 binding=0 kind=CBuffer count=1 stages=0x1 placement=Table sampler=-1 flags=0x0 payload=SharedRoot
// CHECK-DAG: Second group=0 binding=1 kind=CBuffer count=1 stages=0x2 placement=Table sampler=-1 flags=0x0 payload=SharedRoot
// CHECK-DAG: schema=8 headerSize=152 total={{[0-9]+}} target=1 stageMask=0x3
// CHECK-DAG: First group=0 binding=4 kind=CBuffer count=1 stages=0x1 placement=Table sampler=-1 flags=0x0 payload=SharedRoot
// CHECK-DAG: Second group=0 binding=5 kind=CBuffer count=1 stages=0x2 placement=Table sampler=-1 flags=0x0 payload=SharedRoot
// CHECK-DAG: metadata deterministic=yes

struct SharedRoot {
    float4 Value;
};

[[vk::binding(4, 0)]] ConstantBuffer<SharedRoot> First : register(b0);
[[vk::binding(5, 0)]] ConstantBuffer<SharedRoot> Second : register(b1);

struct VsOut {
    float4 Position : SV_Position;
};

[shader("vertex")]
VsOut VsMain(float3 position : POSITION) {
    VsOut output;
    output.Position = float4(position + First.Value.xyz, 1.0f);
    return output;
}

[shader("pixel")]
float4 PsMain(VsOut input) : SV_Target0 {
    return Second.Value + input.Position.x * 0.0f;
}
