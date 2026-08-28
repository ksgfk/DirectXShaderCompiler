struct SceneData { float4 Color; };
[[vk::binding(0, 0)]] TextureBuffer<SceneData> Tb : register(t0);

[shader("pixel")]
float4 PsMain() : SV_Target0 {
    return Tb.Color;
}

[shader("vertex")]

float4 VsMain(float3 position : POSITION) : SV_Position {
    return float4(position, 1.0f);
}
