// RUN: %radray_probe %s | FileCheck %s
// CHECK: compile status=0
// CHECK: schema=9
// CHECK: Matrices{{.*}}kind=Array{{.*}}count=2 offset=0 size=92 stride=48{{.*}}rows=3 cols=3{{.*}}element=Matrix elementSize=44 matrixStride=16
// CHECK: schema=9
// CHECK: Matrices{{.*}}kind=Array{{.*}}count=2 offset=0 size=92 stride=48{{.*}}rows=3 cols=3{{.*}}element=Matrix elementSize=44 matrixStride=16

struct Payload { float3x3 Matrices[2]; };
#ifdef __spirv__
[[vk::binding(0, 0)]]
#endif
ConstantBuffer<Payload> Constants : register(b0);
#ifdef __spirv__
[[vk::binding(1, 0)]]
#endif
RWStructuredBuffer<float> Output : register(u0);

[shader("compute")]
[numthreads(1, 1, 1)]
void CSMain() {
  [unroll] for (uint i = 0; i < 2; ++i)
    [unroll] for (uint r = 0; r < 3; ++r)
      [unroll] for (uint c = 0; c < 3; ++c)
        Output[i * 9 + r * 3 + c] = Constants.Matrices[i][r][c];
}
