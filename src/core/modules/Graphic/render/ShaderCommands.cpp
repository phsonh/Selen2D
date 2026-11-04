#include "ShaderCommands.h"
#include "core/modules/Graphic/shader/Builtins.h"
#include "core/modules/Graphic/shader/Sampling.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace core::modules::Graphic::Render::detail {
namespace {
float Float(double v) {
    const auto result=static_cast<float>(v);
    if(!std::isfinite(result)) throw std::invalid_argument("Shader draw exceeds GPU float range");
    return result;
}
std::vector<ShaderUniform> Uniforms(const Shader::Stage& stage,const ShaderStageData& user,
    const Canvas::Info& info,const Geometry::SpatialState& spatial,bool fullscreen,const ShaderCommand& command) {
    auto builtins=Shader::Builtins::Pack(info,spatial,fullscreen,stage);
    std::vector<ShaderUniform> result;
    result.reserve(stage.buffers.size());
    for(auto& value:builtins) result.push_back({value.slot,std::move(value.bytes)});
    std::size_t used{};
    for(const auto& buffer:stage.buffers) {
        if(buffer.name=="EngineBuiltins") continue;
        if(buffer.name=="Params") {
            const auto match=std::find_if(user.params.begin(),user.params.end(),[&](const auto& p){return p.slot==buffer.slot;});
            if(match==user.params.end() || match->bytes.size()!=buffer.size)
                throw std::invalid_argument("Shader Params snapshot does not match reflection");
            result.push_back(*match); ++used; continue;
        }
        if(buffer.name==Shader::Sampling::BufferName) {
            if(!buffer.size || buffer.size>65536) throw std::invalid_argument("Invalid Shader sampling metadata size");
            ShaderUniform value{buffer.slot,std::vector<std::uint8_t>(buffer.size)};
            for(const auto& field:buffer.fields) {
                const bool region=field.name.starts_with("EngineRegion_");
                const bool infoField=field.name.starts_with("EngineInfo_");
                if((!region && !infoField) || field.type!=Shader::ValueType::Float4 || field.size!=16 ||
                    field.offset>value.bytes.size() || field.size>value.bytes.size()-field.offset)
                    throw std::invalid_argument("Invalid reflected Shader sampling metadata field");
                if(!field.used) continue;
                const auto name=field.name.substr(region?13:11);
                const auto texture=std::find_if(stage.textures.begin(),stage.textures.end(),[&](const auto& t){return t.name==name && !t.engine;});
                if(texture==stage.textures.end()) throw std::invalid_argument("Shader sampling metadata has no reflected texture");
                const auto input=std::find_if(user.textures.begin(),user.textures.end(),[&](const auto& t){return t.slot==texture->slot;});
                if(input==user.textures.end() || input->part!=TexturePart::Color || input->source>=command.draw.sources.size())
                    throw std::invalid_argument("Shader sampling metadata has no color input");
                std::array<float,4> data{};
                if(const auto* image=std::get_if<Image>(&command.draw.sources[input->source])) {
                    if(region) data={Float(image->insetX),Float(image->insetY),Float(image->LogicalWidth()),Float(image->LogicalHeight())};
                    else data={Float(image->width),Float(image->height),Float(image->mipLevels?image->mipLevels:image->textureLevels),
                        float((image->filter==Filter::Linear?2:0)+(image->address==Address::Repeat?1:0))};
                } else if(const auto canvas=command.canvases[input->source]) {
                    if(region) data={0,0,Float(canvas->info.width),Float(canvas->info.height)};
                    else data={Float(canvas->info.width),Float(canvas->info.height),1,2};
                } else throw std::invalid_argument("Shader sampling metadata requires a live color input");
                std::memcpy(value.bytes.data()+field.offset,data.data(),sizeof(data));
            }
            result.push_back(std::move(value));continue;
        }
        if(buffer.name!="EngineDrawState" || buffer.size>65536 || !buffer.size)
            throw std::invalid_argument("Unknown private Shader uniform buffer");
        ShaderUniform value{buffer.slot,std::vector<std::uint8_t>(buffer.size)};
        std::array<float,4> range{},color{},ops{};
        if(info.threeD && spatial.fog) {
            const auto& fog=*spatial.fog;
            range={Float(fog.start),Float(fog.end),float(fog.mode),1};
            if(range[0]>=range[1] || !std::isfinite(range[1]-range[0]))
                throw std::invalid_argument("Shader Fog interval exceeds GPU precision");
            color={float((fog.color>>16)&255)/255,float((fog.color>>8)&255)/255,
                float(fog.color&255)/255,float(fog.color>>24)/255};
            ops={float(fog.rgb),float(fog.alpha),0,0};
        }
        const std::array<float,4> camera{Float(spatial.camera.position[0]),Float(spatial.camera.position[1]),Float(spatial.camera.position[2]),0};
        const std::array<float,4> targetKind{info.threeD?1.f:0.f,0,0,0};
        for(const auto& field:buffer.fields) {
            const std::array<float,4>* data{};
            if(field.name=="EngineFogRange") data=&range;
            else if(field.name=="EngineFogColor") data=&color;
            else if(field.name=="EngineFogOps") data=&ops;
            else if(field.name=="EngineCameraPosition") data=&camera;
            else if(field.name=="EngineTargetKind") data=&targetKind;
            if(!data || field.type!=Shader::ValueType::Float4 || field.size!=sizeof(*data) ||
                field.offset>value.bytes.size() || field.size>value.bytes.size()-field.offset)
                throw std::invalid_argument("Invalid private Shader field reflection");
            if(field.used) std::memcpy(value.bytes.data()+field.offset,data->data(),sizeof(*data));
        }
        result.push_back(std::move(value));
    }
    if(used!=user.params.size()) throw std::invalid_argument("Unexpected/duplicate Shader Params buffer");
    return result;
}
}
ShaderCommands::ShaderCommands(SDL_GPUDevice* device):device_(device),images_(device),canvases_(device),
    meshes_(device),pipelines_(device),samplers_(device) {
    const std::array<Geometry::MeshInputVertex,3> vertices{{{-1,-1,0,0,1},{-1,3,0,0,-1},{3,-1,0,2,1}}};
    const std::array<std::uint32_t,3> indices{0,1,2};
    triangle_=Geometry::MeshData::Create(vertices,indices);
}
ShaderCommands::~ShaderCommands(){Shutdown();}
void ShaderCommands::Prepare(ShaderCommand& command,const Geometry::SpatialState& spatial,
    const std::shared_ptr<Canvas::detail::Attachment>& target,SDL_GPUTextureFormat format) {
    const auto& draw=command.draw;
    const auto& metadata=draw.program->Metadata();
    const bool fullscreen=!draw.mesh;
    command.vertexUniforms=Uniforms(metadata.vertex,draw.vertex,command.info,spatial,fullscreen,command);
    command.fragmentUniforms=Uniforms(metadata.fragment,draw.fragment,command.info,spatial,fullscreen,command);
    command.pipeline=pipelines_.Get(draw.program,format,target->depthFormat,spatial.depthEnabled,fullscreen);
    // Empty geometry keeps full validation and geometry pipeline semantics;
    // Renderer records only its target marker after Prepare, with no GPU draw.
    if(!draw.mesh || !draw.mesh->Vertices().empty()) command.mesh=meshes_.Acquire(draw.mesh ? draw.mesh:triangle_);
    else command.mesh.reset();
    command.images.resize(draw.sources.size()); command.colors.resize(draw.sources.size());
    auto prepareStage=[&](const Shader::Stage& stage,const ShaderStageData& values,auto& bindings){
        bindings.resize(stage.textures.size());
        std::array<bool,16> slots{};
        std::size_t used{};
        for(const auto& texture:stage.textures) {
            if(texture.slot>=bindings.size() || texture.slot>=slots.size() || slots[texture.slot])
                throw std::invalid_argument("Invalid Shader texture slot reflection");
            slots[texture.slot]=true;
            if(texture.engine) {
                if(texture.name!="EngineFogDistance") throw std::invalid_argument("Unknown engine Shader texture");
                if(command.info.threeD && fullscreen && spatial.fog)
                    command.history=canvases_.Acquire(target,Shader::CanvasViews::Kind::Distance);
                if(!emptyDistance_) {
                    SDL_GPUTextureCreateInfo create{};
                    create.type=SDL_GPU_TEXTURETYPE_2D; create.format=SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
                    create.usage=SDL_GPU_TEXTUREUSAGE_SAMPLER|SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
                    create.width=create.height=create.layer_count_or_depth=create.num_levels=1;
                    emptyDistance_=SDL_CreateGPUTexture(device_,&create);
                    if(!emptyDistance_) throw std::runtime_error(SDL_GetError());
                }
                bindings[texture.slot]={emptyDistance_,samplers_.Sampler(Filter::Point,Address::Clamp)};
                continue;
            }
            auto input=std::find_if(values.textures.begin(),values.textures.end(),[&](const auto& p){return p.slot==texture.slot;});
            if(input==values.textures.end() || input->source>=draw.sources.size())
                throw std::invalid_argument("Shader texture input does not match reflection");
            if(input->part!=TexturePart::Color && input->part!=TexturePart::Depth && input->part!=TexturePart::Distance)
                throw std::invalid_argument("Invalid Shader texture part");
            ++used; const auto index=input->source;
            if(auto image=std::get_if<Image>(&draw.sources[index])) {
                if(input->part!=TexturePart::Color) throw std::invalid_argument("Image has no depth/distance input");
                if(!command.images[index]) command.images[index]=images_.Acquire(*image);
                bindings[texture.slot].sampler=samplers_.Sampler(image->filter,image->address);
            } else if(auto canvas=command.canvases[index]) {
                if(input->part==TexturePart::Color) {
                    if(!command.colors[index]) command.colors[index]=canvases_.Acquire(canvas,Shader::CanvasViews::Kind::Color);
                    bindings[texture.slot].sampler=samplers_.Sampler(Filter::Linear,Address::Clamp);
                } else {
                    if(!canvas->info.threeD) throw std::invalid_argument("Depth/distance input requires Canvas3D");
                    bindings[texture.slot]={input->part==TexturePart::Depth ? canvas->depth:canvas->distance,
                        samplers_.Sampler(Filter::Point,Address::Clamp)};
                }
            } else throw std::invalid_argument("Missing Shader texture source");
        }
        if(used!=values.textures.size()) throw std::invalid_argument("Unexpected/duplicate Shader texture input");
    };
    prepareStage(metadata.vertex,draw.vertex,command.vertexTextures);
    prepareStage(metadata.fragment,draw.fragment,command.fragmentTextures);
}
void ShaderCommands::EncodeInputs(SDL_GPUCommandBuffer* cb,ShaderCommand& command,
    const std::function<std::uint64_t(Canvas::detail::Attachment*,bool)>& serial) {
    const std::array mesh{command.mesh}; meshes_.EncodeCopy(cb,mesh);
    for(const auto& image:command.images) if(image) {const std::array input{image}; images_.EncodeCopy(cb,input);}
    for(std::size_t i=0;i<command.colors.size();++i) if(command.colors[i])
        canvases_.Encode(cb,command.colors[i],serial(command.canvases[i].get(),false));
    if(command.history) {
        // The target pin is represented by the snapshot's source inside the View;
        // its serial is supplied separately through the null marker by Renderer.
        canvases_.Encode(cb,command.history,serial(nullptr,true));
    }
    if(emptyDistance_ && !emptyReady_ && !emptyEncoded_) {
        SDL_GPUColorTargetInfo clear{}; clear.texture=emptyDistance_; clear.load_op=SDL_GPU_LOADOP_CLEAR;
        clear.store_op=SDL_GPU_STOREOP_STORE;
        auto* pass=SDL_BeginGPURenderPass(cb,&clear,1,nullptr);
        if(!pass) throw std::runtime_error(SDL_GetError());
        SDL_EndGPURenderPass(pass);
        emptyEncoded_=true;
    }
    auto resolve=[&](const Shader::Stage& stage,const ShaderStageData& values,auto& bindings){
        for(const auto& input:values.textures) {
            if(command.images[input.source]) bindings[input.slot].texture=command.images[input.source]->Native(cb);
            else if(input.part==TexturePart::Color) bindings[input.slot].texture=command.colors[input.source]->Native(cb);
        }
        for(const auto& input:stage.textures) if(input.engine && command.history)
            bindings[input.slot].texture=command.history->Native(cb);
    };
    const auto& metadata=command.draw.program->Metadata();
    resolve(metadata.vertex,command.draw.vertex,command.vertexTextures);
    resolve(metadata.fragment,command.draw.fragment,command.fragmentTextures);
}
void ShaderCommands::Draw(SDL_GPUCommandBuffer* cb,SDL_GPURenderPass* pass,const ShaderCommand& command) {
    SDL_BindGPUGraphicsPipeline(pass,command.pipeline->Native());
    SDL_GPUBufferBinding vertex{command.mesh->Vertex(cb),0},index{command.mesh->Index(cb),0};
    SDL_BindGPUVertexBuffers(pass,0,&vertex,1);
    SDL_BindGPUIndexBuffer(pass,&index,SDL_GPU_INDEXELEMENTSIZE_32BIT);
    for(const auto& uniform:command.vertexUniforms)
        SDL_PushGPUVertexUniformData(cb,uniform.slot,uniform.bytes.data(),static_cast<Uint32>(uniform.bytes.size()));
    for(const auto& uniform:command.fragmentUniforms)
        SDL_PushGPUFragmentUniformData(cb,uniform.slot,uniform.bytes.data(),static_cast<Uint32>(uniform.bytes.size()));
    if(!command.vertexTextures.empty()) SDL_BindGPUVertexSamplers(pass,0,command.vertexTextures.data(),static_cast<Uint32>(command.vertexTextures.size()));
    if(!command.fragmentTextures.empty()) SDL_BindGPUFragmentSamplers(pass,0,command.fragmentTextures.data(),static_cast<Uint32>(command.fragmentTextures.size()));
    SDL_DrawGPUIndexedPrimitives(pass,command.mesh->IndexCount(),1,0,0,0);
}
void ShaderCommands::Commit(SDL_GPUCommandBuffer* cb) {
    // Every lease also lives in its prepared draw, retained by Renderer to Fence.
    (void)meshes_.CommitEncoded(cb); (void)images_.CommitEncoded(cb); (void)canvases_.CommitEncoded(cb);
    if(emptyEncoded_) {emptyReady_=true; emptyEncoded_=false;}
}
void ShaderCommands::Reclaim(){images_.Reclaim();canvases_.Reclaim();meshes_.Reclaim();pipelines_.Reclaim();}
void ShaderCommands::Shutdown(){
    if(closed_) return; closed_=true;
    images_.Shutdown();canvases_.Shutdown();meshes_.Shutdown();pipelines_.Shutdown();
    if(emptyDistance_) SDL_ReleaseGPUTexture(device_,emptyDistance_);
    emptyDistance_=nullptr;
}
}
