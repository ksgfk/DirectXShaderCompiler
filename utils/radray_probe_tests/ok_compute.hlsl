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
