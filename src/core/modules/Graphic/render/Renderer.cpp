#include "Renderer.h"
#include "Profile.h"
#include "ShaderCommands.h"
#include "GlyphTextures.h"
#include "core/modules/Graphic/readback/Readback.h"
#include "core/runtime/Log/Log.h"
#include "core/modules/Graphic/backend/ImagePipeline.h"
#include "core/modules/Graphic/geometry/Ribbon.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <utility>

namespace core::modules::Graphic::Render {
namespace {
using Clock = std::chrono::steady_clock;
using Attachment = Canvas::detail::Attachment;
constexpr std::size_t noFrame = std::numeric_limits<std::size_t>::max();
struct PassGuard {
    SDL_GPURenderPass* value;
    ~PassGuard(){if(value) SDL_EndGPURenderPass(value);}
    void End(){SDL_EndGPURenderPass(value);value=nullptr;}
};
float GpuFloat(double value) {
    const auto result=static_cast<float>(value);
    if (!std::isfinite(result)) throw std::invalid_argument("3D value exceeds GPU float range");
    return result;
}
struct FogUniform { std::array<float,4> range{}, color{}, operations{}; };
FogUniform FogData(const std::optional<Geometry::Fog>& fog) {
    FogUniform data;
    if (fog) {
        data.range={GpuFloat(fog->start),GpuFloat(fog->end),float(fog->mode),1};
        if(data.range[0]>=data.range[1] || !std::isfinite(data.range[1]-data.range[0]))
            throw std::invalid_argument("Fog interval exceeds GPU precision/range");
        const auto c=fog->color;
        data.color={float((c>>16)&255)/255,float((c>>8)&255)/255,float(c&255)/255,float(c>>24)/255};
        data.operations={float(fog->rgb),float(fog->alpha),0,0};
    }
    return data;
}
SDL_GPUDepthStencilTargetInfo DepthTarget(const Attachment& target,bool clear) {
    SDL_GPUDepthStencilTargetInfo result{};
    result.texture=target.depth; result.clear_depth=1;
    result.load_op=clear ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
    result.store_op=SDL_GPU_STOREOP_STORE;
    result.stencil_load_op=SDL_GPU_LOADOP_DONT_CARE;
    result.stencil_store_op=SDL_GPU_STOREOP_DONT_CARE;
    return result;
}
// Color and distance must belong to the same rasterization/depth operation.
// The distance target is still attached with Z disabled, but its pipeline write
// mask is zero so that older valid surface distances survive color-only draws.
std::array<SDL_GPUColorTargetInfo,2> ColorTargets(const Attachment& target,
    SDL_GPUTexture* color,bool clear,SDL_FColor clearColor) {
    std::array<SDL_GPUColorTargetInfo,2> result{};
    auto& output=result[0];
    output.texture=color;
    output.load_op=clear ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
    output.store_op=SDL_GPU_STOREOP_STORE;
    output.clear_color=clearColor;
    // Never cycle attachments: A -> B -> A must preserve A and its history.
    output.cycle=false;
    if(target.distance) {
        result[1]=output;
        result[1].texture=target.distance;
        result[1].clear_color={0,0,0,0};
    }
    return result;
}
SDL_FColor Pma(std::uint32_t argb) {
    const float a = float(argb >> 24) / 255;
    return {float((argb >> 16) & 255) / 255 * a, float((argb >> 8) & 255) / 255 * a, float(argb & 255) / 255 * a, a};
}
}
struct Renderer::Impl {
    enum class Kind { Target, Clear, Image, Shader, Text };
    struct TextPart {
        GlyphTextures::Lease glyph;
        Uint32 inputVertex{},firstVertex{};
        bool outline{};
    };
    struct Command {
        Kind kind;
        std::shared_ptr<Attachment> target;
        std::uint32_t argb{};
        Render::Image image;
        std::shared_ptr<Attachment> sourceCanvas;
        Paint paint;
        Uint32 inputVertex{}, vertexCount{};
        bool ribbon{};
        std::size_t sequence{};
        Uint32 firstVertex{};
        SDL_GPUGraphicsPipeline* pipeline{};
        SDL_GPUSampler* sampler{};
        Geometry::SpatialState spatial;
        std::shared_ptr<detail::ShaderCommand> shader;
        std::shared_ptr<const Text::PreparedText> text;
        std::vector<TextPart> textParts;
    };
    struct Frame {
        std::vector<Command> commands;
        std::vector<detail::Vertex> recordedVertices;
        std::vector<detail::Vertex> vertices;
        SDL_GPUBuffer* vertexBuffer{};
        SDL_GPUTransferBuffer* transfer{};
        Uint32 bufferBytes{};
        SDL_GPUFence* fence{};
        Clock::time_point submitted;
        std::size_t shaderBytes{};
        std::vector<GlyphTextures::Lease> glyphUploads;
        SDL_GPUCommandBuffer* presentCommand{};
        SDL_GPUTexture* swapchain{};
        SDL_GPUTexture* mirror{};
        Uint32 windowWidth{},windowHeight{},mirrorWidth{},mirrorHeight{};
        SDL_GPUTextureFormat mirrorFormat{SDL_GPU_TEXTUREFORMAT_INVALID};
        bool acquired{},mirrored{},windowInitialized{};
    };
    SDL_GPUDevice* device;
    SDL_Window* window;
    Config config;
    Canvas::Store canvases;
    const std::thread::id thread{std::this_thread::get_id()};
    std::vector<Frame> frames;
    std::unique_ptr<backend::ImagePipeline> imagePipeline;
    std::unique_ptr<detail::ShaderCommands> shaderCommands;
    Text::GlyphCache glyphMasks;
    std::unique_ptr<GlyphTextures::Store> glyphTextures;
    std::uint64_t contentSerial{};
    Geometry::RibbonBuilder ribbonBuilder;
    std::vector<detail::Vertex> ribbonVertices;
    std::size_t current{noFrame};
    Canvas::Handle target;
    Stats stats;
    std::unique_ptr<Readback::Request> capture;
    std::shared_ptr<Attachment> captureSource;
    SDL_GPUFence* captureFence{};
    bool closed{}, fatal{}, shutdownResult{true};

