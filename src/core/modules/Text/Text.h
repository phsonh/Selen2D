#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace core::modules::Text {
using JsonValue = nlohmann::json;
inline constexpr int maximum_json_depth = 128;
// Implementation choices for Text D03; they are limits, not extra Lua APIs.
inline constexpr std::size_t maximum_json_bytes = 8 * 1024 * 1024;
inline constexpr std::size_t maximum_json_nodes = 262144;
inline constexpr std::size_t maximum_json_number_bytes = 1024;
inline constexpr double maximum_json_integer = 9007199254740991.0;
void ValidateJsonString(std::string_view text);
void ValidateJsonNumber(double value);
std::string JsonFieldPath(std::string_view parent, std::string_view key);

// Pure memory conversion. The Lua Json2Table entry restricts the root to a
// container; the host may also use this parser for its startup configuration.
JsonValue DecodeJson(std::string_view text, std::string_view source = "JSON text");
std::string EncodeJson(const JsonValue& value, int indent = -1);
std::string FormatTime(double seconds, std::string_view format = "mm:ss");
}
