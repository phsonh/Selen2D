// SDL's graphics storage buffers use raw views on D3D12. Each immutable
// sampling record consists of three aligned float4 blocks (48 bytes).
ByteAddressBuffer spriteParameters : register(t0,space0);
struct Input {
    float2 position : TEXCOORD0;
    float3 uvq : TEXCOORD1;
    float4 color : TEXCOORD2;
    uint parameterIndex : TEXCOORD3;
};
// Keep the full legacy VS output signature for the shared fragment shader.
struct Output {
    float4 position : SV_Position;
    float3 uvq : TEXCOORD0;
    float4 color : TEXCOORD1;
    nointerpolation float4 region : TEXCOORD2;
    nointerpolation float4 flags : TEXCOORD3;
    float3 relative : TEXCOORD4;
    nointerpolation float4 logical : TEXCOORD5;
};
Output main(Input input) {
    Output output;
    uint address=input.parameterIndex*48;
    output.position=float4(input.position,0,1);
    output.uvq=input.uvq;
    output.color=input.color;
    output.region=asfloat(spriteParameters.Load4(address));
    output.flags=asfloat(spriteParameters.Load4(address+16));
    output.relative=float3(0,0,0);
    output.logical=asfloat(spriteParameters.Load4(address+32));
    return output;
}
