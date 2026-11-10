// RUN: %radray_probe %s 60 3 | FileCheck %s

// CHECK: metadata deterministic=yes
// CHECK: DXIL lane
// CHECK: root constants (1):
// CHECK: Push space=0 register=0 offset=0 size=16 stages=0x3 flags=0x0 payload=none
// CHECK: SPIRV lane
// CHECK: root constants (0):

#define RS "RootConstants(num32BitConstants=4, b0)"
struct PushData { float4 Tint; };
[[vk::push_constant]] ConstantBuffer<PushData> Push : register(b0);

[shader("vertex")]
[RootSignature(RS)]
float4 VsMain(float3 position : POSITION) : SV_Position {
    return float4(position, 1.0);
}

[shader("pixel")]
[RootSignature(RS)]
float4 PsMain() : SV_Target0 { return float4(1.0, 1.0, 1.0, 1.0); }
