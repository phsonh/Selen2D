Texture2D<float4> source : register(t0, space2);
// Snapshot raw PMA channels, including nonzero RGB at zero Alpha. Filtering
// occurs later; only its result is unassociated by the color sampling helper.
SamplerState sourceSampler : register(s0, space2);
float4 main(float4 position : SV_Position) : SV_Target0 {
    float4 value = source.Load(int3(int2(position.xy), 0));
    return value;
}
