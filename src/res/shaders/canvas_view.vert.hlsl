// An oversized fullscreen triangle; no Mesh, vertex buffer, Camera or Fog.
float4 main(uint vertex : SV_VertexID) : SV_Position {
    float2 corner = float2((vertex << 1) & 2, vertex & 2);
    return float4(corner * float2(2, -2) + float2(-1, 1), 0, 1);
}
