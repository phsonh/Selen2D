struct Input {
    float4 position : TEXCOORD0;
    float3 uvq : TEXCOORD1;
    float4 color : TEXCOORD2;
    float4 region : TEXCOORD3;
    float4 flags : TEXCOORD4; // premultiplied, sampling mode, logical mip levels, extrusion
    float3 relative : TEXCOORD5;
    float4 logical : TEXCOORD6; // coverage-relative pixel inset and extent
};
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
    output.position=input.position;
    output.uvq=input.uvq;
    output.color=input.color;
    output.region=input.region;
    output.flags=input.flags;
    output.relative=input.relative;
    output.logical=input.logical;
    return output;
}
