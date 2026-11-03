#ifndef EXAMPLE_PALETTE_HLSL
#define EXAMPLE_PALETTE_HLSL

// Ordinary HLSL helper functions can live in relative includes.
float3 AnimatedAccent(float3 baseColor, float seconds)
{
    float amount = 0.5 + 0.5 * sin(seconds * 0.7);
    return lerp(baseColor, baseColor.brg, amount * 0.28);
}

float SoftDisk(float distanceFromCenter, float radius, float edge)
{
    return 1.0 - smoothstep(radius - edge, radius + edge, distanceFromCenter);
}

#endif
