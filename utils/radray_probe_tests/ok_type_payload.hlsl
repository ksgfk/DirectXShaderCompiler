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
