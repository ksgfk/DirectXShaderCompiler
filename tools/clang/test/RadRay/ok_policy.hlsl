// RUN: %radray_probe %s 60 3 | FileCheck %s --check-prefix=CHECK

// CHECK-DAG: toolchain=0x0000000001090214
// CHECK-DAG: toolchain=0x0000000001090214
// CHECK-DAG: DXIL lane
// CHECK-DAG: schema=8 headerSize=152 total={{[0-9]+}} target=0 stageMask=0x3
// CHECK-DAG: rootSignature=260 bytes
// CHECK-DAG: Scene group=0 binding=1 kind=CBuffer count=1 stages=0x1 placement=RootDescriptor sampler=-1 flags=0x0 payload=SceneData
// CHECK-DAG: Albedo group=0 binding=0 kind=Texture count=2 stages=0x2 placement=Table sampler=-1 flags=0x0 payload=none
// CHECK-DAG: Points group=0 binding=2 kind=StructuredBuffer count=1 stages=0x2 placement=RootDescriptor sampler=-1 flags=0x0 payload=none
// CHECK-DAG: Output group=0 binding=0 kind=RWStructuredBuffer count=1 stages=0x2 placement=Table sampler=-1 flags=0x0 payload=none
// CHECK-DAG: LinearClamp group=0 binding=0 kind=Sampler count=1 stages=0x2 placement=StaticSampler sampler=-1 flags=0x0 payload=none
// CHECK-DAG: Push space=0 register=0 offset=0 size=16 stages=0x3 flags=0x0 payload=PushData
// CHECK-DAG: samplers (0):
// CHECK-DAG: SPIRV lane
// CHECK-DAG: schema=8 headerSize=152 total={{[0-9]+}} target=1 stageMask=0x3
// CHECK-DAG: rootSignature=0 bytes
// CHECK-DAG: Scene group=0 binding=0 kind=CBuffer count=1 stages=0x1 placement=RootDescriptor sampler=-1 flags=0x0 payload=SceneData
// CHECK-DAG: Albedo group=0 binding=1 kind=Texture count=2 stages=0x2 placement=Table sampler=-1 flags=0x0 payload=none
// CHECK-DAG: Output group=0 binding=3 kind=RWStructuredBuffer count=1 stages=0x2 placement=Table sampler=-1 flags=0x0 payload=none
// CHECK-DAG: Points group=0 binding=4 kind=StructuredBuffer count=1 stages=0x2 placement=RootDescriptor sampler=-1 flags=0x0 payload=none
// CHECK-DAG: LinearClamp group=0 binding=5 kind=Sampler count=1 stages=0x2 placement=Table sampler=0 flags=0x0 payload=none
// CHECK-DAG: Push space=0 register=0 offset=0 size=16 stages=0x3 flags=0x0 payload=PushData
// CHECK-DAG: samplers (1):
// CHECK-DAG: mag=1 min=1 mip=1 addr=2/3/0 bias=0.0 aniso=0/1.0 cmp=0/3 lod=0.0..8.0 border=4 reduction=0 flags=0x0

#define RS \
    "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT)," \
    "RootConstants(num32BitConstants=4, b0)," \
    "CBV(b1)," \
    "DescriptorTable(SRV(t0, numDescriptors=2), UAV(u0))," \
    "SRV(t2)," \
    "StaticSampler(s0, filter=FILTER_MIN_MAG_MIP_LINEAR, addressU=TEXTURE_ADDRESS_CLAMP," \
    "              addressV=TEXTURE_ADDRESS_BORDER, comparisonFunc=COMPARISON_LESS_EQUAL," \
    "              borderColor=STATIC_BORDER_COLOR_OPAQUE_WHITE, maxLOD=8.0)"

struct PushData {
    float4 Tint;
};

[[vk::push_constant]] ConstantBuffer<PushData> Push : register(b0);

struct SceneData {
    float4x4 ViewProjection;
};

[[vk::binding(0, 0)]] ConstantBuffer<SceneData> Scene : register(b1);
[[vk::binding(1, 0)]] Texture2D Albedo[2] : register(t0);
[[vk::binding(3, 0)]] RWStructuredBuffer<float4> Output : register(u0);
[[vk::binding(4, 0)]] StructuredBuffer<float4> Points : register(t2);
[[vk::binding(5, 0)]] SamplerState LinearClamp : register(s0);

struct VsOut {
    float4 Position : SV_Position;
    float2 Uv : TEXCOORD0;
};

[shader("vertex")]
[RootSignature(RS)]
VsOut VsMain(float3 position : POSITION, float2 uv : TEXCOORD0) {
    VsOut output;
    output.Position = mul(Scene.ViewProjection, float4(position, 1.0f));
    output.Uv = uv;
    return output;
}

[shader("pixel")]
[RootSignature(RS)]
float4 PsMain(VsOut input) : SV_Target0 {
    float4 color = Albedo[0].Sample(LinearClamp, input.Uv);
    color += Albedo[1].Sample(LinearClamp, input.Uv * 0.5f);
    color *= Push.Tint;
    color += Points[0];
    Output[0] = color;
    return color;
}
