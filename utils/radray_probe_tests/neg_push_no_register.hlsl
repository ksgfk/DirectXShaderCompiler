struct PushData { float4 Tint; };
#define RS "RootConstants(num32BitConstants=4, b0)"
[[vk::push_constant]] ConstantBuffer<PushData> Push;

[shader("pixel")]
[RootSignature(RS)]
float4 PsMain() : SV_Target0 {
    return Push.Tint;
}

[shader("vertex")]
[RootSignature(RS)]
float4 VsMain(float3 position : POSITION) : SV_Position {
    return float4(position, 1.0f);
}
