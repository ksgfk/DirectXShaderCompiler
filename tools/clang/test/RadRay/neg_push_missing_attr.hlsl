// RUN: not %radray_probe %s 60 3 | FileCheck %s --check-prefix=CHECK

// CHECK-DAG: [2124] 'Push' is root constants in the policy, so it needs {{\[\[vk::push_constant\]\]}} to lower the same way on both targets

#define RS "RootConstants(num32BitConstants=4, b0)"
struct PushData { float4 Tint; };
ConstantBuffer<PushData> Push : register(b0);

struct VsOut { float4 Position : SV_Position; float2 Uv : TEXCOORD0; };

[shader("vertex")]
[RootSignature(RS)]
VsOut VsMain(float3 position : POSITION, float2 uv : TEXCOORD0) {
    VsOut output;
    output.Position = float4(position, 1.0f);
    output.Uv = uv;
    return output;
}

[shader("pixel")]
[RootSignature(RS)]
float4 PsMain(VsOut input) : SV_Target0 {
    return Push.Tint;
}
