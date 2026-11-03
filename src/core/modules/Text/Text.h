#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace core::modules::Text {
using JsonValue = nlohmann::json;
inline constexpr int maximum_json_depth = 128;

// Pure memory conversion. The Lua Json2Table entry restricts the root to a
// container; the host may also use this parser for its startup configuration.
JsonValue DecodeJson(std::string_view text, std::string_view source = "JSON text");
std::string EncodeJson(const JsonValue& value, int indent = -1);
std::string FormatTime(double seconds, std::string_view format = "mm:ss");
}
