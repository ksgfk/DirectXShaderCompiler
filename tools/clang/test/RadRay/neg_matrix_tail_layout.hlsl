// RUN: %radray_probe %s 60 1 | FileCheck %s --check-prefix=DXIL
// RUN: %radray_probe %s 60 2 | FileCheck %s --check-prefix=SPIRV
// RUN: not %radray_probe %s 60 3 | FileCheck %s --check-prefix=PAIRED
// DXIL: Tail{{.*}}offset=24 size=4
// SPIRV: Tail{{.*}}offset=32 size=4
// PAIRED: [2108] DXIL and SPIR-V disagree on type metadata

struct Payload { float2x2 M; float Tail; };
[[vk::binding(0, 0)]] ConstantBuffer<Payload> Constants : register(b0);
[[vk::binding(1, 0)]] RWStructuredBuffer<float> Output : register(u0);
[shader("compute")][numthreads(1,1,1)]
void CSMain() {
  Output[0] = Constants.M[0][0] + Constants.M[0][1] +
              Constants.M[1][0] + Constants.M[1][1] + Constants.Tail;
}