    Impl(SDL_GPUDevice* d, SDL_Window* w, Config c) : device(d), window(w), config(c), canvases(d,w,c.canvases) {
        if (!c.framesInFlight || c.framesInFlight > 8 || !c.maxCommands || !c.fenceTimeoutMs ||
            c.maxVertexBytes < 6*sizeof(detail::Vertex) || c.maxVertexBytes > (1u<<30) || !c.maxShaderBytes ||
            !c.maxReadbackBytes || !c.maxPngBytes)
            throw std::invalid_argument("Invalid Renderer frame limits");
        frames.resize(c.framesInFlight);
        for (auto& frame : frames) frame.commands.reserve(std::min<std::size_t>(c.maxCommands,256));
#ifdef ENGINE_SPRITE_PROFILE
        if(!profile::state.path.empty()) {
            const char* adapter=SDL_GetStringProperty(SDL_GetGPUDeviceProperties(device),SDL_PROP_GPU_DEVICE_NAME_STRING,"unknown");
            const char* driver=SDL_GetGPUDeviceDriver(device);
            core::runtime::Log::Info(std::string("Sprite profile GPU: adapter='")+adapter+"', driver='"+(driver?driver:"unknown")+"'");
        }
#endif
    }
    void CheckThread() const {
        if (thread != std::this_thread::get_id()) throw std::logic_error("Renderer access requires the render owner thread");
    }
    void Check() const {
        CheckThread();
        if (closed || fatal) throw std::logic_error("Renderer is shut down or failed");
    }
    Frame& Recording() {
        Check();
        if (current == noFrame) throw std::logic_error("Drawing requires an active Renderer frame");
        return frames[current];
    }
    void Append(Kind kind, Canvas::Handle handle, std::uint32_t argb = 0) {
        auto& frame = Recording();
        if (frame.commands.size() >= config.maxCommands) throw std::length_error("Renderer command budget exhausted");
        // Acquire validates even a target destroyed after SetTargetCanvas. Old
        // accepted commands retain their attachment; new access cannot revive it.
        auto pin = canvases.Acquire(handle);
        Command command{}; command.kind=kind; command.target=std::move(pin); command.argb=argb;
        frame.commands.push_back(std::move(command));
    }
    bool SkipWindow(const Attachment& output) const {
        return output.info.window && (!window ||
            (SDL_GetWindowFlags(window)&SDL_WINDOW_MINIMIZED));
    }
    Render::Image CanvasImage(Canvas::Handle handle) {
        auto pin=canvases.Acquire(handle);
        if(pin->info.window) throw std::invalid_argument("Window Canvas cannot be sampled");
        Render::Image image;
        image.texture=pin->texture; image.lifetime=pin;
        image.textureWidth=image.width=pin->info.width; image.textureHeight=image.height=pin->info.height;
        image.anchorX=double(image.width)/2; image.anchorY=double(image.height)/2;
        image.alpha=Alpha::Premultiplied;
        return image;
    }
    void Commit(Canvas::Handle destination, const Render::Image& image, Paint paint, std::span<const detail::Vertex> vertices,
        std::shared_ptr<Attachment> sourceCanvas={}, bool ribbon=false) {
        auto& frame=Recording();
        detail::Validate(image,paint);
        const auto maximum=config.maxVertexBytes/sizeof(detail::Vertex);
        if(frame.commands.size()>=config.maxCommands || vertices.size()>maximum-frame.recordedVertices.size())
            throw std::length_error("Renderer draw command/vertex budget exhausted");
        auto output=canvases.Acquire(destination);
        if(image.texture==output->texture) throw std::invalid_argument("Canvas cannot sample itself while it is the output target");
        if(SkipWindow(*output)) {Append(Kind::Target,destination);return;}
        Command command{};
        command.kind=Kind::Image; command.target=std::move(output); command.image=image;
        command.spatial=command.target->spatial;
        command.sourceCanvas=std::move(sourceCanvas); command.paint=paint; command.sequence=frame.commands.size();
        command.inputVertex=static_cast<Uint32>(frame.recordedVertices.size());
        command.vertexCount=static_cast<Uint32>(vertices.size()); command.ribbon=ribbon;
        // Reserve both arenas before publishing either size. Failed submissions
        // never leave a prefix of a Ribbon in the queue. Capacity is frame-owned
        // and reused, including across Image/Ribbon calls and layer sorting.
        if(frame.commands.size()==frame.commands.capacity())
            frame.commands.reserve(std::min(config.maxCommands,std::max<std::size_t>(256,frame.commands.size()*2)));
        const auto size=frame.recordedVertices.size()+vertices.size();
        if(size>frame.recordedVertices.capacity()) frame.recordedVertices.reserve(
            std::max(size,std::min<std::size_t>(maximum,std::max<std::size_t>(256,frame.recordedVertices.capacity()*2))));
        frame.recordedVertices.insert(frame.recordedVertices.end(),vertices.begin(),vertices.end());
        frame.commands.push_back(std::move(command));
    }
    detail::Vertex Project(const Render::Image& image, Canvas::Bounds bounds, Geometry::Point point,
        double u,double t,double q,std::uint32_t color) {
        detail::Vertex v{};
        const auto p=Canvas::ToClip(bounds,{point[0],point[1]});
        v.x=static_cast<float>(p.x); v.y=static_cast<float>(p.y);
        if(!std::isfinite(v.x) || !std::isfinite(v.y)) throw std::invalid_argument("Image projection exceeds GPU float range");
        const float u0=float(image.x)/image.textureWidth, v0=float(image.y)/image.textureHeight;
        const float u1=float(image.x+image.width)/image.textureWidth, v1=float(image.y+image.height)/image.textureHeight;
        // Interpolate image-local UV before applying the logical source rect in
        // the fragment shader. Tiny crops must not collapse a UV denominator.
        v.u=static_cast<float>(u*q);
        v.v=static_cast<float>(t*q); v.q=static_cast<float>(q);
        if(!std::isfinite(v.u) || !std::isfinite(v.v) || !std::isfinite(v.q) || v.q<=0)
            throw std::invalid_argument("Image UV exceeds GPU interpolation range");
        v.r=static_cast<std::uint8_t>((color>>16)&255); v.g=static_cast<std::uint8_t>((color>>8)&255);
        v.b=static_cast<std::uint8_t>(color&255); v.a=static_cast<std::uint8_t>(color>>24);
        v.minU=u0; v.minV=v0; v.maxU=u1; v.maxV=v1;
        v.logicalX=static_cast<float>(image.insetX); v.logicalY=static_cast<float>(image.insetY);
        v.logicalWidth=static_cast<float>(image.LogicalWidth()); v.logicalHeight=static_cast<float>(image.LogicalHeight());
        v.premultiplied=image.alpha==Alpha::Premultiplied ? 1.f:0.f;
        if (image.mipLevels) {
            v.sampling=4.f+(image.filter==Filter::Linear ? 2.f:0.f)+(image.address==Address::Repeat ? 1.f:0.f);
            v.mipLevels=static_cast<float>(image.mipLevels);
            v.mipExtrusion=static_cast<float>(image.mipExtrusion);
        } else if (image.textureLevels>1) {
            // Large Images use ordinary physical mip chains; logical bounds
            // still need to be mapped/addressed separately at every level.
            v.sampling=8.f+(image.filter==Filter::Linear ? 2.f:0.f)+(image.address==Address::Repeat ? 1.f:0.f);
            v.mipLevels=static_cast<float>(image.textureLevels);
        } else if (image.address==Address::Repeat) {
            const bool whole=image.x==0 && image.y==0 && image.insetX==0 && image.insetY==0 &&
                image.LogicalWidth()==image.textureWidth && image.LogicalHeight()==image.textureHeight;
            // Full independent images retain hardware addressing and mip LOD.
            // Subregions wrap their own pixels, never the entire Atlas page.
            v.sampling=whole ? 1.f : (image.filter==Filter::Point ? 2.f : 3.f);
        }
        return v;
    }
    void Draw(Canvas::Handle destination, const Render::Image& image, const std::array<std::array<double,2>,4>& corners, Paint paint,
        std::shared_ptr<Attachment> sourceCanvas={}, const QuadVertices* explicitVertices=nullptr,
        const std::array<double,4>* weights=nullptr) {
        Recording(); detail::Validate(image,paint);
        const auto info=canvases.Describe(destination);
        if(info.threeD) throw std::invalid_argument("2D drawing requires a 2D Canvas target");
        const auto bounds=info.bounds;
        const std::array<Geometry::Point,4> uv{{{1,0},{0,0},{0,1},{1,1}}};
        std::array<detail::Vertex,4> quad;
        for(std::size_t i=0;i<4;++i) {
            const double q=weights ? (*weights)[i]:1;
            const double u=explicitVertices ? (*explicitVertices)[i].u:uv[i][0];
            const double t=explicitVertices ? (*explicitVertices)[i].v:uv[i][1];
            const auto color=explicitVertices ? (*explicitVertices)[i].color:paint.color;
            quad[i]=Project(image,bounds,corners[i],u,t,q,color);
        }
        const std::array<detail::Vertex,6> triangles{quad[0],quad[1],quad[2],quad[0],quad[2],quad[3]};
        Commit(destination,image,paint,triangles,std::move(sourceCanvas));
    }
    void Ribbon(Canvas::Handle destination, const Render::Image& image,std::span<const Geometry::Point> points,double width,Paint paint,bool closedPath,
        std::shared_ptr<Attachment> sourceCanvas={}) {
        Recording(); detail::Validate(image,paint);
        const auto info=canvases.Describe(destination);
        if(info.threeD) throw std::invalid_argument("RenderRibbon requires a 2D Canvas target");
        const auto bounds=info.bounds;
        const auto mesh=ribbonBuilder.Build(points,width,closedPath);
        ribbonVertices.clear(); ribbonVertices.reserve(mesh.size());
        for(const auto& vertex:mesh) ribbonVertices.push_back(Project(image,bounds,vertex.position,vertex.u,vertex.v,1,paint.color));
        Commit(destination,image,paint,ribbonVertices,std::move(sourceCanvas),true);
    }
    void Draw3D(Canvas::Handle destination, const Render::Image& image,const QuadVertices3& points,Blend blend,std::shared_ptr<Attachment> sourceCanvas={}) {
        Recording(); detail::Validate(image,{0xFFFFFFFF,blend,0});
        const auto output=canvases.Acquire(destination);
        if(!output->info.threeD) throw std::invalid_argument("3D drawing requires a Canvas3D target");
        const auto& state=output->spatial;
        const auto view=Geometry::MakeView(state.camera,state.perspective,output->info.width,output->info.height);
        Geometry::Quad3 corners; Geometry::Quad uv;
        for(std::size_t i=0;i<4;++i) { const auto& p=points[i]; corners[i]={p.x,p.y,p.z}; uv[i]={p.u,p.v}; }
        const auto weights=Geometry::ProjectiveWeights(Geometry::FlattenQuad(corners),uv);
        std::array<Geometry::Projected3,4> projected;
        double clipScale{};
        for(std::size_t i=0;i<4;++i) {
            projected[i]=Geometry::Project(view,corners[i]);
            for(const auto component:projected[i].clip) clipScale=std::max(clipScale,std::abs(component));
        }
        if(clipScale==0) throw std::invalid_argument("3D clip coordinates have no representable extent");
        std::array<detail::Vertex,4> quad;
        for(std::size_t i=0;i<4;++i) {
            const auto& p=projected[i];
            // A finite component alone does not imply a representable R32
            // distance. Norms of perspective-interpolated vectors cannot exceed
            // the vertex maximum, so reject invalid geometry before recording.
            (void)GpuFloat(std::hypot(p.relative[0],p.relative[1],p.relative[2]));
            // Reuse the Image-local sampling/color mapping; replace only clip position.
            auto& v=quad[i]; v=Project(image,{-1,1,-1,1},{0,0},uv[i][0],uv[i][1],weights[i],points[i].color);
            // One positive homogeneous scale for the entire primitive preserves
            // clipping, NDC depth and perspective interpolation. Tiny world
            // units must not collapse at the rasterizer's homogeneous-W guard.
            v.x=GpuFloat(p.clip[0]/clipScale); v.y=GpuFloat(p.clip[1]/clipScale);
            v.z=GpuFloat(p.clip[2]/clipScale); v.w=GpuFloat(p.clip[3]/clipScale);
            v.relativeX=GpuFloat(p.relative[0]); v.relativeY=GpuFloat(p.relative[1]); v.relativeZ=GpuFloat(p.relative[2]);
        }
        const std::array<detail::Vertex,6> triangles{quad[0],quad[1],quad[2],quad[0],quad[2],quad[3]};
        Commit(destination,image,{0xFFFFFFFF,blend,0},triangles,std::move(sourceCanvas));
    }
    struct Version { std::uint64_t color{},distance{}; };
    struct Encoding {
        std::vector<Attachment*> initialized;
        std::unordered_map<Attachment*,Version> versions;
        bool windowInitialized{};
    };
    Encoding EncodingState(const Frame& frame,Attachment* extra=nullptr) {
        Encoding result; result.windowInitialized=frame.windowInitialized;
        const auto add=[&](Attachment* a){
            if(a && !a->info.window) result.versions.try_emplace(a,Version{a->colorSerial,a->distanceSerial});
        };
        add(extra);
        for(const auto& item:frame.commands) {
            add(item.target.get()); add(item.sourceCanvas.get());
            if(item.shader) for(const auto& source:item.shader->canvases) add(source.get());
        }
        result.initialized.reserve(result.versions.size());
        return result;
    }
    void Encode(Frame& frame,SDL_GPUCommandBuffer* command,SDL_GPUTexture* windowTexture,
        Encoding& state,Attachment* readSource=nullptr) {
#ifdef ENGINE_SPRITE_PROFILE
        profile::Timer encodeTimer(profile::Phase::Encode,true);
#endif
        auto& initialized=state.initialized; auto& versions=state.versions;
        auto& windowInitialized=state.windowInitialized;
        if(glyphTextures) {
            for(const auto& item:frame.commands) for(const auto& part:item.textParts) {
                const std::array leases{part.glyph};
                glyphTextures->EncodePending(command,leases);
            }
        }
        auto written=[&](Attachment& a,bool distance) {
            if(a.info.window) return;
            if(contentSerial==std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("Canvas content serial exhausted");
            auto& v=versions.at(&a); v.color=++contentSerial;
            if(distance) v.distance=v.color;
        };
        if(!frame.vertices.empty()) {
#ifdef ENGINE_SPRITE_PROFILE
            profile::Timer uploadTimer(profile::Phase::Upload);
#endif
            auto* copy=SDL_BeginGPUCopyPass(command);
            if(!copy) throw std::runtime_error(std::string("Image vertex copy pass failed: ")+SDL_GetError());
            SDL_GPUTransferBufferLocation source{frame.transfer,0};
            SDL_GPUBufferRegion destination{frame.vertexBuffer,0,static_cast<Uint32>(frame.vertices.size()*sizeof(detail::Vertex))};
            SDL_UploadToGPUBuffer(copy,&source,&destination,false); SDL_EndGPUCopyPass(copy);
            stats.vertexBytes+=destination.size;
        }
        // A newly created Canvas may be read before it is ever selected as a
        // target. Initialize those sources once, before any sorted draw segment.
        // No prior contents exist; later explicit writes still execute in order.
        auto initializeSource=[&](Attachment* source) {
            if(!source || source->initialized || std::find(initialized.begin(),initialized.end(),source)!=initialized.end()) return;
            const auto output=ColorTargets(*source,source->texture,true,{0,0,0,0});
            const auto depth=DepthTarget(*source,true);
            auto* pass=SDL_BeginGPURenderPass(command,output.data(),source->distance ? 2u : 1u,source->depth ? &depth : nullptr);
            if(!pass) throw std::runtime_error(std::string("Canvas source initialization failed: ")+SDL_GetError());
            SDL_EndGPURenderPass(pass); initialized.push_back(source);
            written(*source,true);
            ++stats.renderPasses; ++stats.clearPasses;
        };
        if(readSource && !readSource->info.window) initializeSource(readSource);
        for(const auto& item:frame.commands) {
            initializeSource(item.sourceCanvas.get());
            if(item.shader) for(const auto& source:item.shader->canvases) initializeSource(source.get());
        }
        for(std::size_t i=0;i<frame.commands.size();) {
            const auto& item=frame.commands[i];
            auto& attachment = *item.target;
            auto* texture = attachment.info.window ? windowTexture : attachment.texture;
            if (!texture) {
                if (attachment.info.window) { ++i; continue; } // Minimized/offscreen host; other canvases still execute.
                throw std::logic_error("Canvas attachment retired before accepted work was encoded");
            }
            bool isNew = attachment.info.window ? !windowInitialized :
                !attachment.initialized && std::find(initialized.begin(),initialized.end(),&attachment)==initialized.end();
            if(item.kind==Kind::Shader) {
                // Historical distance must be initialized before its copy, and
                // color conversions must follow all earlier sorted writes.
                if(isNew && !attachment.info.window) {initializeSource(&attachment); isNew=false;}
                shaderCommands->EncodeInputs(command,*item.shader,[&](Attachment* source,bool distance){
                    const auto& v=versions.at(source ? source:&attachment);
                    return distance ? v.distance:v.color;
                });
            }
            const bool clear = item.kind==Kind::Clear || isNew;
            const auto output=ColorTargets(attachment,texture,clear,item.kind==Kind::Clear ? Pma(item.argb) :
                (attachment.info.window ? SDL_FColor{0,0,0,1} : SDL_FColor{0,0,0,0}));
            const auto depth=DepthTarget(attachment,clear);
            auto* pass = SDL_BeginGPURenderPass(command,output.data(),attachment.distance ? 2u : 1u,attachment.depth ? &depth : nullptr);
            if (!pass) throw std::runtime_error(std::string("Renderer target pass failed: ")+SDL_GetError());
            PassGuard passGuard{pass};
            bool geometryDepth{};
            bool drew{};
            if(item.kind==Kind::Shader) {
                shaderCommands->Draw(command,pass,*item.shader);
                geometryDepth=attachment.info.threeD && item.spatial.depthEnabled && bool(item.shader->draw.mesh);
                drew=true;
                ++stats.drawCalls;
                stats.textureBindings+=item.shader->vertexTextures.size()+item.shader->fragmentTextures.size();
                ++i;
            } else if(item.kind==Kind::Text) {
                SDL_GPUBufferBinding binding{frame.vertexBuffer,0};
                SDL_BindGPUVertexBuffers(pass,0,&binding,1);
                SDL_BindGPUGraphicsPipeline(pass,item.pipeline);
                FogUniform fog{};fog.operations[2]=UsesAddColor(item.paint.blend)?1.f:0.f;
                fog.operations[3]=1; // Zero mask coverage is not a rectangular fragment.
                SDL_PushGPUFragmentUniformData(command,0,&fog,sizeof(fog));
                for(const auto& part:item.textParts) {
                    SDL_GPUTextureSamplerBinding sampled{part.outline?part.glyph->Ring(command):part.glyph->Fill(command),glyphTextures->Sampler()};
                    SDL_BindGPUFragmentSamplers(pass,0,&sampled,1);
                    SDL_DrawGPUPrimitives(pass,6,1,part.firstVertex,0);
                    ++stats.drawCalls;++stats.textureBindings;
                }
                drew=!item.textParts.empty();++i;
            } else if(item.kind==Kind::Image) {
                SDL_GPUBufferBinding binding{frame.vertexBuffer,0};
                SDL_BindGPUVertexBuffers(pass,0,&binding,1);
                while(i<frame.commands.size() && frame.commands[i].kind==Kind::Image && frame.commands[i].target.get()==&attachment) {
                    const auto& first=frame.commands[i];
                    std::size_t last=i+1;
                    while(last<frame.commands.size()) {
                        const auto& next=frame.commands[last];
                        if(next.kind!=Kind::Image || next.target!=first.target || next.pipeline!=first.pipeline || next.sampler!=first.sampler ||
                            next.image.texture!=first.image.texture || next.spatial.fog!=first.spatial.fog) break;
                        ++last;
                    }
                    SDL_BindGPUGraphicsPipeline(pass,first.pipeline);
                    SDL_GPUTextureSamplerBinding textureBinding{first.image.texture,first.sampler};
                    SDL_BindGPUFragmentSamplers(pass,0,&textureBinding,1);
                    auto fog=FogData(first.spatial.fog);
                    fog.operations[2]=UsesAddColor(first.paint.blend) ? 1.f : 0.f;
                    SDL_PushGPUFragmentUniformData(command,0,&fog,sizeof(fog));
                    Uint32 vertexCount{};
                    for(auto n=i;n<last;++n) {
                        vertexCount+=frame.commands[n].vertexCount;
                        if(frame.commands[n].ribbon) ++stats.ribbons; else ++stats.sprites;
                    }
                    SDL_DrawGPUPrimitives(pass,vertexCount,1,first.firstVertex,0);
                    ++stats.drawCalls; ++stats.textureBindings;
                    drew=true; geometryDepth=geometryDepth || (attachment.info.threeD && first.spatial.depthEnabled);
                    i=last;
                }
            } else ++i;
            passGuard.End();
            if(clear || drew) written(attachment,clear || geometryDepth);
            ++stats.renderPasses;
            if (clear) ++stats.clearPasses; else ++stats.preservedPasses;
            if (attachment.info.window) windowInitialized=true;
            else if (isNew) initialized.push_back(&attachment);
        }
    }
    void CommitEncoding(Frame& frame,SDL_GPUCommandBuffer* command,const Encoding& state) {
        if(shaderCommands) shaderCommands->Commit(command);
        if(glyphTextures) frame.glyphUploads=glyphTextures->CommitEncoded(command);
        for(const auto& [attachment,version]:state.versions) {attachment->colorSerial=version.color;attachment->distanceSerial=version.distance;}
        for(auto* attachment:state.initialized) attachment->initialized=true;
        frame.windowInitialized=state.windowInitialized;
    }
    void AcquirePresentation(Frame& frame);
    void EnsureMirror(Frame& frame);
    void BlitPresentation(Frame& frame,SDL_GPUCommandBuffer* command);
    void RetirePresentation(Frame& frame);
    void ClearCommands(Frame& frame) {
        frame.commands.clear();frame.recordedVertices.clear();frame.vertices.clear();frame.shaderBytes=0;frame.glyphUploads.clear();
    }
#ifdef ENGINE_SPRITE_PROFILE
    void ProfileMemory(const Frame& frame) const {
        if(!profile::state.active) return;
        auto& sample=profile::state.current;
        sample.commandSize=sizeof(Command);sample.vertexSize=sizeof(detail::Vertex);
        sample.commandCount=frame.commands.size();
        sample.currentCommandBytes=frame.commands.size()*sizeof(Command);
        sample.currentGeometryBytes=(frame.recordedVertices.size()+frame.vertices.size())*sizeof(detail::Vertex);
        sample.currentCommandCapacityBytes=frame.commands.capacity()*sizeof(Command);
        sample.currentGeometryCapacityBytes=(frame.recordedVertices.capacity()+frame.vertices.capacity())*sizeof(detail::Vertex);
        sample.currentTransferCapacityBytes=frame.transfer ? frame.bufferBytes : 0;
        sample.currentGpuCapacityBytes=frame.vertexBuffer ? frame.bufferBytes : 0;
        sample.retainedCommandCapacityBytes=sample.retainedGeometryCapacityBytes=0;
        sample.retainedTransferCapacityBytes=sample.retainedGpuCapacityBytes=0;
        for(const auto& slot:frames) {
            sample.retainedCommandCapacityBytes+=slot.commands.capacity()*sizeof(Command);
            sample.retainedGeometryCapacityBytes+=(slot.recordedVertices.capacity()+slot.vertices.capacity())*sizeof(detail::Vertex);
            if(slot.transfer) sample.retainedTransferCapacityBytes+=slot.bufferBytes;
            if(slot.vertexBuffer) sample.retainedGpuCapacityBytes+=slot.bufferBytes;
        }
        sample.ribbonScratchCapacityBytes=ribbonVertices.capacity()*sizeof(detail::Vertex);
    }
#endif
    void Prepare(Frame& frame) {
#ifdef ENGINE_SPRITE_PROFILE
        profile::Timer prepareTimer(profile::Phase::Prepare,true);
#endif
        // All CPU sorting/allocation and lazy pipeline creation happens before CB
        // acquisition. Order is (Layer,original sequence), never texture order.
        for(auto first=frame.commands.begin();first!=frame.commands.end();) {
            if(first->kind!=Kind::Image && first->kind!=Kind::Shader && first->kind!=Kind::Text) { ++first; continue; }
            auto last=first;
            while(last!=frame.commands.end() && last->target==first->target &&
                (last->kind==Kind::Image || last->kind==Kind::Shader || last->kind==Kind::Text)) ++last;
            if(!first->target->info.threeD) std::sort(first,last,[](const Command& a,const Command& b) {
                return a.paint.layer==b.paint.layer ? a.sequence<b.sequence : a.paint.layer<b.paint.layer;
            });
            first=last;
        }
        frame.vertices.clear();
        frame.vertices.reserve(frame.recordedVertices.size());
        for(auto& command:frame.commands) {
            const auto format=command.target->info.window ? SDL_GetGPUSwapchainTextureFormat(device,window) : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
            if(command.kind==Kind::Shader) {
                // Validate native host shutdown misuse before acquiring a CB.
                (void)command.shader->draw.program->Vertex();
                (void)command.shader->draw.program->Fragment();
                continue;
            }
            if(command.kind!=Kind::Image && command.kind!=Kind::Text) continue;
            if(!imagePipeline) imagePipeline=std::make_unique<backend::ImagePipeline>(device);
            if(command.kind==Kind::Text) {
                command.pipeline=imagePipeline->Get(format,command.paint.blend);
                for(auto& part:command.textParts) {
                    part.firstVertex=static_cast<Uint32>(frame.vertices.size());
                    const auto first=frame.recordedVertices.begin()+part.inputVertex;
                    frame.vertices.insert(frame.vertices.end(),first,first+6);
                }
                continue;
            }
            command.firstVertex=static_cast<Uint32>(frame.vertices.size());
            const auto first=frame.recordedVertices.begin()+command.inputVertex;
            frame.vertices.insert(frame.vertices.end(),first,first+command.vertexCount);
            command.pipeline=imagePipeline->Get(format,command.paint.blend,command.target->depthFormat,command.spatial.depthEnabled);
            command.sampler=imagePipeline->Sampler(command.image.filter,command.image.address);
        }
        const auto bytes=static_cast<Uint32>(frame.vertices.size()*sizeof(detail::Vertex));
        if(!bytes) return;
        if(bytes>frame.bufferBytes) {
            const auto capacity=std::min(config.maxVertexBytes,std::max(4096u,std::bit_ceil(bytes)));
            SDL_GPUBufferCreateInfo bufferInfo{}; bufferInfo.usage=SDL_GPU_BUFFERUSAGE_VERTEX; bufferInfo.size=capacity;
            auto* buffer=SDL_CreateGPUBuffer(device,&bufferInfo);
            if(!buffer) throw std::runtime_error(std::string("Image vertex buffer allocation failed: ")+SDL_GetError());
            SDL_GPUTransferBufferCreateInfo transferInfo{}; transferInfo.usage=SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD; transferInfo.size=capacity;
            auto* transfer=SDL_CreateGPUTransferBuffer(device,&transferInfo);
            if(!transfer) {
                SDL_ReleaseGPUBuffer(device,buffer);
                throw std::runtime_error(std::string("Image transfer allocation failed: ")+SDL_GetError());
            }
            if(frame.vertexBuffer) SDL_ReleaseGPUBuffer(device,frame.vertexBuffer);
            if(frame.transfer) SDL_ReleaseGPUTransferBuffer(device,frame.transfer);
            frame.vertexBuffer=buffer; frame.transfer=transfer; frame.bufferBytes=capacity; ++stats.bufferAllocations;
        }
#ifdef ENGINE_SPRITE_PROFILE
        profile::Timer uploadTimer(profile::Phase::Upload);
#endif
        auto* destination=SDL_MapGPUTransferBuffer(device,frame.transfer,false);
        if(!destination) throw std::runtime_error(std::string("Image vertex transfer map failed: ")+SDL_GetError());
        std::memcpy(destination,frame.vertices.data(),bytes);
        SDL_UnmapGPUTransferBuffer(device,frame.transfer);
    }
};
Renderer::Renderer(SDL_GPUDevice* d, SDL_Window* w, Config c) : impl(std::make_unique<Impl>(d,w,c)) {}
Renderer::~Renderer() { Shutdown(); }
void Renderer::Impl::AcquirePresentation(Frame& frame) {
#ifdef ENGINE_SPRITE_PROFILE
    profile::Timer acquireTimer(profile::Phase::Acquire);
#endif
    if(frame.acquired || !window) return;
    // Some SDL GPU drivers return the retained backbuffer even when minimized.
    // Freeze this frame as offscreen-only instead of treating it as presentable.
    if(SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
        frame.acquired=true;
        return;
    }
    auto* command=SDL_AcquireGPUCommandBuffer(device);
    if(!command) throw std::runtime_error(std::string("Presentation command buffer failed: ")+SDL_GetError());
    SDL_GPUTexture* texture{}; Uint32 width{},height{};
    if(!SDL_WaitAndAcquireGPUSwapchainTexture(command,window,&texture,&width,&height)) {
        SDL_CancelGPUCommandBuffer(command);
        throw std::runtime_error(std::string("Swapchain acquisition failed: ")+SDL_GetError());
    }
    frame.acquired=true; frame.swapchain=texture; frame.windowWidth=width; frame.windowHeight=height;
    if(texture) frame.presentCommand=command;
    else SDL_CancelGPUCommandBuffer(command); // Minimized: no presentable image for this frame.
}
void Renderer::Impl::EnsureMirror(Frame& frame) {
    if(frame.mirrored || !frame.swapchain) return;
    const auto format=SDL_GetGPUSwapchainTextureFormat(device,window);
    const auto width=frame.windowWidth,height=frame.windowHeight;
    if(!width || !height || width>16384 || height>16384 || std::uint64_t(width)*height*4>config.maxReadbackBytes)
        throw std::length_error("Window capture mirror exceeds the configured budget");
    if(!frame.mirror || frame.mirrorWidth!=width || frame.mirrorHeight!=height || frame.mirrorFormat!=format) {
        SDL_GPUTextureCreateInfo create{};create.type=SDL_GPU_TEXTURETYPE_2D;create.format=format;
        create.usage=SDL_GPU_TEXTUREUSAGE_COLOR_TARGET|SDL_GPU_TEXTUREUSAGE_SAMPLER;
        create.width=width;create.height=height;create.layer_count_or_depth=create.num_levels=1;
        if(!SDL_GPUTextureSupportsFormat(device,format,create.type,create.usage))
            throw std::runtime_error("Window format cannot be used for a readable capture mirror");
        auto* texture=SDL_CreateGPUTexture(device,&create);
        if(!texture) throw std::runtime_error(std::string("Window capture mirror failed: ")+SDL_GetError());
        if(frame.mirror) SDL_ReleaseGPUTexture(device,frame.mirror); // This frame slot has no unfinished fence.
        frame.mirror=texture;frame.mirrorWidth=width;frame.mirrorHeight=height;frame.mirrorFormat=format;
    }
    frame.mirrored=true;
}
void Renderer::Impl::BlitPresentation(Frame& frame,SDL_GPUCommandBuffer* command) {
    if(!frame.swapchain) return;
    if(frame.mirrored && frame.windowInitialized) {
        SDL_GPUBlitInfo blit{};
        blit.source.texture=frame.mirror;blit.source.w=frame.mirrorWidth;blit.source.h=frame.mirrorHeight;
        blit.destination.texture=frame.swapchain;blit.destination.w=frame.windowWidth;blit.destination.h=frame.windowHeight;
        blit.load_op=SDL_GPU_LOADOP_DONT_CARE;blit.filter=SDL_GPU_FILTER_NEAREST;
        SDL_BlitGPUTexture(command,&blit);
    } else if(!frame.windowInitialized) {
        SDL_GPUColorTargetInfo clear{};clear.texture=frame.swapchain;clear.load_op=SDL_GPU_LOADOP_CLEAR;
        clear.store_op=SDL_GPU_STOREOP_STORE;clear.clear_color={0,0,0,1};
        auto* pass=SDL_BeginGPURenderPass(command,&clear,1,nullptr);
        if(!pass) throw std::runtime_error(SDL_GetError());
        SDL_EndGPURenderPass(pass);
    }
}
void Renderer::Impl::RetirePresentation(Frame& frame) {
    if(!frame.presentCommand) return;
    auto* command=std::exchange(frame.presentCommand,nullptr);
    try { BlitPresentation(frame,command); }
    catch(...) {SDL_SubmitGPUCommandBuffer(command);throw;}
    frame.fence=SDL_SubmitGPUCommandBufferAndAcquireFence(command);
    if(!frame.fence) throw std::runtime_error(std::string("Presentation retirement failed: ")+SDL_GetError());
    frame.submitted=Clock::now();++stats.submittedFrames;++stats.inFlight;
}
bool Renderer::HasAcquiredSwapchain() const {
    impl->CheckThread();
    return std::any_of(impl->frames.begin(),impl->frames.end(),[](const auto& f){return f.presentCommand && f.swapchain;});
}
Canvas::Store& Renderer::Canvases() { impl->Check(); return impl->canvases; }
void Renderer::Poll() {
    impl->Check();
    for (auto& frame : impl->frames) {
        if (!frame.fence) continue;
        if (SDL_QueryGPUFence(impl->device,frame.fence)) {
            SDL_ReleaseGPUFence(impl->device,frame.fence); frame.fence = nullptr;
            impl->ClearCommands(frame); --impl->stats.inFlight; ++impl->stats.completedFrames;
        } else if (Clock::now()-frame.submitted > std::chrono::milliseconds(impl->config.fenceTimeoutMs)) {
            impl->fatal = true;
            throw std::runtime_error("Renderer GPU frame fence timed out");
        }
    }
    if(impl->shaderCommands) impl->shaderCommands->Reclaim();
    if(impl->glyphTextures) impl->glyphTextures->Reclaim();
    impl->canvases.Reclaim();
}
bool Renderer::TryBeginFrame() {
    impl->Check();
    if (impl->current != noFrame) throw std::logic_error("Renderer frame is already open");
    Poll();
    for (std::size_t i=0; i<impl->frames.size(); ++i) {
        auto& frame = impl->frames[i];
        if (frame.fence) continue;
        auto window = impl->canvases.GetWindowCanvas();
        Impl::Command command{}; command.kind=Impl::Kind::Target; command.target=impl->canvases.Acquire(window);
        frame.commands.push_back(std::move(command)); frame.recordedVertices.clear();
        frame.acquired=frame.mirrored=frame.windowInitialized=false;
        frame.swapchain=nullptr;frame.windowWidth=frame.windowHeight=0;
        impl->target = window; impl->current = i;
        return true;
    }
    ++impl->stats.backpressure;
    return false;
}
void Renderer::SetTargetCanvas(Canvas::Handle handle) {
    impl->Append(Impl::Kind::Target,handle);
    impl->target = handle; // Commit only after successful append.
}
Canvas::Handle Renderer::GetTargetCanvas() {
    impl->Recording(); (void)impl->canvases.Describe(impl->target); return impl->target;
}
Canvas::Info Renderer::GetTargetInfo(std::optional<Canvas::Handle> requested) { impl->Recording(); return impl->canvases.Describe(requested.value_or(impl->target)); }
Geometry::SpatialState Renderer::GetSpatialState(Canvas::Handle handle) const {
    impl->Check(); const auto value=impl->canvases.Acquire(handle);
    if(!value->info.threeD) throw std::invalid_argument("Camera, perspective, fog and ZBuffer require Canvas3D");
    return value->spatial;
}
void Renderer::SetSpatialState(Canvas::Handle handle,const Geometry::SpatialState& state) {
    const auto before=GetSpatialState(handle);
    Geometry::ValidatePoint3(state.camera.position); Geometry::ValidateDirection(state.camera.look); Geometry::ValidateDirection(state.camera.up);
    Geometry::ValidatePerspective(state.perspective);
    if(state.fog) Geometry::ValidateFog(*state.fog);
    (void)FogData(state.fog);
    if(before==state) return;
    // State may be configured during startup. During recording, an active target
    // change seals the preceding segment before publishing the new state.
    if(impl->current!=noFrame) impl->Append(Impl::Kind::Target,handle);
    impl->canvases.Acquire(handle)->spatial=state;
}
void Renderer::Clear(std::uint32_t argb, std::optional<Canvas::Handle> requested) { impl->Append(Impl::Kind::Clear,requested.value_or(impl->target),argb); }
void Renderer::RenderImage(const Image& image, Transform transform, Paint paint, std::optional<Canvas::Handle> requested) {
    impl->Draw(requested.value_or(impl->target),image,detail::ImageCorners(image,transform),paint);
}
void Renderer::RenderImage(Canvas::Handle source, Transform transform, Paint paint, std::optional<Canvas::Handle> requested) {
    impl->Recording();
    const auto image=impl->CanvasImage(source);
    impl->Draw(requested.value_or(impl->target),image,detail::ImageCorners(image,transform),paint,impl->canvases.Acquire(source));
}
void Renderer::RenderRect(const Image& image, double l,double r,double b,double t, Paint paint, std::optional<Canvas::Handle> requested) {
    impl->Draw(requested.value_or(impl->target),image,{{{r,t},{l,t},{l,b},{r,b}}},paint);
}
void Renderer::RenderRect(Canvas::Handle source, double l,double r,double b,double t, Paint paint, std::optional<Canvas::Handle> requested) {
    impl->Recording();
    impl->Draw(requested.value_or(impl->target),impl->CanvasImage(source),{{{r,t},{l,t},{l,b},{r,b}}},paint,impl->canvases.Acquire(source));
}
void Renderer::RenderQuad(const Image& image,const Geometry::Quad& corners,Paint paint, std::optional<Canvas::Handle> requested) {
    impl->Recording();
    const auto weights=Geometry::ProjectiveWeights(corners,{{{1,0},{0,0},{0,1},{1,1}}});
    impl->Draw(requested.value_or(impl->target),image,corners,paint,{},nullptr,&weights);
}
void Renderer::RenderQuad(Canvas::Handle source,const Geometry::Quad& corners,Paint paint, std::optional<Canvas::Handle> requested) {
    impl->Recording();
    const auto weights=Geometry::ProjectiveWeights(corners,{{{1,0},{0,0},{0,1},{1,1}}});
    impl->Draw(requested.value_or(impl->target),impl->CanvasImage(source),corners,paint,impl->canvases.Acquire(source),nullptr,&weights);
}
namespace {
std::array<double,4> QuadMapping(const QuadVertices& vertices,Geometry::Quad& corners) {
    Geometry::Quad uv{};
    for(std::size_t i=0;i<4;++i) { corners[i]={vertices[i].x,vertices[i].y}; uv[i]={vertices[i].u,vertices[i].v}; }
    return Geometry::ProjectiveWeights(corners,uv);
}
}
void Renderer::Render4V(const Image& image,const QuadVertices& vertices,Blend blend,double layer, std::optional<Canvas::Handle> requested) {
    impl->Recording();
    Geometry::Quad corners{}; const auto weights=QuadMapping(vertices,corners);
    impl->Draw(requested.value_or(impl->target),image,corners,{0xFFFFFFFF,blend,layer},{},&vertices,&weights);
}
void Renderer::Render4V(Canvas::Handle source,const QuadVertices& vertices,Blend blend,double layer, std::optional<Canvas::Handle> requested) {
    impl->Recording();
    Geometry::Quad corners{}; const auto weights=QuadMapping(vertices,corners);
    impl->Draw(requested.value_or(impl->target),impl->CanvasImage(source),corners,{0xFFFFFFFF,blend,layer},impl->canvases.Acquire(source),&vertices,&weights);
}
namespace {
QuadVertices3 Quad3Points(const Geometry::Quad3& corners,std::uint32_t color) {
    const Geometry::Quad uv{{{1,0},{0,0},{0,1},{1,1}}};
    QuadVertices3 result;
    for(std::size_t i=0;i<4;++i) result[i]={corners[i][0],corners[i][1],corners[i][2],uv[i][0],uv[i][1],color};
    return result;
}
}
void Renderer::RenderQuad3D(const Image& image,const Geometry::Quad3& corners,std::uint32_t color,Blend blend, std::optional<Canvas::Handle> requested) {
    impl->Draw3D(requested.value_or(impl->target),image,Quad3Points(corners,color),blend);
}
void Renderer::RenderQuad3D(Canvas::Handle source,const Geometry::Quad3& corners,std::uint32_t color,Blend blend, std::optional<Canvas::Handle> requested) {
    impl->Recording(); impl->Draw3D(requested.value_or(impl->target),impl->CanvasImage(source),Quad3Points(corners,color),blend,impl->canvases.Acquire(source));
}
void Renderer::Render4V3D(const Image& image,const QuadVertices3& points,Blend blend, std::optional<Canvas::Handle> requested) { impl->Draw3D(requested.value_or(impl->target),image,points,blend); }
void Renderer::Render4V3D(Canvas::Handle source,const QuadVertices3& points,Blend blend, std::optional<Canvas::Handle> requested) {
    impl->Recording(); impl->Draw3D(requested.value_or(impl->target),impl->CanvasImage(source),points,blend,impl->canvases.Acquire(source));
}
void Renderer::RenderRibbon(const Image& image,std::span<const Geometry::Point> points,double width,Paint paint,bool closed, std::optional<Canvas::Handle> requested) {
    impl->Ribbon(requested.value_or(impl->target),image,points,width,paint,closed);
}
void Renderer::RenderRibbon(Canvas::Handle source,std::span<const Geometry::Point> points,double width,Paint paint,bool closed, std::optional<Canvas::Handle> requested) {
    impl->Recording();
    impl->Ribbon(requested.value_or(impl->target),impl->CanvasImage(source),points,width,paint,closed,impl->canvases.Acquire(source));
}
void Renderer::RenderText(std::shared_ptr<const Text::LayoutResult> layout,double x,double y,
    std::uint32_t color,Blend blend,double layer,std::optional<Canvas::Handle> requested) {
    auto& frame=impl->Recording();
    if(!layout || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(layer) || !IsValidBlend(blend))
        throw std::invalid_argument("Text drawing requires layout, finite coordinates/layer and a valid blend");
    const auto handle=requested.value_or(impl->target);
    auto target=impl->canvases.Acquire(handle);
    const auto info=impl->canvases.Describe(handle);
    if(info.threeD) throw std::invalid_argument("Text drawing requires a 2D Canvas");
    if(impl->SkipWindow(*target)) {impl->Append(Impl::Kind::Target,handle);return;}
    std::size_t bytes=sizeof(Impl::Command)+sizeof(Text::LayoutResult);
    const auto account=[&](std::size_t size) {
        if(size>impl->config.maxShaderBytes || bytes>impl->config.maxShaderBytes-size)
            throw std::length_error("Renderer text snapshot budget exhausted");
        bytes+=size;
    };
    account(layout->text.size());account(layout->lines.size()*sizeof(Text::Line));
    account(layout->glyphs.size()*(sizeof(Text::Glyph)+sizeof(Text::PreparedGlyph)+sizeof(Impl::TextPart)*2));
    if(frame.commands.size()>=impl->config.maxCommands || bytes>impl->config.maxShaderBytes-frame.shaderBytes)
        throw std::length_error("Renderer text command/snapshot budget exhausted");
    const double rasterScale=std::max(info.width/std::abs(info.bounds.right-info.bounds.left),
        info.height/std::abs(info.bounds.top-info.bounds.bottom));
    const auto prepared=impl->glyphMasks.Prepare(std::move(layout),rasterScale);
    const auto anchor=prepared->layout->AnchorPoint();
    std::size_t count{};
    for(const auto& g:prepared->glyphs) if(g.bitmap->width && g.bitmap->height) ++count;
    if(!count) return; // Layout and target validation still ran; no rectangular blank glyph.
    const bool outline=prepared->layout->style.outlineWidth>0;
    const auto vertexCount=count*6*(outline?2:1),maximum=impl->config.maxVertexBytes/sizeof(detail::Vertex);
    if(vertexCount>maximum-frame.recordedVertices.size()) throw std::length_error("Renderer text vertex budget exhausted");
    if(!impl->glyphTextures) impl->glyphTextures=std::make_unique<GlyphTextures::Store>(impl->device);
    Impl::Command command{};command.kind=Impl::Kind::Text;command.target=std::move(target);
    command.paint={color,blend,layer};command.sequence=frame.commands.size();command.text=prepared;
    command.textParts.reserve(count*(outline?2:1));
    std::vector<detail::Vertex> vertices;vertices.reserve(vertexCount);
    // A text command remains one ordering unit. Every ring is emitted before
    // every fill, so later glyph outlines never cover an earlier glyph's ink.
    for(int phase=outline?0:1;phase<2;++phase) {
        for(const auto& glyph:prepared->glyphs) {
            const auto& bitmap=*glyph.bitmap;
            if(!bitmap.width || !bitmap.height) continue;
            const auto lease=impl->glyphTextures->Acquire(glyph.bitmap);
            const double left=x+glyph.x-anchor.x+bitmap.left/bitmap.rasterScale;
            const double top=y-(glyph.y-anchor.y+bitmap.top/bitmap.rasterScale);
            const double right=left+bitmap.width/bitmap.rasterScale,bottom=top-bitmap.height/bitmap.rasterScale;
            const std::array<Geometry::Point,4> corners{{{right,top},{left,top},{left,bottom},{right,bottom}}};
            const std::array<Geometry::Point,4> uv{{{1,0},{0,0},{0,1},{1,1}}};
            Image image;image.width=image.textureWidth=bitmap.width;image.height=image.textureHeight=bitmap.height;
            std::array<detail::Vertex,4> quad;
            const auto tint=phase==0?prepared->layout->style.outlineColor:color;
            for(std::size_t i=0;i<4;++i) quad[i]=impl->Project(image,info.bounds,corners[i],uv[i][0],uv[i][1],1,tint);
            command.textParts.push_back({lease,static_cast<Uint32>(frame.recordedVertices.size()+vertices.size()),0,phase==0});
            for(const auto i:{0,1,2,0,2,3}) vertices.push_back(quad[i]);
        }
    }
    // Admission is atomic: allocation or glyph failure never records a prefix.
    frame.commands.reserve(frame.commands.size()+1);
    frame.recordedVertices.reserve(frame.recordedVertices.size()+vertices.size());
    frame.recordedVertices.insert(frame.recordedVertices.end(),vertices.begin(),vertices.end());
    frame.commands.push_back(std::move(command));frame.shaderBytes+=bytes;
}
void Renderer::RenderShader(ShaderDraw draw, std::optional<Canvas::Handle> requested) {
    auto& frame=impl->Recording();
    if(!draw.program || !std::isfinite(draw.layer)) throw std::invalid_argument("Shader draw requires a live Program and finite layer");
    const auto info=impl->canvases.Describe(requested.value_or(impl->target));
    if(info.threeD && draw.layer!=0) throw std::invalid_argument("Canvas3D Shader draw does not accept layer");
    std::size_t bytes=sizeof(detail::ShaderCommand);
    auto account=[&](std::size_t size) {
        if(size>impl->config.maxShaderBytes || bytes>impl->config.maxShaderBytes-size)
            throw std::length_error("Renderer Shader snapshot budget exhausted");
        bytes+=size;
    };
    if(draw.sources.size()>impl->config.maxShaderBytes/(sizeof(ShaderSource)+128) || draw.vertex.textures.size()>16 || draw.fragment.textures.size()>16 ||
        draw.vertex.params.size()>4 || draw.fragment.params.size()>4)
        throw std::length_error("Shader snapshot exceeds stage ABI limits");
    account(draw.sources.size()*(sizeof(ShaderSource)+128));
    for(const auto& value:draw.vertex.params) account(value.bytes.size());
    for(const auto& value:draw.fragment.params) account(value.bytes.size());
    // Include the bounded generated uniforms in the per-frame ownership budget.
    for(const auto& value:draw.program->Metadata().vertex.buffers) account(value.size);
    for(const auto& value:draw.program->Metadata().fragment.buffers) account(value.size);
    if(frame.commands.size()>=impl->config.maxCommands || bytes>impl->config.maxShaderBytes-frame.shaderBytes)
        throw std::length_error("Renderer Shader command/snapshot budget exhausted");
    auto output=impl->canvases.Acquire(requested.value_or(impl->target));
    auto shader=std::make_shared<detail::ShaderCommand>();
    shader->info=info; shader->canvases.resize(draw.sources.size());
    for(std::size_t i=0;i<draw.sources.size();++i) {
        if(auto handle=std::get_if<Canvas::Handle>(&draw.sources[i])) {
            auto pin=impl->canvases.Acquire(*handle);
            if(pin->info.window || pin==output) throw std::invalid_argument("Shader input cannot be WindowCanvas or its output target");
            shader->canvases[i]=std::move(pin);
        } else if(auto image=std::get_if<Image>(&draw.sources[i])) {
            detail::Validate(*image,{});
            if(image->texture==output->texture) throw std::invalid_argument("Shader cannot sample its output target");
        }
    }
    shader->draw=std::move(draw);
    if(!impl->shaderCommands) impl->shaderCommands=std::make_unique<detail::ShaderCommands>(impl->device);
    const auto format=info.window ? SDL_GetGPUSwapchainTextureFormat(impl->device,impl->window):SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    // Prepare while handles are live. Destroy after acceptance invalidates new
    // access, but cannot invalidate a command already pinned for submission.
    impl->shaderCommands->Prepare(*shader,output->spatial,output,format);
    if((shader->draw.mesh && shader->draw.mesh->Vertices().empty()) || impl->SkipWindow(*output)) {
        impl->Append(Impl::Kind::Target,requested.value_or(impl->target));return;
    }
    Impl::Command command{}; command.kind=Impl::Kind::Shader; command.target=std::move(output);
    command.spatial=command.target->spatial; command.paint.layer=shader->draw.layer;
    command.sequence=frame.commands.size(); command.shader=std::move(shader);
    frame.commands.push_back(std::move(command)); frame.shaderBytes+=bytes;
}
void Renderer::AbortFrame() {
    impl->Check();
    if (impl->current == noFrame) return;
    auto& frame=impl->frames[impl->current];
    impl->ClearCommands(frame);impl->current=noFrame;
    // SDL cannot cancel an acquired swapchain. Discard the unsubmitted tail,
    // then present the last explicit screenshot prefix (or initialized black).
    try {impl->RetirePresentation(frame);} catch(...) {impl->fatal=true;throw;}
    if(impl->shaderCommands) impl->shaderCommands->Reclaim();
    impl->canvases.Reclaim();
}
void Renderer::EndFrame() {
    auto& frame=impl->Recording();
    impl->Prepare(frame);
#ifdef ENGINE_SPRITE_PROFILE
    impl->ProfileMemory(frame);
#endif
    auto state=impl->EncodingState(frame);
    impl->AcquirePresentation(frame);
#ifdef ENGINE_SPRITE_PROFILE
    profile::Timer acquireTimer(profile::Phase::Acquire);
#endif
    auto* command=std::exchange(frame.presentCommand,nullptr);
    if(!command) command=SDL_AcquireGPUCommandBuffer(impl->device);
#ifdef ENGINE_SPRITE_PROFILE
    acquireTimer.Stop();
#endif
    if(!command) throw std::runtime_error(std::string("Renderer command buffer failed: ")+SDL_GetError());
    try {
        impl->Encode(frame,command,frame.mirrored ? frame.mirror:frame.swapchain,state);
        // Blit reads the newly encoded version, not the last prefix version.
        frame.windowInitialized=state.windowInitialized;
        if(frame.mirrored) impl->BlitPresentation(frame,command);
        auto* submitted=std::exchange(command,nullptr);
#ifdef ENGINE_SPRITE_PROFILE
        profile::Timer submitTimer(profile::Phase::Submit);
#endif
        frame.fence=SDL_SubmitGPUCommandBufferAndAcquireFence(submitted);
#ifdef ENGINE_SPRITE_PROFILE
        submitTimer.Stop();
#endif
        if(!frame.fence) throw std::runtime_error(std::string("Renderer submission failed: ")+SDL_GetError());
        impl->CommitEncoding(frame,submitted,state);
        frame.submitted=Clock::now();++impl->stats.submittedFrames;++impl->stats.inFlight;
        impl->current=noFrame;
    } catch(...) {
        impl->fatal=true;impl->current=noFrame;
        if(command) {
            if(frame.swapchain) SDL_SubmitGPUCommandBuffer(command);
            else SDL_CancelGPUCommandBuffer(command);
        }
        throw; // Keep pins until terminal Shutdown drains unknown/partial work.
    }
}
std::optional<std::string> Renderer::GetCanvasPNG(Canvas::Handle handle) {
    impl->Check();
    auto source=impl->canvases.Acquire(handle); // Program misuse always throws, never turns into nil.
    const bool inside=impl->current!=noFrame;
    if(source->info.window && !inside) throw std::logic_error("WindowCanvas PNG requires a current presentable frame");
    Impl::Frame scratch;
    auto& frame=inside ? impl->frames[impl->current]:scratch;
    auto failure=[&](const std::exception& error) {
        core::runtime::Log::Error(std::string("Graphic.GetCanvasPNG: ")+error.what());
    };
    try {if(inside) impl->AcquirePresentation(frame);}
    catch(const std::exception& error) {failure(error);return {};}
    if(source->info.window && (!frame.swapchain || (SDL_GetWindowFlags(impl->window) & SDL_WINDOW_MINIMIZED)))
        return {}; // A legal window readback has no presentable image this frame.
    SDL_GPUCommandBuffer* command{};
    bool submitted{};
    try {
        if(inside) impl->EnsureMirror(frame);
        const auto width=source->info.window ? frame.mirrorWidth:source->info.width;
        const auto height=source->info.window ? frame.mirrorHeight:source->info.height;
        const auto format=source->info.window ? frame.mirrorFormat:SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        impl->capture=std::make_unique<Readback::Request>(impl->device,width,height,format,source->info.window,
            Readback::Config{impl->config.maxReadbackBytes,impl->config.maxPngBytes});
        impl->captureSource=source;
        impl->Prepare(frame);
        auto state=impl->EncodingState(frame,source.get());
#ifdef ENGINE_SPRITE_PROFILE
        profile::Timer acquireTimer(profile::Phase::Acquire);
#endif
        command=SDL_AcquireGPUCommandBuffer(impl->device);
#ifdef ENGINE_SPRITE_PROFILE
        acquireTimer.Stop();
#endif
        if(!command) throw std::runtime_error(std::string("Readback command buffer failed: ")+SDL_GetError());
        impl->Encode(frame,command,frame.mirrored ? frame.mirror:nullptr,state,source.get());
        impl->capture->Encode(command,source->info.window ? frame.mirror:source->texture);
        auto* encoded=std::exchange(command,nullptr);submitted=true;
#ifdef ENGINE_SPRITE_PROFILE
        profile::Timer submitTimer(profile::Phase::Submit);
#endif
        impl->captureFence=SDL_SubmitGPUCommandBufferAndAcquireFence(encoded);
#ifdef ENGINE_SPRITE_PROFILE
        submitTimer.Stop();
#endif
        if(!impl->captureFence) throw std::runtime_error(std::string("Readback submission failed: ")+SDL_GetError());
        impl->CommitEncoding(frame,encoded,state);++impl->stats.readbackSubmissions;
        const auto deadline=Clock::now()+std::chrono::milliseconds(impl->config.fenceTimeoutMs);
        while(!SDL_QueryGPUFence(impl->device,impl->captureFence)) {
            if(Clock::now()>=deadline) throw std::runtime_error("Readback GPU fence timed out");
            SDL_Delay(1); // Deliberate synchronous read; never reenter Lua/input/game update.
        }
        SDL_ReleaseGPUFence(impl->device,impl->captureFence);impl->captureFence=nullptr;
        submitted=false;
        impl->ClearCommands(frame); // Explicit barrier; future Layers cannot move before this capture.
        auto bytes=impl->capture->PNG();
        impl->capture.reset();impl->captureSource.reset();++impl->stats.readbacks;
        return bytes;
    } catch(const std::exception& error) {
        if(command || submitted) {
            impl->fatal=true;
            if(command) SDL_CancelGPUCommandBuffer(command); // Prefix CB never owns a swapchain.
            // Do not release captures or command pins until terminal device drain.
        } else {impl->capture.reset();impl->captureSource.reset();}
        failure(error);return {};
    }
}
Stats Renderer::GetStats() const {
    impl->Check(); auto s=impl->stats;
    if (impl->current!=noFrame) s.queuedCommands=impl->frames[impl->current].commands.size();
    return s;
}
bool Renderer::Shutdown() {
    impl->CheckThread();
    if (impl->closed) return impl->shutdownResult;
    impl->closed=true; impl->current=noFrame;
    for(auto& frame:impl->frames) {
        try {impl->RetirePresentation(frame);} catch(const std::exception& error) {
            impl->shutdownResult=false;core::runtime::Log::Error(error.what());
        }
    }
    impl->shutdownResult=SDL_WaitForGPUIdle(impl->device) && impl->shutdownResult;
    if(impl->captureFence) SDL_ReleaseGPUFence(impl->device,impl->captureFence);
    impl->captureFence=nullptr;impl->capture.reset();impl->captureSource.reset();
    for (auto& frame : impl->frames) {
        if (frame.fence) SDL_ReleaseGPUFence(impl->device,frame.fence);
        frame.fence=nullptr; impl->ClearCommands(frame);
        if(frame.vertexBuffer) SDL_ReleaseGPUBuffer(impl->device,frame.vertexBuffer);
        if(frame.transfer) SDL_ReleaseGPUTransferBuffer(impl->device,frame.transfer);
        if(frame.mirror) SDL_ReleaseGPUTexture(impl->device,frame.mirror);
        frame.mirror=nullptr;
        frame.vertexBuffer=nullptr; frame.transfer=nullptr;
    }
    impl->shaderCommands.reset();
    impl->glyphTextures.reset();impl->glyphMasks.Clear();
    impl->imagePipeline.reset();
    impl->canvases.Shutdown(); impl->stats.inFlight=0;
    return impl->shutdownResult;
}
}
