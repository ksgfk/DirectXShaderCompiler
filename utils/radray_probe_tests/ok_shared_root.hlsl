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
