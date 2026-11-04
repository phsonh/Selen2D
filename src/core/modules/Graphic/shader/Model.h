#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace core::modules::Graphic::Shader {
enum class ValueType { Float, Float2, Float3, Float4, Int, UInt, Matrix4 };
struct Location { std::string file; std::uint32_t line{1}, column{1}; };
enum class DeclarationKind { Params, Builtins, Texture, Sampler };
enum class TextureRole { Color, Depth, Distance };
struct Declaration {
    DeclarationKind kind{};
    std::string name;
    // Byte offset immediately after the declaration's name in Model::source.
    // Generation inserts a register annotation here, never regex-rewrites HLSL.
    std::size_t bindingOffset{};
    Location location;
    TextureRole textureRole{TextureRole::Color};
    // Nonempty only for a derived numeric input. Classify once from the full
    // source declarations; optimization and register order never change this.
    std::string mainTexture;
};
struct Parameter { std::string name; ValueType type{}; Location location; };
struct SamplingCall {
    std::size_t begin{},end{}; // Semantic member-expression range, not arguments.
    std::string texture;
    bool explicitLod{};
};
struct Model {
    std::string source; // DXC-preprocessed text, including source #line records.
    std::vector<Declaration> declarations;
    std::vector<Parameter> parameters;
    bool hasVertex{};
    std::vector<SamplingCall> sampling;
};
// Parses only the documented global declaration surface. Function bodies are
// left to DXC, not interpreted by a second HLSL compiler. Builtin declarations
// are accepted only from the engine's virtual Engine.hlsl source record.
Model Parse(std::string preprocessed, std::string_view rootPath);
std::string Describe(const Location&);
const char* TypeName(ValueType) noexcept;
}
