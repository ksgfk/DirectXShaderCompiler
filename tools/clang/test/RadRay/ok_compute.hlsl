// RUN: %radray_probe %s 60 3 | FileCheck %s --check-prefix=CHECK

// CHECK-DAG: toolchain=0x0000000001090214
// CHECK-DAG: toolchain=0x0000000001090214
// CHECK-DAG: schema=8 headerSize=152 total={{[0-9]+}} target=0 stageMask=0x4
// CHECK-DAG: schema=8 headerSize=152 total={{[0-9]+}} target=1 stageMask=0x4
// CHECK-DAG: Scene group=0 binding=1 kind=CBuffer count=1 stages=0x4 placement=RootDescriptor sampler=-1 flags=0x0 payload=SceneData
// CHECK-DAG: Push space=0 register=0 offset=0 size=4 stages=0x4 flags=0x0 payload=PushData

struct SceneData { float4 Color; };
struct PushData { uint Count; };
#define RS \
    "RootConstants(num32BitConstants=1, b0)," \
    "CBV(b1)," \
    "DescriptorTable(UAV(u0))"

[[vk::push_constant]] ConstantBuffer<PushData> Push : register(b0);
[[vk::binding(0, 0)]] ConstantBuffer<SceneData> Scene : register(b1);
[[vk::binding(1, 0)]] RWStructuredBuffer<float4> Output : register(u0);

[shader("compute")]
[RootSignature(RS)]
[numthreads(64, 1, 1)]
void CsMain(uint3 id : SV_DispatchThreadID) {
    if (id.x < Push.Count)
        Output[id.x] = Scene.Color;
}
