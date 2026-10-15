// RUN: not %radray_probe %s 60 2 | FileCheck %s --check-prefix=SPIRV
// RUN: not %radray_probe %s 60 1 | FileCheck %s --check-prefix=DXIL

// SPIRV-DAG: [2120] push constant block 'Push' needs a register() annotation to be placed on both targets
// DXIL-DAG: [2111] DXIL resource 'Push' is missing an explicit register() binding

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
