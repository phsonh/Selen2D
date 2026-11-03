Texture2D<float4> source : register(t0,space2);
SamplerState imageSampler : register(s0,space2);
cbuffer FogState : register(b0,space3) {
    float4 fogRange; // start, end, curve mode, enabled
    float4 fogColor; // straight RGB, independent fog alpha
    float4 fogOperations; // Fog RGB enum, Fog Alpha enum, Image Add-prefix flag, reserved
};

// Non-mipmapped Atlas regions repeat in logical Image space. Texture sampler
// Wrap only addresses a whole Texture, so it cannot provide this boundary.
float4 SampleLocalRepeat(float2 uv, float4 region, bool linearFilter) {
    uint width, height;
    source.GetDimensions(width, height);
    float2 textureSize=float2(width,height);
    int2 origin=int2(round(region.xy*textureSize));
    int2 extent=max(int2(round((region.zw-region.xy)*textureSize)),int2(1,1));
    // Wrap before converting to integer pixels: negative UVs work, and a large
    // repeat count cannot overflow the bounded per-region integer coordinates.
    float2 wrapped=frac((uv-region.xy)/(region.zw-region.xy));
    if(!linearFilter) {
        int2 pixel=min(int2(floor(wrapped*float2(extent))),extent-1);
        return source.Load(int3(origin+pixel,0));
    }
    // Texel centers lie at n+0.5. At u/v=0 the two neighboring texels must be
    // the last and first Image texels, not its extruded edge or a neighbor tile.
    float2 pixel=wrapped*float2(extent)-0.5;
    int2 low=int2(floor(pixel));
    float2 weight=frac(pixel);
    int2 first=(low+extent)%extent;
    int2 second=(first+1)%extent;
    float4 a=source.Load(int3(origin+first,0));
    float4 b=source.Load(int3(origin+int2(second.x,first.y),0));
    float4 c=source.Load(int3(origin+int2(first.x,second.y),0));
    float4 d=source.Load(int3(origin+second,0));
    return lerp(lerp(a,b,weight.x),lerp(c,d,weight.x),weight.y);
}

// Logical mips are independently downsampled and extruded CPU images. Their
// physical texture has only LOD 0: never ask the GPU to downsample an Atlas page.
// Each strip keeps the same content X; its smaller levels follow downwards.
void StripLevel(uint level, int2 baseOrigin, uint2 baseExtent, uint extrusion,
    out int2 origin, out int2 extent) {
    uint y=uint(baseOrigin.y);
    uint height=baseExtent.y;
    [loop] for(uint previous=0;previous<level;++previous) {
        y+=height+2*extrusion;
        height=max(1u,height>>1);
    }
    origin=int2(baseOrigin.x,int(y));
    extent=int2(max(uint2(1,1),baseExtent>>level));
}

float4 SampleStripLevel(float2 local, uint level, int2 baseOrigin, uint2 baseExtent,
    float2 textureSize, uint extrusion, bool linearFilter, bool wrap) {
    int2 origin,extent;
    StripLevel(level,baseOrigin,baseExtent,extrusion,origin,extent);
    // Address the logical region before converting to bounded integer texels.
    // In particular, a negative repeat coordinate wraps into this Image alone.
    float2 addressed=wrap ? frac(local) : saturate(local);
    if(!linearFilter) {
        int2 pixel=min(int2(floor(addressed*float2(extent))),extent-1);
        return source.Load(int3(origin+pixel,0));
    }
    if(!wrap) {
        // Explicit physical LOD 0 and texel-center clamp prevent interpolation
        // into another level or Image, even at the final 1x1 logical mip.
        float2 pixel=clamp(addressed*float2(extent),0.5,float2(extent)-0.5);
        return source.SampleLevel(imageSampler,(float2(origin)+pixel)/textureSize,0);
    }
    // A clamped extrusion cannot represent a repeat seam. Interpolate the four
    // neighbors with per-axis logical wrapping instead of sampling the gutter.
    float2 pixel=addressed*float2(extent)-0.5;
    int2 low=int2(floor(pixel));
    float2 weight=frac(pixel);
    int2 first=(low+extent)%extent;
    int2 second=(first+1)%extent;
    float4 a=source.Load(int3(origin+first,0));
    float4 b=source.Load(int3(origin+int2(second.x,first.y),0));
    float4 c=source.Load(int3(origin+int2(first.x,second.y),0));
    float4 d=source.Load(int3(origin+second,0));
    return lerp(lerp(a,b,weight.x),lerp(c,d,weight.x),weight.y);
}

float4 SampleMipStrip(float2 uv, float2 uvDx, float2 uvDy, float4 region, float4 flags) {
    uint width,height;
    source.GetDimensions(width,height);
    float2 textureSize=float2(width,height);
    int2 baseOrigin=int2(round(region.xy*textureSize));
    uint2 baseExtent=max(uint2(round((region.zw-region.xy)*textureSize)),uint2(1,1));
    float2 local=(uv-region.xy)/(region.zw-region.xy);
    // Derivatives belong to the unaddressed logical Image coordinates. Taking
    // them after frac/clamp would create false LOD changes at a repeat boundary
    // or return to LOD 0 outside a clamp boundary.
    float2 texelDx=uvDx/(region.zw-region.xy)*float2(baseExtent);
    float2 texelDy=uvDy/(region.zw-region.xy)*float2(baseExtent);
    uint last=uint(flags.z)-1;
    float lod=clamp(log2(max(1.0,max(length(texelDx),length(texelDy)))),0.0,float(last));
    bool linearFilter=flags.y>=6.0;
    bool wrap=(uint(flags.y)&1u)!=0;
    uint extrusion=uint(flags.w);
    if(!linearFilter) {
        uint level=uint(floor(lod+0.5));
        return SampleStripLevel(local,level,baseOrigin,baseExtent,textureSize,extrusion,false,wrap);
    }
    uint lower=uint(floor(lod)),upper=min(lower+1,last);
    float4 first=SampleStripLevel(local,lower,baseOrigin,baseExtent,textureSize,extrusion,true,wrap);
    float weight=lod-float(lower);
    if(lower==upper || weight==0.0) return first;
    float4 second=SampleStripLevel(local,upper,baseOrigin,baseExtent,textureSize,extrusion,true,wrap);
    return lerp(first,second,weight);
}

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
    float3 relative : TEXCOORD4) {
    // Atlas Clamp is local to the Image region, not the entire backing texture.
    // Extrusion supplies border pixels; this needs only one texture sample.
    // Homogeneous UV has one shared rational mapping over both triangles.
    // Ordinary Image/Rect uses q=1; 3D clip.w independently handles perspective.
    float2 uv=uvq.xy/uvq.z;
    float2 uvDx=ddx(uv),uvDy=ddy(uv);
    float4 texel;
    [branch] if(flags.y>3.5) {
        texel=SampleMipStrip(uv,uvDx,uvDy,region,flags);
    } else if(flags.y>1.5) {
        texel=SampleLocalRepeat(uv,region,flags.y>2.5);
    } else {
        // Clamp remains one hardware sample. Full-image Repeat also keeps the
        // existing sampler. Preserve pre-clamp derivatives for physical mips.
        float2 sampleUV=flags.y>0.5 ? uv : clamp(uv,region.xy,region.zw);
        texel=source.SampleGrad(imageSampler,sampleUV,uvDx,uvDy);
    }
    float alpha=texel.a*color.a;
    // Canvas is PMA in storage; image files are Straight. Fog operates on
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
