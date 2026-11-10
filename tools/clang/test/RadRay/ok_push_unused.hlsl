// RUN: %radray_probe %s 60 2 | FileCheck %s

// CHECK: metadata deterministic=yes
// CHECK: SPIRV lane
// CHECK: root constants (0):

struct PushData { float4 Tint; };
[[vk::push_constant]] ConstantBuffer<PushData> Push : register(b0);

[shader("vertex")]
float4 VsMain(float3 position : POSITION) : SV_Position {
    return float4(position, 1.0);
}

[shader("pixel")]
float4 PsMain() : SV_Target0 { return float4(1.0, 1.0, 1.0, 1.0); }
