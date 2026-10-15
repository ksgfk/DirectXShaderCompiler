// RUN: not %radray_probe %s 60 3 | FileCheck %s --check-prefix=CHECK

// CHECK-DAG: [2122] resource 'Points' cannot be a root descriptor: only a single buffer or constant buffer can be bound by address

#define RS "SRV(t0)"
[[vk::binding(0, 0)]] StructuredBuffer<float4> Points[2] : register(t0);

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
    return Points[0][0] + Points[1][0];
}
