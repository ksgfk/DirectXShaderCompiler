// RUN: %radray_probe %s 60 1 | FileCheck %s
// CHECK: compile status=0
// CHECK: Matrices{{.*}}kind=Array{{.*}}count=1 offset=0 size=44 stride=48

cbuffer Constants : register(b0) { float3x3 Matrices[1]; };
RWStructuredBuffer<float> Output : register(u0);
[shader("compute")][numthreads(1,1,1)]
void CSMain() {
  [unroll] for (uint r = 0; r < 3; ++r)
    [unroll] for (uint c = 0; c < 3; ++c)
      Output[r * 3 + c] = Matrices[0][r][c];
}
