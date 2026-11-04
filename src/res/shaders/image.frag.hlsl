Texture2D<float4> source : register(t0,space2);
SamplerState imageSampler : register(s0,space2);
cbuffer FogState : register(b0,space3) {
    float4 fogRange; // start, end, curve mode, enabled
    float4 fogColor; // straight RGB, independent fog alpha
    float4 fogOperations; // Fog RGB enum, Fog Alpha enum, Image Add-prefix flag, reserved
};

// A logical crop is a fractional rectangle over the copied integer pixel
// coverage. Repeat extends that cropped point field periodically. Linear
// filtering integrates a one-texel box over this field: for integer bounds it
// is exactly ordinary bilinear interpolation, including the repeat seam.
struct AxisFilter { int4 pixel; float4 weight; };
AxisFilter RepeatAxis(float coordinate, float begin, float extent, int coverage) {
    AxisFilter result;
    result.pixel=int4(0,0,0,0); result.weight=float4(1,0,0,0);
    int first=int(floor(begin));
    if(extent<=0.0) { // A positive double may underflow a GPU float; its limit is one point.
        result.pixel=int4(clamp(first,0,coverage-1),0,0,0); return result;
    }
    if(extent<1.0) {
        // A sub-texel period spans at most two source pixels. Integrate complete
        // periods analytically instead of looping 1/extent times.
        float cut=clamp(float(first+1)-begin,0.0,extent);
        float left;
        if(extent<0.00001) left=cut/extent;
        else {
            float full=min(1.0,floor(1.0/extent)*extent);
            float tail=max(0.0,1.0-full);
            float start=frac(coordinate-0.5/extent)*extent;
            float segment=min(tail,extent-start);
            left=full*(cut/extent)+clamp(cut-start,0.0,segment)+min(tail-segment,cut);
        }
        result.pixel=clamp(int4(first,first+1,0,0),0,coverage-1);
        result.weight=float4(saturate(left),1.0-saturate(left),0,0);
        return result;
    }
    // A unit footprint crosses at most one periodic seam, and each remaining
    // segment crosses at most one source pixel boundary: four weights suffice.
    float start=frac(coordinate-0.5/extent)*extent;
    float segment=min(1.0,extent-start);
    float absolute=begin+start;
    int low=int(floor(absolute));
    float firstWeight=clamp(float(low+1)-absolute,0.0,segment);
    float remainder=1.0-segment;
    float thirdWeight=clamp(float(first+1)-begin,0.0,remainder);
    result.pixel=clamp(int4(low,low+1,first,first+1),0,coverage-1);
    result.weight=float4(firstWeight,segment-firstWeight,thirdWeight,remainder-thirdWeight);
    return result;
}
float4 SampleRegion(float2 local, int2 origin, int2 coverage, float4 logical,
    uint physicalLevel, float2 textureSize, bool linearFilter, bool wrap) {
    float2 addressed=wrap ? frac(local) : saturate(local);
    float2 pixel=logical.xy+addressed*logical.zw;
    if(!linearFilter) {
        int2 texelIndex=clamp(int2(floor(pixel)),int2(0,0),coverage-1);
        return source.Load(int3(origin+texelIndex,physicalLevel));
    }
    if(!wrap) {
        pixel=clamp(pixel,0.5,float2(coverage)-0.5);
        return source.SampleLevel(imageSampler,(float2(origin)+pixel)/textureSize,float(physicalLevel));
    }
    if(all(logical.xy==floor(logical.xy))&&all(logical.zw==floor(logical.zw))&&all(logical.zw>=1)) {
        // Keep the ordinary integer-region path at four texture loads.
        int2 extent=int2(logical.zw),base=origin+int2(logical.xy);
        float2 position=addressed*logical.zw-0.5;
        int2 low=int2(floor(position)); float2 weight=frac(position);
        int2 first=(low+extent)%extent,second=(first+1)%extent;
        return lerp(lerp(source.Load(int3(base+first,physicalLevel)),
                         source.Load(int3(base+int2(second.x,first.y),physicalLevel)),weight.x),
                    lerp(source.Load(int3(base+int2(first.x,second.y),physicalLevel)),
                         source.Load(int3(base+second,physicalLevel)),weight.x),weight.y);
    }
    AxisFilter x=RepeatAxis(addressed.x,logical.x,logical.z,coverage.x);
    AxisFilter y=RepeatAxis(addressed.y,logical.y,logical.w,coverage.y);
    float4 color=0;
    [unroll] for(uint row=0;row<4;++row) {
        [unroll] for(uint column=0;column<4;++column) {
            float weight=x.weight[column]*y.weight[row];
            if(weight>0.0) color+=source.Load(int3(origin+int2(x.pixel[column],y.pixel[row]),physicalLevel))*weight;
        }
    }
    return color;
}
void StripLevel(uint level, int2 baseOrigin, uint2 baseExtent, uint extrusion,
    out int2 origin, out int2 extent) {
    uint y=uint(baseOrigin.y),height=baseExtent.y;
    [loop] for(uint previous=0;previous<level;++previous) {
        y+=height+2*extrusion; height=max(1u,height>>1);
    }
    origin=int2(baseOrigin.x,int(y)); extent=int2(max(uint2(1,1),baseExtent>>level));
}
float4 SampleImageLevel(float2 local,uint level,int2 baseOrigin,uint2 baseExtent,float4 logical,
    float2 textureSize,uint extrusion,bool linearFilter,bool wrap,bool physicalMips) {
    int2 origin,extent; uint physicalLevel=0;
    if(physicalMips) {
        physicalLevel=level;
        uint width,height,levels; source.GetDimensions(level,width,height,levels);
        textureSize=float2(width,height);
        origin=baseOrigin>>level; extent=int2(max(uint2(1,1),baseExtent>>level));
    } else StripLevel(level,baseOrigin,baseExtent,extrusion,origin,extent);
    // SDL's physical mips and CPU strips both resample each full coverage level.
    // Map the fractional rectangle by the same per-axis ratio, including odd sizes.
    float2 ratio=float2(extent)/float2(baseExtent);
    return SampleRegion(local,origin,extent,float4(logical.xy*ratio,logical.zw*ratio),
        physicalLevel,textureSize,linearFilter,wrap);
}
float4 SampleImageMips(float2 local,float2 localDx,float2 localDy,float4 region,float4 logical,float4 flags) {
    uint width,height; source.GetDimensions(width,height);
    float2 textureSize=float2(width,height);
    int2 origin=int2(round(region.xy*textureSize));
    uint2 extent=max(uint2(round((region.zw-region.xy)*textureSize)),uint2(1,1));
    // Derivatives precede frac/clamp and use the exact logical source extent.
    float2 dx=localDx*logical.zw,dy=localDy*logical.zw;
    uint last=uint(flags.z)-1,mode=uint(flags.y);
    float lod=clamp(log2(max(1.0,max(length(dx),length(dy)))),0.0,float(last));
    bool linearFilter=(mode&2u)!=0,wrap=(mode&1u)!=0,physicalMips=mode>=8;
    uint extrusion=uint(flags.w);
    if(!linearFilter) return SampleImageLevel(local,uint(floor(lod+0.5)),origin,extent,logical,
        textureSize,extrusion,false,wrap,physicalMips);
    uint lower=uint(floor(lod)),upper=min(lower+1,last);
    float4 first=SampleImageLevel(local,lower,origin,extent,logical,textureSize,extrusion,true,wrap,physicalMips);
    float weight=lod-float(lower);
    if(lower==upper||weight==0) return first;
    return lerp(first,SampleImageLevel(local,upper,origin,extent,logical,textureSize,extrusion,true,wrap,physicalMips),weight);
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
        texel=SampleImageMips(local,localDx,localDy,region,logical,flags);
    } else if(flags.y>1.5) {
        texel=SampleRegion(local,origin,coverage,logical,0,textureSize,flags.y>2.5,true);
    } else {
        float2 pixel=logical.xy+(flags.y>0.5 ? local : saturate(local))*logical.zw;
        if(flags.y<0.5) pixel=clamp(pixel,0.5,float2(coverage)-0.5);
        float2 uv=(float2(origin)+pixel)/textureSize;
        texel=source.SampleGrad(imageSampler,uv,localDx*logical.zw/textureSize,localDy*logical.zw/textureSize);
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
