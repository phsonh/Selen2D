Texture2D<float4> source : register(t0,space2);
SamplerState imageSampler : register(s0,space2);
cbuffer FogState : register(b0,space3) {
    float4 fogRange; // start, end, curve mode, enabled
    float4 fogColor; // straight RGB, independent fog alpha
    float4 fogOperations; // Fog RGB enum, Fog Alpha enum, Add-prefix flag, glyph coverage mask
};

#include "image_sampling.hlsli"

// Keep the full VS output signature, including SV_Position, even when unused.
// Omitting it changes DXIL signature packing and breaks stage linkage.
struct ImageOutput {
    float4 color : SV_Target0;
    float distance : SV_Target1;
};
float StableLength(float3 value) {
    // Squaring otherwise valid world coordinates can overflow (1e20) or
    // underflow (1e-20). Normalize before the dot product; native admission
    // separately rejects a final norm which cannot be represented by R32_FLOAT.
    float scale=max(abs(value.x),max(abs(value.y),abs(value.z)));
    if(scale==0) return 0;
    precise float3 normalized=value/scale;
    return scale*length(normalized);
}
ImageOutput main(float4 position : SV_Position, float3 uvq : TEXCOORD0, float4 color : TEXCOORD1,
    nointerpolation float4 region : TEXCOORD2, nointerpolation float4 flags : TEXCOORD3,
    float3 relative : TEXCOORD4, nointerpolation float4 logical : TEXCOORD5) {
    // Homogeneous UV stays image-local. Physical allocation and fractional
    // source bounds remain separate through projective quads and 3D clipping.
    float2 local=uvq.xy/uvq.z;
    float2 localDx=ddx(local),localDy=ddy(local);
    uint width,height; source.GetDimensions(width,height);
    float2 textureSize=float2(width,height);
    int2 origin=int2(round(region.xy*textureSize));
    int2 coverage=max(int2(round((region.zw-region.xy)*textureSize)),int2(1,1));
    float4 texel;
    [branch] if(flags.y>3.5) {
        texel=EngineSampleImageGrad(source,imageSampler,local,localDx,localDy,region,logical,flags);
    } else if(flags.y>1.5) {
        texel=EngineSampleRegion(source,imageSampler,local,origin,coverage,logical,0,textureSize,flags.y>2.5,true);
    } else {
        float2 pixel=logical.xy+(flags.y>0.5 ? local : saturate(local))*logical.zw;
        if(flags.y<0.5) pixel=clamp(pixel,0.5,float2(coverage)-0.5);
        float2 uv=(float2(origin)+pixel)/textureSize;
        texel=source.SampleGrad(imageSampler,uv,localDx*logical.zw/textureSize,localDy*logical.zw/textureSize);
    }
    if(fogOperations.w>0.5 && texel.a==0) discard;
    float alpha=texel.a*color.a;
    // Image and Canvas use PMA filtering storage. Fog operates on
    // Straight RGB, then premultiplies once with its final (possibly faded) alpha.
    float3 rgb=flags.x>0.5 ? (texel.a>0 ? texel.rgb/texel.a : float3(0,0,0)) : texel.rgb;
    // RGB tint combination precedes Fog and premultiplication. The Add prefix
    // still MULTIPLIES texture/vertex Alpha; it never adds their Alpha values.
    // This uniform belongs to each recorded draw even when Fog is disabled.
    rgb=fogOperations.z>0.5 ? min(rgb+color.rgb,float3(1,1,1)) : rgb*color.rgb;
    float cameraDistance=StableLength(relative);
    if(fogRange.w>0.5) {
        // Interpolate 3D position first, then measure distance per fragment.
        // Vertex-distance interpolation would bend the fog incorrectly on large quads.
        float t=saturate((cameraDistance-fogRange.x)/(fogRange.y-fogRange.x));
        int mode=(int)fogRange.z;
        float v=mode>=4 ? 1-t : t;
        float f=v;
        if(mode!=0) {
            f=v*v;
            if(mode==2 || mode==3 || mode==5 || mode==6) f*=v;
            if(mode==3 || mode==6) f*=v;
        }
        if(mode>=4) f=1-f;
        int rgbMode=(int)fogOperations.x;
        if(rgbMode==0) rgb=lerp(rgb,fogColor.rgb,f);
        else if(rgbMode==1) rgb+=fogColor.rgb*f;
        else if(rgbMode==2) rgb-=fogColor.rgb*f;
        else if(rgbMode==3) rgb*=1-f+fogColor.rgb*f;
        else rgb=lerp(rgb,1-(1-rgb)*(1-fogColor.rgb),f);
        rgb=saturate(rgb);
        int alphaMode=(int)fogOperations.y;
        if(alphaMode==1) alpha*=1-f+f*fogColor.a;
        else if(alphaMode==2) alpha*=1-f;
    }
    ImageOutput output;
    output.color=float4(rgb*alpha,alpha);
    // Interpolate the camera-relative world position before taking its length.
    // Zero Alpha is still geometry. The 3D pipeline gates this write together
    // with depth; 2D has no bound target 1 and ignores this additional output.
    output.distance=cameraDistance;
    return output;
}
