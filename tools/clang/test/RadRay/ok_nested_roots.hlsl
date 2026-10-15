// RUN: %radray_probe %s 60 3 | FileCheck %s --check-prefix=CHECK

// CHECK-DAG: toolchain=0x0000000001090214
// CHECK-DAG: toolchain=0x0000000001090214
// CHECK-DAG: schema=8 headerSize=152 total={{[0-9]+}} target=0 stageMask=0x3
// CHECK-DAG: Inner group=0 binding=0 kind=CBuffer count=1 stages=0x1 placement=Table sampler=-1 flags=0x0 payload=InnerRoot
// CHECK-DAG: Outer group=0 binding=1 kind=CBuffer count=1 stages=0x2 placement=Table sampler=-1 flags=0x0 payload=OuterRoot
// CHECK-DAG: schema=8 headerSize=152 total={{[0-9]+}} target=1 stageMask=0x3
// CHECK-DAG: Inner group=0 binding=6 kind=CBuffer count=1 stages=0x1 placement=Table sampler=-1 flags=0x0 payload=InnerRoot
// CHECK-DAG: Outer group=0 binding=7 kind=CBuffer count=1 stages=0x2 placement=Table sampler=-1 flags=0x0 payload=OuterRoot
// CHECK-DAG: metadata deterministic=yes

struct InnerRoot {
    float4 InnerValue;
};

struct OuterRoot {
    InnerRoot Nested;
    float4 OuterValue;
};

[[vk::binding(6, 0)]] ConstantBuffer<InnerRoot> Inner : register(b0);
[[vk::binding(7, 0)]] ConstantBuffer<OuterRoot> Outer : register(b1);

struct VsOut {
    float4 Position : SV_Position;
};

[shader("vertex")]
VsOut VsMain(float3 position : POSITION) {
    VsOut output;
    output.Position = float4(position + Inner.InnerValue.xyz, 1.0f);
    return output;
}

[shader("pixel")]
float4 PsMain(VsOut input) : SV_Target0 {
    return Outer.Nested.InnerValue + Outer.OuterValue + input.Position.x * 0.0f;
}
