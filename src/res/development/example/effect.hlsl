#include "Solongo.hlsl"
#include "palette.hlsl"

// Lua keys match these field names exactly. The engine assigns GPU bindings.
cbuffer Params
{
    float elapsed;
    float2 center;
    float radius;
    float3 accentColor;
    float3 backgroundColor;
};

float4 PSMain(PSInput input) : SV_Target0
{
    // Coordinates use the target height as one unit: center is (0,0), Y is up.
    float2 position = (input.uv - 0.5) * float2(SolongoTargetSize.x / SolongoTargetSize.y, 1.0);
    position.y = -position.y;
    float edge = max(1.25 * SolongoTargetInvSize.y, 0.0001);
    float activeRadius = radius * (1.0 + 0.045 * sin(elapsed * 2.4));
    float distanceFromCenter = length(position - center);
    float disk = SoftDisk(distanceFromCenter, activeRadius, edge);
    float ring = 1.0 - smoothstep(edge, edge * 2.0, abs(distanceFromCenter - activeRadius * 1.45));
    float glow = exp(-max(distanceFromCenter - activeRadius, 0.0) / max(radius * 0.3, 0.001));

    float2 gridDistance = abs(frac(position * 8.0 + 0.5) - 0.5) / 8.0;
    float grid = 1.0 - smoothstep(edge, edge * 2.0, min(gridDistance.x, gridDistance.y));
    float3 accent = AnimatedAccent(accentColor, elapsed);
    float3 color = backgroundColor * (0.7 + 0.3 * saturate(1.0 - length(position)));
    color += float3(0.014, 0.020, 0.030) * grid;
    color += accent * (0.16 * glow + 0.32 * ring);
    color = lerp(color, accent * 0.88, disk);

    float2 highlightCenter = center + float2(-0.28, 0.32) * activeRadius;
    float highlight = 1.0 - smoothstep(0.0, activeRadius * 0.85, length(position - highlightCenter));
    color += float3(0.40, 0.45, 0.50) * highlight * disk;
    return float4(saturate(color), 1.0);
}
