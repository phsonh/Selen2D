#include "ShaderParams.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace lua_bind::modules::Graphic::ShaderParams {
namespace {
namespace S = core::modules::Graphic::Shader;
struct Stack {
    lua_State* lua;
    int top;
    ~Stack() { lua_settop(lua, top); }
};
struct Context {
    lua_State* lua;
    const S::Compiled& shader;
    [[noreturn]] void Error(std::string_view field, std::string_view reason) const {
        const auto display = [](std::string_view text, std::size_t maximum) {
            return std::string(text.substr(0, maximum)) + (text.size() > maximum ? "..." : "");
        };
        throw std::invalid_argument("Shader '" + display(shader.path, 1024) + "' params" +
            (field.empty() ? std::string{} : "." + display(field, 256)) + ": " + display(reason, 2048));
    }
    void Table(int index, std::string_view field) const {
        if (lua_type(lua, index) != LUA_TTABLE) Error(field, "expected a plain Lua table");
        if (lua_getmetatable(lua, index)) {
            lua_pop(lua, 1);
            Error(field, "metatables are not permitted");
        }
    }
    void Tuple(int index, int count, std::string_view field) const {
        Table(index, field);
        int actual{};
        lua_pushnil(lua);
        while (lua_next(lua, index)) {
            if (lua_type(lua, -2) != LUA_TNUMBER) Error(field, "expected consecutive numeric keys only");
            const auto key = lua_tonumber(lua, -2);
            if (!std::isfinite(key) || key < 1 || key > count || std::floor(key) != key)
                Error(field, "tuple has an extra or invalid numeric key");
            ++actual;
            lua_pop(lua, 1);
        }
        if (actual != count) Error(field, "tuple is incomplete; expected exactly " + std::to_string(count) + " values");
    }
    double Number(int index, std::string_view field) const {
        if (lua_type(lua, index) != LUA_TNUMBER) Error(field, "expected a number (numeric strings are not accepted)");
        const auto value = lua_tonumber(lua, index);
        if (!std::isfinite(value)) Error(field, "number must be finite");
        return value;
    }
    float Float(int index, std::string_view field) const {
        const auto value = Number(index, field);
        if (value < -std::numeric_limits<float>::max() || value > std::numeric_limits<float>::max())
            Error(field, "number is outside finite float32 range");
        return static_cast<float>(value);
    }
};
struct Value {
    S::ValueType type{};
    bool required{}, provided{};
    std::array<std::uint8_t, 64> bytes{};
};
std::size_t Bytes(S::ValueType type) {
    switch (type) {
    case S::ValueType::Float: case S::ValueType::Int: case S::ValueType::UInt: return 4;
    case S::ValueType::Float2: return 8;
    case S::ValueType::Float3: return 12;
    case S::ValueType::Float4: return 16;
    case S::ValueType::Matrix4: return 64;
    }
    throw std::invalid_argument("invalid reflected Shader parameter type");
}
template<class T> void Write(Value& value, std::size_t offset, T number) {
    static_assert(sizeof(T) == 4);
    std::memcpy(value.bytes.data() + offset, &number, sizeof(number));
}
void Read(const Context& context, int index, std::string_view name, Value& value) {
    if (value.type == S::ValueType::Float) Write(value, 0, context.Float(index, name));
    else if (value.type == S::ValueType::Int || value.type == S::ValueType::UInt) {
        const auto number = context.Number(index, name);
        const bool signedValue = value.type == S::ValueType::Int;
        const double minimum = signedValue ? double(std::numeric_limits<std::int32_t>::min()) : 0.;
        const double maximum = signedValue ? double(std::numeric_limits<std::int32_t>::max()) : double(std::numeric_limits<std::uint32_t>::max());
        if (std::floor(number) != number || number < minimum || number > maximum)
            context.Error(name, signedValue ? "expected an int32 integer" : "expected a uint32 integer");
        if (signedValue) Write(value, 0, static_cast<std::int32_t>(number));
        else Write(value, 0, static_cast<std::uint32_t>(number));
    } else if (value.type == S::ValueType::Matrix4) {
        context.Tuple(index, 4, name);
        for (int row = 0; row < 4; ++row) {
            lua_rawgeti(context.lua, index, row + 1);
            const auto rowIndex = lua_gettop(context.lua);
            const auto rowName = std::string(name) + "[" + std::to_string(row + 1) + "]";
            context.Tuple(rowIndex, 4, rowName);
            for (int column = 0; column < 4; ++column) {
                lua_rawgeti(context.lua, rowIndex, column + 1);
                const auto component = rowName + "[" + std::to_string(column + 1) + "]";
                // Lua lists mathematical rows; HLSL float4x4 is column-major.
                Write(value, std::size_t(column * 4 + row) * 4, context.Float(-1, component));
                lua_pop(context.lua, 1);
            }
            lua_pop(context.lua, 1);
        }
    } else {
        const auto count = static_cast<int>(Bytes(value.type) / 4);
        context.Tuple(index, count, name);
        for (int i = 0; i < count; ++i) {
            lua_rawgeti(context.lua, index, i + 1);
            const auto component = std::string(name) + "[" + std::to_string(i + 1) + "]";
            Write(value, std::size_t(i) * 4, context.Float(-1, component));
            lua_pop(context.lua, 1);
        }
    }
    value.provided = true;
}
struct Texture {
    std::string base;
    TexturePart part{TexturePart::Color};
    std::size_t root{};
};
struct StringHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view value) const noexcept { return std::hash<std::string_view>{}(value); }
};
using Values = std::unordered_map<std::string, Value, StringHash, std::equal_to<>>;
using Textures = std::unordered_map<std::string, Texture, StringHash, std::equal_to<>>;
void InspectBuffers(const Context& context, const S::Stage& stage, Values& values) {
    bool params{};
    std::unordered_set<std::uint32_t> slots;
    for (const auto& buffer : stage.buffers) {
        if (!slots.insert(buffer.slot).second || buffer.slot >= 4)
            context.Error({}, "invalid reflected uniform slots");
        if (buffer.name != "Params") continue;
        if (params) context.Error({}, "duplicate reflected Params buffer");
        params = true;
        if (!buffer.size || buffer.size > 64 * 1024) context.Error({}, "invalid reflected Params buffer size");
        std::unordered_set<std::string> seen;
        for (const auto& field : buffer.fields) {
            const auto found = values.find(field.name);
            if (found == values.end() || found->second.type != field.type || !seen.insert(field.name).second)
                context.Error(field.name, "reflection does not match the declared parameter");
            const auto size = Bytes(field.type);
            if (field.offset > buffer.size || field.size > buffer.size - field.offset || field.size != size)
                context.Error(field.name, "invalid reflected parameter extent");
            found->second.required = found->second.required || field.used;
        }
    }
}
void InspectTextures(const Context& context, const S::Stage& stage, const Textures& textures,
    Snapshot& result, Stage& output) {
    std::unordered_set<std::uint32_t> slots;
    std::unordered_set<std::string> names;
    for (const auto& binding : stage.textures) {
        if (binding.slot >= 16 || !slots.insert(binding.slot).second || !names.insert(binding.name).second)
            context.Error(binding.name, "invalid reflected texture binding");
        if (binding.engine) {
            if (binding.name != "SolongoFogDistance") context.Error(binding.name, "unknown internal texture binding");
            continue; // Renderer supplies history; Lua cannot fill a reserved field.
        }
        const auto found = textures.find(binding.name);
        if (found == textures.end()) context.Error(binding.name, "invalid reflected texture binding");
        const auto& texture = found->second;
        result.textures[texture.root].required = true;
        output.textures.push_back({binding.slot, texture.root, texture.part});
    }
    std::sort(output.textures.begin(), output.textures.end(), [](const auto& a, const auto& b) { return a.slot < b.slot; });
}
void PackBuffers(const S::Stage& stage, const Values& values, Stage& output) {
    for (const auto& buffer : stage.buffers) {
        if (buffer.name != "Params") continue;
        Uniform packed{buffer.slot, std::vector<std::uint8_t>(buffer.size)};
        for (const auto& field : buffer.fields) {
            const auto& value = values.at(field.name);
            if (field.used) std::memcpy(packed.bytes.data() + field.offset, value.bytes.data(), Bytes(field.type));
        }
        output.params.push_back(std::move(packed));
    }
    std::sort(output.params.begin(), output.params.end(), [](const auto& a, const auto& b) { return a.slot < b.slot; });
}
}
Snapshot Pack(lua_State* lua, int paramsIndex, const S::Compiled& shader, const ResolveTexture& resolve) {
    if (!lua) throw std::invalid_argument("Shader params require a Lua state");
    const int originalTop = lua_gettop(lua);
    if (paramsIndex < 0 && paramsIndex > LUA_REGISTRYINDEX) paramsIndex = originalTop + paramsIndex + 1;
    if (paramsIndex < 1 || paramsIndex > originalTop) throw std::invalid_argument("Shader params require a valid stack index");
    // Reserve before creating owning C++ locals. Everything below uses raw reads
    // of existing Lua values; none creates strings, invokes Lua or grows tables.
    if (!lua_checkstack(lua, 16)) throw std::runtime_error("Shader parameter Lua stack capacity exceeded");
    const Stack restore{lua, originalTop};
    const Context context{lua, shader};
    context.Table(paramsIndex, {});
    Snapshot result;
    Values values;
    Textures textures;
    std::unordered_set<std::string, StringHash, std::equal_to<>> samplers;
    for (const auto& parameter : shader.model.parameters) {
        (void)Bytes(parameter.type);
        if (!values.emplace(parameter.name, Value{parameter.type}).second)
            context.Error(parameter.name, "duplicate declared parameter");
    }
    for (const auto& declaration : shader.model.declarations) {
        if (declaration.kind == S::DeclarationKind::Texture) {
            if (values.contains(declaration.name) || !textures.emplace(declaration.name, Texture{}).second)
                context.Error(declaration.name, "duplicate declared texture or parameter");
        } else if (declaration.kind == S::DeclarationKind::Sampler) samplers.insert(declaration.name);
    }
    // Derivation depends on main declarations, never declaration or GPU slot
    // order. Classify shorter names first: foo_depth derives from foo, but is
    // no longer a main. Thus foo_depth_distance is an ordinary main unless its
    // exact suffix base is itself a main. Derived data never recursively becomes
    // a Canvas3D, and no new naming ban is imposed on otherwise ordinary names.
    std::vector<std::string_view> names;
    names.reserve(textures.size());
    for (const auto& [name, texture] : textures) names.push_back(name);
    std::sort(names.begin(), names.end(), [](auto a, auto b) {
        return a.size() == b.size() ? a < b : a.size() < b.size();
    });
    for (const auto name : names) {
        auto& texture = textures.find(name)->second;
        for (const auto& [suffix, part] : {std::pair<std::string_view, TexturePart>{"_depth", TexturePart::Depth},
             {"_distance", TexturePart::Distance}}) {
            if (name.ends_with(suffix)) {
                const std::string base(name.substr(0, name.size() - suffix.size()));
                const auto found = textures.find(base);
                if (found != textures.end() && found->second.base.empty()) { texture.base = base; texture.part = part; }
            }
        }
    }
    for (const auto& declaration : shader.model.declarations) {
        if (declaration.kind != S::DeclarationKind::Texture) continue;
        auto& texture = textures.at(declaration.name);
        if (!texture.base.empty()) continue;
        texture.root = result.textures.size();
        result.textures.push_back({declaration.name});
    }
    for (auto& [name, texture] : textures) {
        if (texture.base.empty()) continue;
        const auto& base = textures.at(texture.base);
        texture.root = base.root;
        auto& root = result.textures[texture.root];
        root.requiresCanvas3D = true;
        root.required = true; // A derived declaration is a Canvas3D contract even when optimized out.
    }
    InspectBuffers(context, shader.vertex, values);
    InspectBuffers(context, shader.fragment, values);
    InspectTextures(context, shader.vertex, textures, result, result.vertex);
    InspectTextures(context, shader.fragment, textures, result, result.fragment);
    lua_pushnil(lua);
    while (lua_next(lua, paramsIndex)) {
        if (lua_type(lua, -2) != LUA_TSTRING) context.Error({}, "field keys must be strings");
        std::size_t size{};
        const char* key = lua_tolstring(lua, -2, &size); // Already a string; no coercion/allocation.
        const std::string_view name(key, size); // Do not copy arbitrary-length unknown Lua keys.
        if (name.starts_with("Solongo")) context.Error(name, "engine builtin fields cannot be supplied");
        if (samplers.contains(name)) context.Error(name, "sampler is automatically managed");
        if (const auto found = values.find(name); found != values.end()) Read(context, lua_gettop(lua), name, found->second);
        else if (const auto textureFound = textures.find(name); textureFound != textures.end()) {
            if (!textureFound->second.base.empty()) context.Error(name, "derived texture is automatically supplied from '" + textureFound->second.base + "'");
            auto& request = result.textures[textureFound->second.root];
            if (!resolve) context.Error(name, "texture resolver is unavailable");
            const int top = lua_gettop(lua);
            try { resolve(name, top, request.requiresCanvas3D); }
            catch (const std::exception& error) { context.Error(name, error.what()); }
            if (lua_gettop(lua) != top) context.Error(name, "texture resolver changed the Lua stack");
            request.provided = true;
        } else context.Error(name, "unknown field");
        lua_pop(lua, 1);
    }
    for (const auto& [name, value] : values)
        if (value.required && !value.provided) context.Error(name, std::string("missing required ") + S::TypeName(value.type));
    for (const auto& texture : result.textures)
        if (texture.required && !texture.provided) context.Error(texture.name,
            texture.requiresCanvas3D ? "missing required Canvas3D main texture" : "missing required Texture2D input");
    PackBuffers(shader.vertex, values, result.vertex);
    PackBuffers(shader.fragment, values, result.fragment);
    return result;
}
}
