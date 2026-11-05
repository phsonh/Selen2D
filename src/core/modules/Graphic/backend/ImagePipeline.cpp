#include "ImagePipeline.h"
#include "ImageShaders.h"
#include "SpriteShaders.h"
#include <SDL3/SDL.h>
#include <array>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace core::modules::Graphic::backend {
SDL_GPUColorTargetBlendState MakeBlendState(Render::Blend blend) {
    using B = Render::Blend;
    if (!Render::IsValidBlend(blend)) throw std::invalid_argument("Invalid Image pipeline Blend");
    SDL_GPUColorTargetBlendState state{};
    state.enable_blend = true;
    state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    switch (blend) {
    case B::MulAlpha: case B::AddAlpha: break;
    case B::MulAdd: case B::AddAdd:
        state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE; break;
    case B::MulSub: case B::AddSub:
        state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        state.color_blend_op = SDL_GPU_BLENDOP_SUBTRACT; break;
    case B::MulRev: case B::AddRev:
        state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        state.color_blend_op = SDL_GPU_BLENDOP_REVERSE_SUBTRACT; break;
    case B::MulMin: case B::AddMin:
        state.dst_color_blendfactor = state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        state.color_blend_op = state.alpha_blend_op = SDL_GPU_BLENDOP_MIN; break;
    case B::MulMax: case B::AddMax:
        state.dst_color_blendfactor = state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        state.color_blend_op = state.alpha_blend_op = SDL_GPU_BLENDOP_MAX; break;
    case B::MulMul: case B::AddMul:
        state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_DST_COLOR;
        state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ZERO; break;
    case B::MulScreen: case B::AddScreen:
        state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR; break;
    case B::AlphaBalance:
        state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR;
        state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
        state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
        state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE; break;
    case B::Replace:
        state.dst_color_blendfactor = state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO; break;
    }
    return state;
}
struct ImagePipeline::Impl {
    SDL_GPUDevice* device;
    SDL_GPUShader* vertex{};
    SDL_GPUShader* spriteVertex{};
    SDL_GPUShader* fragment{};
    struct Entry { SDL_GPUTextureFormat format; Render::Blend blend; SDL_GPUTextureFormat depth; bool depthEnabled; bool sprite2D; SDL_GPUGraphicsPipeline* pipeline; };
    std::vector<Entry> pipelines;
    std::array<SDL_GPUSampler*,4> samplers{};
    explicit Impl(SDL_GPUDevice* d):device(d) {}
    ~Impl() {
        for(auto entry:pipelines) SDL_ReleaseGPUGraphicsPipeline(device,entry.pipeline);
        for(auto* sampler:samplers) if(sampler) SDL_ReleaseGPUSampler(device,sampler);
        if(vertex) SDL_ReleaseGPUShader(device,vertex);
        if(spriteVertex) SDL_ReleaseGPUShader(device,spriteVertex);
        if(fragment) SDL_ReleaseGPUShader(device,fragment);
    }
    void Init() {
        using namespace image_shader_bytecode;
        if (!(SDL_GetGPUShaderFormats(device)&format))
            throw std::runtime_error("Built-in Image bytecode is unavailable for the active SDL_GPU shader format");
        SDL_GPUShaderCreateInfo create{};
        create.entrypoint="main"; create.format=format; create.stage=SDL_GPU_SHADERSTAGE_VERTEX;
        create.code=image_shader_bytecode::vertex; create.code_size=sizeof(image_shader_bytecode::vertex);
        vertex=SDL_CreateGPUShader(device,&create);
        if(!vertex) throw std::runtime_error(std::string("Image vertex shader failed: ")+SDL_GetError());
        create.num_storage_buffers=1;
        create.code=sprite_shader_bytecode::vertex;create.code_size=sizeof(sprite_shader_bytecode::vertex);
        spriteVertex=SDL_CreateGPUShader(device,&create);
        if(!spriteVertex) throw std::runtime_error(std::string("Sprite vertex shader failed: ")+SDL_GetError());
        create.num_storage_buffers=0;
        create.stage=SDL_GPU_SHADERSTAGE_FRAGMENT; create.num_samplers=1; create.num_uniform_buffers=1;
        create.code=image_shader_bytecode::fragment; create.code_size=sizeof(image_shader_bytecode::fragment);
        fragment=SDL_CreateGPUShader(device,&create);
        if(!fragment) throw std::runtime_error(std::string("Image fragment shader failed: ")+SDL_GetError());
        pipelines.reserve(4);
    }
};
ImagePipeline::ImagePipeline(SDL_GPUDevice* d):impl(std::make_unique<Impl>(d)) { impl->Init(); }
ImagePipeline::~ImagePipeline()=default;
SDL_GPUGraphicsPipeline* ImagePipeline::Get(SDL_GPUTextureFormat format, Render::Blend blend, SDL_GPUTextureFormat depth, bool depthEnabled, bool sprite2D) {
    if(sprite2D && depth!=SDL_GPU_TEXTUREFORMAT_INVALID) throw std::invalid_argument("Sprite vertex layout requires a 2D target");
    for(auto entry:impl->pipelines) if(entry.format==format && entry.blend==blend && entry.depth==depth && entry.depthEnabled==depthEnabled && entry.sprite2D==sprite2D) return entry.pipeline;
    // Bounded cache includes all 18 blends across 2D/depth-enabled/disabled
    // Canvas targets and swapchain formats. Do not precreate unused variants.
    if(impl->pipelines.size()>=256) throw std::length_error("Image pipeline variant limit exceeded");
    // Reserve first so a successful GPU allocation cannot be lost to vector growth.
    impl->pipelines.reserve(impl->pipelines.size()+1);
    using V=Render::detail::Vertex;
    static_assert(std::is_standard_layout_v<V> && std::is_trivially_copyable_v<V>);
    static_assert(sizeof(V)==92 && alignof(V)==4);
    static_assert(offsetof(V,x)==0 && offsetof(V,y)==4 && offsetof(V,z)==8 && offsetof(V,w)==12);
    static_assert(offsetof(V,u)==16 && offsetof(V,v)==20 && offsetof(V,q)==24);
    static_assert(offsetof(V,r)==28 && offsetof(V,g)==29 && offsetof(V,b)==30 && offsetof(V,a)==31);
    static_assert(offsetof(V,minU)==32 && offsetof(V,minV)==36 && offsetof(V,maxU)==40 && offsetof(V,maxV)==44);
    static_assert(offsetof(V,premultiplied)==48 && offsetof(V,sampling)==52 && offsetof(V,mipLevels)==56 && offsetof(V,mipExtrusion)==60);
    static_assert(offsetof(V,relativeX)==64 && offsetof(V,relativeY)==68 && offsetof(V,relativeZ)==72);
    static_assert(offsetof(V,logicalX)==76 && offsetof(V,logicalY)==80 && offsetof(V,logicalWidth)==84 && offsetof(V,logicalHeight)==88);
    using S=Render::detail::SpriteVertex;
    using P=Render::detail::SpriteParameters;
    static_assert(std::is_standard_layout_v<S> && std::is_trivially_copyable_v<S> && sizeof(S)==28 && alignof(S)==4);
    static_assert(offsetof(S,x)==0 && offsetof(S,y)==4 && offsetof(S,u)==8 && offsetof(S,v)==12 && offsetof(S,q)==16);
    static_assert(offsetof(S,r)==20 && offsetof(S,g)==21 && offsetof(S,b)==22 && offsetof(S,a)==23 && offsetof(S,parameterIndex)==24);
    static_assert(std::is_standard_layout_v<P> && std::is_trivially_copyable_v<P> && sizeof(P)==48 && alignof(P)==16);
    static_assert(offsetof(P,region)==0 && offsetof(P,flags)==16 && offsetof(P,logical)==32);
    SDL_GPUVertexBufferDescription buffer{0,static_cast<Uint32>(sprite2D ? sizeof(S):sizeof(V)),SDL_GPU_VERTEXINPUTRATE_VERTEX,0};
    const std::array<SDL_GPUVertexAttribute,7> attributes{{
        {0,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4,offsetof(V,x)},
        {1,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,offsetof(V,u)},
        {2,0,SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM,offsetof(V,r)},
        {3,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4,offsetof(V,minU)},
        {4,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4,offsetof(V,premultiplied)},
        {5,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,offsetof(V,relativeX)},
        {6,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4,offsetof(V,logicalX)}}};
    const std::array<SDL_GPUVertexAttribute,4> spriteAttributes{{
        {0,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,offsetof(S,x)},
        {1,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,offsetof(S,u)},
        {2,0,SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM,offsetof(S,r)},
        {3,0,SDL_GPU_VERTEXELEMENTFORMAT_UINT,offsetof(S,parameterIndex)}}};
    const bool threeD=depth!=SDL_GPU_TEXTUREFORMAT_INVALID;
    std::array<SDL_GPUColorTargetDescription,2> targets{};
    targets[0].format=format;
    targets[0].blend_state=MakeBlendState(blend);
    // Distance describes the same depth-winning geometry, independently of
    // its color/Alpha blend. A disabled Z buffer must preserve both attachments.
    targets[1].format=SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
    targets[1].blend_state.enable_blend=false;
    targets[1].blend_state.enable_color_write_mask=true;
    targets[1].blend_state.color_write_mask=depthEnabled ? SDL_GPU_COLORCOMPONENT_R : 0;
    SDL_GPUGraphicsPipelineCreateInfo create{};
    create.vertex_shader=sprite2D ? impl->spriteVertex:impl->vertex; create.fragment_shader=impl->fragment;
    create.vertex_input_state={&buffer,1,sprite2D ? spriteAttributes.data():attributes.data(),
        static_cast<Uint32>(sprite2D ? spriteAttributes.size():attributes.size())};
    create.primitive_type=SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    create.rasterizer_state.fill_mode=SDL_GPU_FILLMODE_FILL;
    create.rasterizer_state.cull_mode=SDL_GPU_CULLMODE_NONE;
    create.rasterizer_state.front_face=SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    create.rasterizer_state.enable_depth_clip=true; // Near/far clipping is independent of depth test/write.
    create.multisample_state.sample_count=SDL_GPU_SAMPLECOUNT_1;
    create.target_info.color_target_descriptions=targets.data(); create.target_info.num_color_targets=threeD ? 2 : 1;
    create.target_info.has_depth_stencil_target=threeD;
    create.target_info.depth_stencil_format=depth;
    create.depth_stencil_state.enable_depth_test=depthEnabled;
    create.depth_stencil_state.enable_depth_write=depthEnabled;
    create.depth_stencil_state.compare_op=SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    auto* pipeline=SDL_CreateGPUGraphicsPipeline(impl->device,&create);
    if(!pipeline) throw std::runtime_error(std::string("Image pipeline failed: ")+SDL_GetError());
    impl->pipelines.push_back({format,blend,depth,depthEnabled,sprite2D,pipeline});
    return pipeline;
}
SDL_GPUSampler* ImagePipeline::Sampler(Render::Filter filter, Render::Address address) {
    const auto index=(filter==Render::Filter::Linear ? 2:0)+(address==Render::Address::Repeat ? 1:0);
    auto*& sampler=impl->samplers[index];
    if(sampler) return sampler;
    SDL_GPUSamplerCreateInfo create{};
    create.min_filter=create.mag_filter=filter==Render::Filter::Linear ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
    create.mipmap_mode=filter==Render::Filter::Linear ? SDL_GPU_SAMPLERMIPMAPMODE_LINEAR : SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    create.address_mode_u=create.address_mode_v=create.address_mode_w=
        address==Render::Address::Repeat ? SDL_GPU_SAMPLERADDRESSMODE_REPEAT : SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    create.max_lod=1000;
    sampler=SDL_CreateGPUSampler(impl->device,&create);
    if(!sampler) throw std::runtime_error(std::string("Image sampler failed: ")+SDL_GetError());
    return sampler;
}
}
