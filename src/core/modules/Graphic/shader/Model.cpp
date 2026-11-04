#include "Model.h"
#include <algorithm>
#include <limits>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace core::modules::Graphic::Shader {
std::string Describe(const Location& value) {
    return value.file + ":" + std::to_string(value.line) + ":" + std::to_string(value.column);
}
const char* TypeName(ValueType value) noexcept {
    switch(value) {
    case ValueType::Float: return "float";
    case ValueType::Float2: return "float2";
    case ValueType::Float3: return "float3";
    case ValueType::Float4: return "float4";
    case ValueType::Int: return "int";
    case ValueType::UInt: return "uint";
    case ValueType::Matrix4: return "float4x4";
    }
    return "unknown";
}
namespace {
constexpr std::size_t MaxTokens = 262144; // Includes the terminal token.
constexpr std::size_t MaxDeclarations = 4096;
constexpr std::size_t MaxParameters = 4096;
bool Letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool Digit(char c) { return c >= '0' && c <= '9'; }
bool Space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v'; }
enum class TokenKind { Identifier, Number, Literal, Punctuation, End };
struct Position { std::size_t file{}; std::uint32_t line{1}, column{1}; };
struct Token { TokenKind kind{}; std::size_t begin{}, end{}; Position where; };

// Token offsets refer to the original preprocessed text. Source records are
// interned so a large function body does not copy its filename for every token.
class Lexer {
public:
    Lexer(std::string_view text, std::string_view root) : source(text) { files.emplace_back(root); }
    std::vector<Token> Run() {
        std::vector<Token> result;
        while(cursor < source.size()) {
            const char c = source[cursor];
            if(Space(c) || c == '\n') { Advance(); continue; }
            if(c == '/' && Peek(1) == '/') {
                while(cursor < source.size() && source[cursor] != '\n') Advance();
                continue;
            }
            if(c == '/' && Peek(1) == '*') {
                const auto start = position;
                const bool leading = lineWhitespace;
                Advance(); Advance();
                while(cursor < source.size() && !(source[cursor] == '*' && Peek(1) == '/')) Advance();
                if(cursor == source.size()) Fail(start, "unterminated comment");
                Advance(); Advance();
                lineWhitespace = position.line != start.line || leading;
                continue;
            }
            if(c == '#' && lineWhitespace) { Directive(); continue; }
            const auto begin = cursor;
            const auto where = position;
            TokenKind kind = TokenKind::Punctuation;
            if(Letter(c)) {
                kind = TokenKind::Identifier;
                do { Advance(); } while(cursor < source.size() && (Letter(source[cursor]) || Digit(source[cursor])));
            } else if(Digit(c)) {
                kind = TokenKind::Number;
                do { Advance(); } while(cursor < source.size() && (Letter(source[cursor]) || Digit(source[cursor]) || source[cursor] == '.'));
            } else if(c == '"' || c == '\'') {
                kind = TokenKind::Literal;
                Advance();
                while(cursor < source.size() && source[cursor] != c) {
                    if(source[cursor] == '\n') Fail(where, "unterminated string literal");
                    if(source[cursor] == '\\') {
                        Advance();
                        if(cursor == source.size()) Fail(where, "unterminated string literal");
                    }
                    Advance();
                }
                if(cursor == source.size()) Fail(where, "unterminated string literal");
                Advance();
            } else Advance();
            if(result.size() >= MaxTokens - 1) Fail(where, "HLSL token limit exceeded (262144 including end marker)");
            result.push_back({kind, begin, cursor, where});
        }
        result.push_back({TokenKind::End, cursor, cursor, position});
        return result;
    }
    Location At(Position p) const { return {files.at(p.file), p.line, p.column}; }
private:
    std::string_view source;
    std::vector<std::string> files;
    std::size_t cursor{};
    Position position;
    bool lineWhitespace{true};
    char Peek(std::size_t n) const { return cursor + n < source.size() ? source[cursor + n] : '\0'; }
    [[noreturn]] void Fail(Position p, std::string_view reason) const {
        throw std::invalid_argument(Describe(At(p)) + ": " + std::string(reason));
    }
    void Advance() {
        const char c = source[cursor++];
        if(c == '\n') {
            if(position.line == std::numeric_limits<std::uint32_t>::max()) Fail(position, "source line overflow");
            ++position.line; position.column = 1; lineWhitespace = true;
        } else {
            if(position.column == std::numeric_limits<std::uint32_t>::max()) Fail(position, "source column overflow");
            ++position.column;
            if(!Space(c)) lineWhitespace = false;
        }
    }
    void Directive() {
        const auto where = position;
        auto end = source.find('\n', cursor);
        if(end == std::string_view::npos) end = source.size();
        auto text = source.substr(cursor + 1, end - cursor - 1);
        std::size_t i{};
        const auto spaces = [&] { while(i < text.size() && Space(text[i])) ++i; };
        spaces();
        if(text.substr(i, 4) == "line" && (i + 4 == text.size() || Space(text[i + 4]))) { i += 4; spaces(); }
        if(i == text.size() || !Digit(text[i])) Fail(where, "only source #line records are supported after HLSL preprocessing");
        std::uint64_t line{};
        while(i < text.size() && Digit(text[i])) {
            line = line * 10 + static_cast<unsigned>(text[i++] - '0');
            if(line > std::numeric_limits<std::uint32_t>::max()) Fail(where, "invalid #line number");
        }
        if(line == 0) Fail(where, "invalid #line number");
        spaces();
        auto file = position.file;
        if(i < text.size() && text[i] == '"') {
            ++i;
            std::string name;
            while(i < text.size() && text[i] != '"') {
                char c = text[i++];
                if(c == '\\' && i < text.size() && (text[i] == '\\' || text[i] == '"')) c = text[i++];
                name.push_back(c);
            }
            if(i == text.size() || name.empty()) Fail(where, "invalid #line filename");
            ++i; spaces();
            const auto found = std::find(files.begin(), files.end(), name);
            if(found == files.end()) { file = files.size(); files.push_back(std::move(name)); }
            else file = static_cast<std::size_t>(found - files.begin());
        }
        // DXC emits #line; also accept the standard preprocessor line marker
        // spelling '# 1 "file" 1'. Flags do not alter the VFS trust decision.
        while(i < text.size()) {
            if(!Digit(text[i])) Fail(where, "invalid source #line record");
            while(i < text.size() && Digit(text[i])) ++i;
            spaces();
        }
        while(cursor < end) Advance();
        if(cursor < source.size()) Advance();
        position = {file, static_cast<std::uint32_t>(line), 1};
        lineWhitespace = true;
    }
};

std::optional<ValueType> ParameterType(std::string_view name) {
    for(const auto type : {ValueType::Float, ValueType::Float2, ValueType::Float3, ValueType::Float4,
                          ValueType::Int, ValueType::UInt, ValueType::Matrix4})
        if(name == TypeName(type)) return type;
    return std::nullopt;
}
bool ResourceType(std::string_view name) {
    return name.starts_with("Texture") || name.starts_with("RWTexture") || name.starts_with("RasterizerOrdered") ||
        name == "SamplerState" || name == "SamplerComparisonState" || name == "sampler" ||
        name == "ConstantBuffer" || name == "Buffer" || name == "RWBuffer" || name == "StructuredBuffer" ||
        name == "RWStructuredBuffer" || name == "ByteAddressBuffer" || name == "RWByteAddressBuffer" ||
        name == "AppendStructuredBuffer" || name == "ConsumeStructuredBuffer" || name == "RaytracingAccelerationStructure";
}

class Parser {
public:
    Parser(std::string source, std::string_view root) : model{std::move(source), {}, {}, false}, lexer(model.source, root), tokens(lexer.Run()) {}
    Model Run() {
        while(Current().kind != TokenKind::End) {
            if(Eat(";")) continue;
            if(Is("cbuffer")) { Buffer(); continue; }
            if(Is("struct")) { Structure(); continue; }
            if(Is("Texture2D") || Is("SamplerState")) { Resource(); continue; }
            General();
        }
        if(!publicVertex || !publicPixel) Error(Current(), "include \"Engine.hlsl\" to use the public VSInput/PSInput ABI");
        if(!pixelDefined) Error(Current(), "missing definition: float4 PSMain(PSInput input) : SV_Target0");
        for(const auto& declaration : model.declarations) {
            if(declaration.kind == DeclarationKind::Texture && !samplers.contains(declaration.name + "Sampler"))
                throw std::invalid_argument(Describe(declaration.location) + ": Texture2D '" + declaration.name + "' requires SamplerState '" + declaration.name + "Sampler'");
            if(declaration.kind == DeclarationKind::Sampler) {
                constexpr std::string_view suffix = "Sampler";
                const std::string_view name = declaration.name;
                if(!name.ends_with(suffix) || !textures.contains(std::string(name.substr(0, name.size() - suffix.size()))))
                    throw std::invalid_argument(Describe(declaration.location) + ": SamplerState '" + declaration.name + "' has no matching Texture2D");
            }
        }
        std::vector<Declaration*> texturesByLength;
        for(auto& declaration:model.declarations)
            if(declaration.kind==DeclarationKind::Texture) texturesByLength.push_back(&declaration);
        std::sort(texturesByLength.begin(),texturesByLength.end(),[](const auto* a,const auto* b) {
            return a->name.size()==b->name.size() ? a->name<b->name : a->name.size()<b->name.size();
        });
        for(auto* declaration:texturesByLength) {
            for(const auto& [suffix,role]:{std::pair<std::string_view,TextureRole>{"_depth",TextureRole::Depth},
                    {"_distance",TextureRole::Distance}}) {
                if(!declaration->name.ends_with(suffix)) continue;
                const auto base=declaration->name.substr(0,declaration->name.size()-suffix.size());
                const auto found=std::find_if(texturesByLength.begin(),texturesByLength.end(),[&](const auto* candidate) {
                    return candidate->name==base && candidate->mainTexture.empty();
                });
                if(found!=texturesByLength.end()) {declaration->mainTexture=base; declaration->textureRole=role;}
            }
        }
        return std::move(model);
    }
private:
    Model model;
    Lexer lexer;
    std::vector<Token> tokens;
    std::size_t cursor{};
    bool publicVertex{}, publicPixel{}, pixelDefined{};
    std::unordered_map<std::string, bool> globals; // true only for overloadable functions.
    std::unordered_set<std::string> textures, samplers;
    const Token& Current() const { return tokens[cursor]; }
    std::string_view Text(const Token& token) const { return std::string_view(model.source).substr(token.begin, token.end - token.begin); }
    bool Is(std::string_view value) const { return Text(Current()) == value; }
    bool Eat(std::string_view value) { if(!Is(value)) return false; ++cursor; return true; }
    bool Trusted(const Token& token) const { return lexer.At(token.where).file == "Engine.hlsl"; }
    [[noreturn]] void Error(const Token& token, std::string_view reason) const {
        throw std::invalid_argument(Describe(lexer.At(token.where)) + ": " + std::string(reason));
    }
    void Expect(std::string_view value) { if(!Eat(value)) Error(Current(), "expected '" + std::string(value) + "'"); }
    Token Identifier() {
        if(Current().kind != TokenKind::Identifier) Error(Current(), "expected an HLSL identifier");
        return tokens[cursor++];
    }
    void Name(const Token& token, bool function = false) {
        const auto name = Text(token);
        if(name.starts_with("Engine") && !Trusted(token)) Error(token, "the Engine identifier prefix is reserved");
        const auto [found, added] = globals.emplace(std::string(name), function);
        if(!added && !(function && found->second)) Error(token, "duplicate or conflicting global name '" + std::string(name) + "'");
    }
    void Annotation() const {
        if(Is(":")) Error(Current(), "explicit register, packoffset and declaration annotations are not supported");
    }
    void Declare(DeclarationKind kind, const Token& name) {
        if(model.declarations.size() >= MaxDeclarations) Error(name, "HLSL resource declaration limit exceeded (4096)");
        model.declarations.push_back({kind, std::string(Text(name)), name.end, lexer.At(name.where)});
    }
    std::size_t Balanced() {
        const auto begin = cursor;
        if(!Is("(") && !Is("[") && !Is("{")) Error(Current(), "expected a balanced HLSL block");
        std::vector<char> stack;
        do {
            const auto& token = Current();
            const auto value = Text(token);
            if(token.kind == TokenKind::End) Error(tokens[begin], "unterminated HLSL block");
            if(value == "(" || value == "[" || value == "{") {
                if(stack.size() >= 1024) Error(token, "HLSL nesting limit exceeded");
                stack.push_back(value == "(" ? ')' : value == "[" ? ']' : '}');
            } else if(value == ")" || value == "]" || value == "}") {
                if(stack.empty() || stack.back() != value.front()) Error(token, "mismatched HLSL delimiter");
                stack.pop_back();
            }
            ++cursor;
        } while(!stack.empty());
        return cursor - 1;
    }
    void Buffer() {
        ++cursor;
        const auto name = Identifier();
        const bool builtin = Text(name) == "EngineBuiltins" && Trusted(name);
        if(!builtin && Text(name) != "Params") Error(name, "only the user cbuffer Params is supported");
        Name(name);
        Annotation();
        Declare(builtin ? DeclarationKind::Builtins : DeclarationKind::Params, name);
        if(builtin) { if(!Is("{")) Error(Current(), "expected '{'"); Balanced(); Eat(";"); return; }
        Expect("{");
        while(!Eat("}")) {
            if(Current().kind == TokenKind::End) Error(name, "unterminated Params buffer");
            if(Eat(";")) continue;
            const bool columnMajor = Eat("column_major");
            const auto typeToken = Identifier();
            if(Text(typeToken) == "row_major") Error(typeToken, "Params matrices must use column_major float4x4");
            const auto type = ParameterType(Text(typeToken));
            if(!type || (columnMajor && *type != ValueType::Matrix4))
                Error(typeToken, "unsupported Params type; use float/float2/float3/float4/int/uint/float4x4");
            do {
                const auto field = Identifier();
                if(model.parameters.size() >= MaxParameters) Error(field, "HLSL Params field limit exceeded (4096)");
                Name(field);
                model.parameters.push_back({std::string(Text(field)), *type, lexer.At(field.where)});
                Annotation();
                if(Is("[")) Error(Current(), "Params arrays are not supported");
                if(Is("=")) Error(Current(), "Params defaults are not supported; supply values in Graphic.Render");
            } while(Eat(","));
            Expect(";");
        }
        Eat(";");
    }
    void Structure() {
        ++cursor;
        const auto name = Identifier();
        if(!Trusted(name) && (Text(name) == "VSInput" || Text(name) == "PSInput"))
            Error(name, "VSInput/PSInput are defined by the engine; include \"Engine.hlsl\" instead");
        Name(name);
        if(Trusted(name)) {
            if(Text(name) == "VSInput") publicVertex = true;
            if(Text(name) == "PSInput") publicPixel = true;
        }
        if(Eat(";")) return;
        Expect("{"); --cursor; Balanced();
        Expect(";"); // Struct instances are separate declarations, not uniforms.
    }
    void Resource() {
        const bool texture = Eat("Texture2D");
        if(!texture) Expect("SamplerState");
        if(Eat("<")) {
            if(!texture || !Eat("float4")) Error(Current(), "Texture2D supports only its default float4 element type");
            Expect(">");
        }
        do {
            const auto name = Identifier();
            Name(name);
            Annotation();
            if(Is("[")) Error(Current(), "resource arrays are not supported");
            if(Is("=") || Is("{")) Error(Current(), "resource initializers are not supported; samplers are engine-managed");
            Declare(texture ? DeclarationKind::Texture : DeclarationKind::Sampler, name);
            (texture ? textures : samplers).insert(std::string(Text(name)));
        } while(Eat(","));
        Expect(";");
    }
    void General() {
        bool isStatic{}, isConst{}, columnMajor{};
        while(Is("static") || Is("const") || Is("inline") || Is("precise") || Is("column_major")) {
            if(Is("static")) isStatic = true;
            if(Is("const")) isConst = true;
            if(Is("column_major")) columnMajor = true;
            ++cursor;
        }
        const auto type = Identifier();
        if(ResourceType(Text(type)) || Text(type) == "tbuffer" || Text(type) == "groupshared" || Text(type) == "uniform")
            Error(type, "unsupported global resource or uniform declaration");
        if(Text(type) == "row_major") Error(type, "row_major is not supported by the Shader ABI");
        if(columnMajor && Text(type) != "float4x4") Error(type, "column_major requires float4x4");
        // Helper return types may use HLSL's vector/matrix templates. The DXC
        // semantic pass remains authoritative for these non-parameter types.
        bool generic = false;
        if(Eat("<")) {
            generic = true;
            unsigned depth = 1;
            while(depth) {
                if(Current().kind == TokenKind::End) Error(type, "unterminated HLSL type arguments");
                if(Eat("<")) { if(++depth > 1024) Error(Current(), "HLSL nesting limit exceeded"); }
                else if(Eat(">")) --depth;
                else ++cursor;
            }
        }
        const auto name = Identifier();
        if(Is("(")) { Function(type, name, generic); return; }
        if(!isStatic || !isConst) Error(name, "bare global uniforms are not supported; declare values in cbuffer Params");
        Constant(name);
    }
    void Function(const Token& type, const Token& name, bool generic) {
        Name(name, true);
        const auto argumentBegin = cursor + 1;
        const auto argumentEnd = Balanced();
        const bool pixel = Text(name) == "PSMain", vertex = Text(name) == "VSMain";
        if(pixel || vertex) {
            const auto wantedReturn = pixel ? "float4" : "PSInput";
            const auto wantedInput = pixel ? "PSInput" : "VSInput";
            auto first = argumentBegin;
            if(first < argumentEnd && Text(tokens[first]) == "in") ++first;
            if(generic || Text(type) != wantedReturn || argumentEnd - first != 2 ||
               Text(tokens[first]) != wantedInput || tokens[first + 1].kind != TokenKind::Identifier)
                Error(name, pixel ? "expected float4 PSMain(PSInput input) : SV_Target0" : "expected PSInput VSMain(VSInput input)");
            if(pixel) {
                Expect(":");
                if(!Eat("SV_Target0")) Error(Current(), "PSMain must return SV_Target0");
            } else if(Is(":")) Error(Current(), "VSMain returns the public PSInput structure without a return semantic");
        } else {
            // Ordinary helper functions can have HLSL parameter/return
            // semantics. Only the fixed public entry signatures are modeled.
            if(Eat(":")) Identifier();
        }
        if(Eat(";")) return; // A forward declaration is not an entry definition.
        if(!Is("{")) Error(Current(), "expected HLSL function body or prototype");
        if(pixel) {
            if(pixelDefined) Error(name, "duplicate PSMain definition");
            pixelDefined = true;
        }
        if(vertex) {
            if(model.hasVertex) Error(name, "duplicate VSMain definition");
            model.hasVertex = true;
        }
        Balanced();
    }
    void Constant(Token name) {
        for(;;) {
            Name(name);
            while(Is("[")) Balanced();
            Annotation();
            Expect("=");
            bool expression = false;
            while(!Is(",") && !Is(";")) {
                if(Current().kind == TokenKind::End) Error(name, "unterminated static const declaration");
                if(Is("(") || Is("{") || Is("[")) Balanced();
                else {
                    if(Is("}") || Is(")") || Is("]")) Error(Current(), "mismatched constant initializer delimiter");
                    ++cursor;
                }
                expression = true;
            }
            if(!expression) Error(Current(), "missing static const initializer");
            if(Eat(";")) return;
            Expect(","); name = Identifier();
        }
    }
};
}
Model Parse(std::string preprocessed, std::string_view rootPath) {
    return Parser(std::move(preprocessed), rootPath).Run();
}
}
