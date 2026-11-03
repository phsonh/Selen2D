Texture2D<float4> source : register(t0, space2);
// SDL binds one texture/sampler pair; conversion uses exact pixel Load, never
// interpolates PMA data before unassociation, and does not apply Fog or blend.
SamplerState sourceSampler : register(s0, space2);
float4 main(float4 position : SV_Position) : SV_Target0 {
    float4 value = source.Load(int3(int2(position.xy), 0));
    float3 rgb = value.a > 0 ? value.rgb / value.a : float3(0, 0, 0);
    // RGBA16_FLOAT intentionally retains RGB > 1 (e.g. additive PMA content).
    return float4(rgb, value.a);
}
