#include "Program.h"
#include <SDL3/SDL_error.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace core::modules::Graphic::Shader {
namespace detail {
struct ProgramGate {
    SDL_GPUDevice* device;
    const std::thread::id owner{std::this_thread::get_id()};
    bool live{true};
    explicit ProgramGate(SDL_GPUDevice* value) : device(value) {}
    void Check() const {
        if (owner != std::this_thread::get_id())
            throw std::logic_error("Shader program access requires the render owner thread");
        if (!live) throw std::logic_error("Shader program store has shut down");
    }
};
struct ProgramState {
    std::shared_ptr<ProgramGate> gate;
    std::shared_ptr<const Compiled> metadata;
    SDL_GPUShader* vertex{};
    SDL_GPUShader* fragment{};
    SDL_GPUGraphicsPipeline* validation{};
    ProgramState(std::shared_ptr<ProgramGate> owner, std::shared_ptr<const Compiled> compiled)
        : gate(std::move(owner)), metadata(std::move(compiled)) {}
    ~ProgramState() { Release(); }
    void Release() noexcept {
        if (!vertex && !fragment && !validation) return;
        if (gate->owner != std::this_thread::get_id() || !gate->device) std::terminate();
        // SDL owns deferred physical retirement of submitted GPU work. A
        // program's logical lifetime never requires a main-thread GPU wait.
        if (validation) { SDL_ReleaseGPUGraphicsPipeline(gate->device, validation); validation = nullptr; }
        if (fragment) { SDL_ReleaseGPUShader(gate->device, fragment); fragment = nullptr; }
        if (vertex) { SDL_ReleaseGPUShader(gate->device, vertex); vertex = nullptr; }
    }
};
}
namespace {
template<class Binding>
void Dense(const std::vector<Binding>& bindings, const char* label) {
    if (bindings.size() > std::numeric_limits<Uint32>::max())
        throw std::length_error(std::string("Shader ") + label + " count exceeds SDL range");
    std::vector<bool> seen(bindings.size());
    for (const auto& binding : bindings) {
        if (binding.slot >= bindings.size() || seen[binding.slot])
            throw std::invalid_argument(std::string("Shader ") + label + " slots must be unique and contiguous from zero");
        seen[binding.slot] = true;
    }
}
void Validate(const Stage& stage) {
    if (stage.code.empty() || stage.entry.empty() || stage.entry.find('\0') != std::string::npos)
        throw std::invalid_argument("Shader stage requires bytecode and a valid entry point");
    // Compiled is a native boundary too: callers may supply metadata without
    // going through Compiler. Reject before Dense allocates or SDL is called.
    if (stage.textures.size() > 16)
        throw std::length_error("Shader stage texture limit exceeded (16)");
    if (stage.buffers.size() > 4)
        throw std::length_error("Shader stage uniform buffer limit exceeded (4)");
    Dense(stage.textures, "texture");
    Dense(stage.buffers, "uniform buffer");
}
SDL_GPUShader* CreateStage(SDL_GPUDevice* device, const Stage& stage, SDL_GPUShaderStage type) {
    SDL_GPUShaderCreateInfo create{};
    create.code_size = stage.code.size();
    create.code = stage.code.data();
    create.entrypoint = stage.entry.c_str();
    create.format = SDL_GPU_SHADERFORMAT_DXIL;
    create.stage = type;
    create.num_samplers = static_cast<Uint32>(stage.textures.size());
    create.num_uniform_buffers = static_cast<Uint32>(stage.buffers.size());
    auto* shader = SDL_CreateGPUShader(device, &create);
    if (!shader) throw std::runtime_error(std::string(type == SDL_GPU_SHADERSTAGE_VERTEX ?
        "Shader vertex creation failed: " : "Shader fragment creation failed: ") + SDL_GetError());
    return shader;
}
SDL_GPUGraphicsPipeline* ValidatePipeline(SDL_GPUDevice* device, SDL_GPUShader* vertex, SDL_GPUShader* fragment) {
    struct Vertex { float position[3], uv[2], color[4]; };
    static_assert(sizeof(Vertex) == 36 && offsetof(Vertex, uv) == 12 && offsetof(Vertex, color) == 20);
    const SDL_GPUVertexBufferDescription buffer{0, sizeof(Vertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0};
    // The compiler's internal adapter maps the public ABI onto SDL's D3D12
    // TEXCOORD0/1/2 input convention. No native D3D API is called here.
    const std::array<SDL_GPUVertexAttribute, 3> attributes{{
        {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(Vertex, position)},
        {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(Vertex, uv)},
        {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(Vertex, color)},
    }};
    SDL_GPUColorTargetDescription target{};
    target.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    SDL_GPUGraphicsPipelineCreateInfo create{};
    create.vertex_shader = vertex;
    create.fragment_shader = fragment;
    create.vertex_input_state = {&buffer, 1, attributes.data(), static_cast<Uint32>(attributes.size())};
    create.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    create.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    create.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    create.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    create.rasterizer_state.enable_depth_clip = true;
    create.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    create.target_info.color_target_descriptions = &target;
    create.target_info.num_color_targets = 1;
    create.target_info.has_depth_stencil_target = false;
    auto* pipeline = SDL_CreateGPUGraphicsPipeline(device, &create);
    if (!pipeline) throw std::runtime_error(std::string("Shader baseline pipeline validation failed: ") + SDL_GetError());
    return pipeline;
}
}
struct ProgramStore::Impl {
    std::shared_ptr<detail::ProgramGate> gate;
    std::size_t capacity;
    std::vector<std::weak_ptr<detail::ProgramState>> programs;
    Impl(SDL_GPUDevice* device, std::size_t limit)
        : gate(std::make_shared<detail::ProgramGate>(device)), capacity(limit) {
        if (!device || !limit) throw std::invalid_argument("Shader program store requires a device and nonzero capacity");
        programs.reserve(limit);
    }
};
Program::Program(std::shared_ptr<detail::ProgramState> state) : state_(std::move(state)) {}
Program::~Program() = default;
SDL_GPUShader* Program::Vertex() const { state_->gate->Check(); return state_->vertex; }
SDL_GPUShader* Program::Fragment() const { state_->gate->Check(); return state_->fragment; }
const Compiled& Program::Metadata() const { state_->gate->Check(); return *state_->metadata; }
ProgramStore::ProgramStore(SDL_GPUDevice* device, std::size_t capacity)
    : impl_(std::make_unique<Impl>(device, capacity)) {}
ProgramStore::~ProgramStore() { Shutdown(); }
std::shared_ptr<const Program> ProgramStore::Create(std::shared_ptr<const Compiled> compiled) {
    impl_->gate->Check();
    if (!compiled) throw std::invalid_argument("Shader program requires compiled metadata");
    if (compiled->format != BytecodeFormat::Dxil ||
        !(SDL_GetGPUShaderFormats(impl_->gate->device) & SDL_GPU_SHADERFORMAT_DXIL))
        throw std::runtime_error("Shader DXIL bytecode is not supported by the active SDL_GPU device");
    Validate(compiled->vertex);
    Validate(compiled->fragment);
    std::erase_if(impl_->programs, [](const auto& program) { return program.expired(); });
    if (impl_->programs.size() >= impl_->capacity)
        throw std::length_error("Shader program store capacity exceeded");
    auto state = std::make_shared<detail::ProgramState>(impl_->gate, std::move(compiled));
    state->vertex = CreateStage(impl_->gate->device, state->metadata->vertex, SDL_GPU_SHADERSTAGE_VERTEX);
    state->fragment = CreateStage(impl_->gate->device, state->metadata->fragment, SDL_GPU_SHADERSTAGE_FRAGMENT);
    state->validation = ValidatePipeline(impl_->gate->device, state->vertex, state->fragment);
    auto program = std::shared_ptr<const Program>(new Program(state));
    // Capacity was reserved at construction. Publication cannot allocate after
    // successful GPU setup; any earlier exception releases the whole state.
    impl_->programs.push_back(state);
    return program;
}
void ProgramStore::Shutdown() noexcept {
    if (!impl_) return;
    if (impl_->gate->owner != std::this_thread::get_id()) std::terminate();
    if (!impl_->gate->live) return;
    impl_->gate->live = false;
    for (const auto& weak : impl_->programs) if (auto state = weak.lock()) state->Release();
    impl_->programs.clear();
    impl_->gate->device = nullptr;
}
}
