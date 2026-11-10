// RUN: %radray_probe %s 60 2 | FileCheck %s

// CHECK: metadata deterministic=yes
// CHECK: SPIRV lane
// CHECK: root constants (1):
// CHECK: Push space=0 register=0 offset=0 size=16 stages=0x1 flags=0x1 payload=PushData

struct PushData { float4 Offset; };
[[vk::push_constant]] ConstantBuffer<PushData> Push : register(b0);

[shader("vertex")]
float4 VsMain(float3 position : POSITION) : SV_Position {
    return float4(position, 1.0) + Push.Offset;
}

[shader("pixel")]
float4 PsMain() : SV_Target0 { return float4(1.0, 1.0, 1.0, 1.0); }
