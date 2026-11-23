Texture2D<float> sourceDepth : register(t0, space2);

// SV_Position uses destination pixel centres. Integer Load preserves the
// source D32 value at the same pixel, with no UV scaling or interpolation.
float main(float4 position : SV_Position) : SV_Depth {
    return sourceDepth.Load(int3(uint2(position.xy), 0));
}
