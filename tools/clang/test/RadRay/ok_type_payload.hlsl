// RUN: %radray_probe %s 60 3 | FileCheck %s --check-prefix=CHECK

// CHECK-DAG: toolchain=0x0000000001090214
// CHECK-DAG: toolchain=0x0000000001090214
// CHECK-DAG: schema=8 headerSize=152 total={{[0-9]+}} target=0 stageMask=0x4
// CHECK-DAG: schema=8 headerSize=152 total={{[0-9]+}} target=1 stageMask=0x4
// CHECK-DAG: Payload group=0 binding=0 kind=CBuffer count=1 stages=0x4 placement=Table sampler=-1 flags=0x0 payload=PayloadData
// CHECK-DAG: Transform parent=0 kind=Matrix count=1 offset=0 size=64 stride=64 flags=0x0 scalar=float rows=4 cols=4 nested=none
// CHECK-DAG: ShadowSphere parent=0 kind=Array count=4 offset=64 size=64 stride=16 flags=0x0 scalar=float rows=1 cols=4 nested=none
// CHECK-DAG: Count parent=0 kind=Scalar count=1 offset=128 size=4 stride=4 flags=0x0 scalar=uint rows=1 cols=1 nested=none
// CHECK-DAG: SignedCount parent=0 kind=Scalar count=1 offset=132 size=4 stride=4 flags=0x0 scalar=sint rows=1 cols=1 nested=none
// CHECK-DAG: Scale parent=0 kind=Scalar count=1 offset=136 size=4 stride=4 flags=0x0 scalar=float rows=1 cols=1 nested=none

struct PayloadData {
    float4x4 Transform;
    float4 ShadowSphere[4];
    uint Count;
    int SignedCount;
    float Scale;
};

[[vk::binding(0, 0)]] ConstantBuffer<PayloadData> Payload : register(b0);
[[vk::binding(1, 0)]] RWStructuredBuffer<float4> Output : register(u0);

[shader("compute")]
[numthreads(1, 1, 1)]
void CsMain(uint3 id : SV_DispatchThreadID) {
    float4 value = Payload.ShadowSphere[id.x & 3];
    value += Payload.Transform[0];
    value += Payload.Count;
    value += Payload.SignedCount;
    value += Payload.Scale;
    Output[id.x] = value;
}
