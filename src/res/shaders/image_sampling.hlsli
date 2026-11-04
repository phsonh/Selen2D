// A logical crop is a fractional rectangle over the copied integer pixel
// coverage. Repeat extends that cropped point field periodically. Linear
// filtering integrates a one-texel box over this field: for integer bounds it
// is exactly ordinary bilinear interpolation, including the repeat seam.
struct EngineAxisFilter { int4 pixel; float4 weight; };
EngineAxisFilter EngineRepeatAxis(float coordinate, float begin, float extent, int coverage) {
    EngineAxisFilter result;
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
float4 EngineSampleRegion(Texture2D<float4> EngineSource, SamplerState EngineSampler, float2 local, int2 origin, int2 coverage, float4 logical,
    uint physicalLevel, float2 textureSize, bool linearFilter, bool wrap) {
    float2 addressed=wrap ? frac(local) : saturate(local);
    float2 pixel=logical.xy+addressed*logical.zw;
    if(!linearFilter) {
        int2 texelIndex=clamp(int2(floor(pixel)),int2(0,0),coverage-1);
        return EngineSource.Load(int3(origin+texelIndex,physicalLevel));
    }
    if(!wrap) {
        pixel=clamp(pixel,0.5,float2(coverage)-0.5);
        return EngineSource.SampleLevel(EngineSampler,(float2(origin)+pixel)/textureSize,float(physicalLevel));
    }
    if(all(logical.xy==floor(logical.xy))&&all(logical.zw==floor(logical.zw))&&all(logical.zw>=1)) {
        // Keep the ordinary integer-region path at four texture loads.
        int2 extent=int2(logical.zw),base=origin+int2(logical.xy);
        float2 position=addressed*logical.zw-0.5;
        int2 low=int2(floor(position)); float2 weight=frac(position);
        int2 first=(low+extent)%extent,second=(first+1)%extent;
        return lerp(lerp(EngineSource.Load(int3(base+first,physicalLevel)),
                         EngineSource.Load(int3(base+int2(second.x,first.y),physicalLevel)),weight.x),
                    lerp(EngineSource.Load(int3(base+int2(first.x,second.y),physicalLevel)),
                         EngineSource.Load(int3(base+second,physicalLevel)),weight.x),weight.y);
    }
    EngineAxisFilter x=EngineRepeatAxis(addressed.x,logical.x,logical.z,coverage.x);
    EngineAxisFilter y=EngineRepeatAxis(addressed.y,logical.y,logical.w,coverage.y);
    float4 color=0;
    [unroll] for(uint row=0;row<4;++row) {
        [unroll] for(uint column=0;column<4;++column) {
            float weight=x.weight[column]*y.weight[row];
            if(weight>0.0) color+=EngineSource.Load(int3(origin+int2(x.pixel[column],y.pixel[row]),physicalLevel))*weight;
        }
    }
    return color;
}
void EngineStripLevel(uint level, int2 baseOrigin, uint2 baseExtent, uint extrusion,
    out int2 origin, out int2 extent) {
    uint y=uint(baseOrigin.y),height=baseExtent.y;
    [loop] for(uint previous=0;previous<level;++previous) {
        y+=height+2*extrusion; height=max(1u,height>>1);
    }
    origin=int2(baseOrigin.x,int(y)); extent=int2(max(uint2(1,1),baseExtent>>level));
}
float4 EngineSampleImageLevel(Texture2D<float4> EngineSource, SamplerState EngineSampler, float2 local,uint level,int2 baseOrigin,uint2 baseExtent,float4 logical,
    float2 textureSize,uint extrusion,bool linearFilter,bool wrap,bool physicalMips) {
    int2 origin,extent; uint physicalLevel=0;
    if(physicalMips) {
        physicalLevel=level;
        uint width,height,levels; EngineSource.GetDimensions(level,width,height,levels);
        textureSize=float2(width,height);
        origin=baseOrigin>>level; extent=int2(max(uint2(1,1),baseExtent>>level));
    } else EngineStripLevel(level,baseOrigin,baseExtent,extrusion,origin,extent);
    // SDL's physical mips and CPU strips both resample each full coverage level.
    // Map the fractional rectangle by the same per-axis ratio, including odd sizes.
    float2 ratio=float2(extent)/float2(baseExtent);
    return EngineSampleRegion(EngineSource, EngineSampler, local,origin,extent,float4(logical.xy*ratio,logical.zw*ratio),
        physicalLevel,textureSize,linearFilter,wrap);
}
float4 EngineSampleImageLod(Texture2D<float4> EngineSource, SamplerState EngineSampler, float2 local,float requestedLod,float4 region,float4 logical,float4 flags) {
    uint width,height; EngineSource.GetDimensions(width,height);
    float2 textureSize=float2(width,height);
    int2 origin=int2(round(region.xy*textureSize));
    uint2 extent=max(uint2(round((region.zw-region.xy)*textureSize)),uint2(1,1));
    uint last=uint(flags.z)-1,mode=uint(flags.y);
    float lod=clamp(requestedLod,0.0,float(last));
    bool linearFilter=(mode&2u)!=0,wrap=(mode&1u)!=0,physicalMips=mode>=8;
    uint extrusion=uint(flags.w);
    if(!linearFilter) return EngineSampleImageLevel(EngineSource, EngineSampler, local,uint(floor(lod+0.5)),origin,extent,logical,
        textureSize,extrusion,false,wrap,physicalMips);
    uint lower=uint(floor(lod)),upper=min(lower+1,last);
    float4 first=EngineSampleImageLevel(EngineSource, EngineSampler, local,lower,origin,extent,logical,textureSize,extrusion,true,wrap,physicalMips);
    float weight=lod-float(lower);
    if(lower==upper||weight==0) return first;
    return lerp(first,EngineSampleImageLevel(EngineSource, EngineSampler, local,upper,origin,extent,logical,textureSize,extrusion,true,wrap,physicalMips),weight);
}

float4 EngineSampleImageGrad(Texture2D<float4> EngineSource, SamplerState EngineSampler,
    float2 local,float2 localDx,float2 localDy,float4 region,float4 logical,float4 flags) {
    // Derivatives precede frac/clamp and use the exact logical source extent.
    float2 dx=localDx*logical.zw,dy=localDy*logical.zw;
    float lod=log2(max(1.0,max(length(dx),length(dy))));
    return EngineSampleImageLod(EngineSource,EngineSampler,local,lod,region,logical,flags);
}
float4 EngineUnassociate(float4 value) {
    return float4(value.a>0 ? value.rgb/value.a : float3(0,0,0),value.a);
}
