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
