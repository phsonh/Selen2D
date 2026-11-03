#include "Text.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <ostream>
#include <streambuf>
#include <unordered_set>
#include <vector>
#include <utility>

namespace core::modules::Text {
namespace {
void ValidateUtf8(std::string_view text) {
    // Validate the entire input, including comments that the JSON parser skips.
    for (std::size_t i = 0; i < text.size();) {
        const auto start = i;
        const auto first = static_cast<unsigned char>(text[i++]);
        if (first < 0x80) continue;
        unsigned remaining{}, codepoint{}, minimum{};
        if (first >= 0xc2 && first <= 0xdf) { remaining = 1; codepoint = first & 0x1f; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { remaining = 2; codepoint = first & 0x0f; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { remaining = 3; codepoint = first & 0x07; minimum = 0x10000; }
        else throw std::invalid_argument("invalid UTF-8 at byte " + std::to_string(start + 1));
        if (remaining > text.size() - i) throw std::invalid_argument("truncated UTF-8 at byte " + std::to_string(start + 1));
        while (remaining--) {
            const auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0) != 0x80) throw std::invalid_argument("invalid UTF-8 at byte " + std::to_string(i));
            codepoint = (codepoint << 6) | (next & 0x3f);
        }
        if (codepoint < minimum || codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff))
            throw std::invalid_argument("invalid UTF-8 at byte " + std::to_string(start + 1));
    }
}

std::string AllowTrailingCommas(std::string_view text) {
    // The bundled parser handles JSONC comments natively, but not trailing
    // commas. Blank only a comma immediately before ] or } outside strings and
    // comments. Keep offsets/newlines intact for useful parser diagnostics.
    std::string result(text);
    // Accept one BOM only at the start; preserve all parser byte positions.
    if (text.starts_with("\xef\xbb\xbf")) result.replace(0, 3, 3, ' ');
    std::size_t pending = std::string::npos;
    bool value_before = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        if (c == '/' && i + 1 < text.size()) {
            if (text[i + 1] == '/') {
                i += 2;
                while (i < text.size() && text[i] != '\n' && text[i] != '\r') ++i;
                if (i == text.size()) break;
                continue;
            }
            if (text[i + 1] == '*') {
                const auto end = text.find("*/", i + 2);
                if (end == std::string_view::npos) break; // Native parser diagnoses it.
                i = end + 1;
                continue;
            }
        }
        if (c == '"') {
            pending = std::string::npos;
            value_before = true;
            while (++i < text.size()) {
                if (text[i] == '\\') { if (i + 1 < text.size()) ++i; }
                else if (text[i] == '"') break;
            }
            continue;
        }
        if (c == ',') {
            // Do not turn [,], {,}, or a second comma into valid JSON.
            pending = value_before ? i : std::string::npos;
            value_before = false;
            continue;
        }
        if ((c == ']' || c == '}') && pending != std::string::npos) result[pending] = ' ';
        pending = std::string::npos;
        value_before = c != '[' && c != '{' && c != ':';
    }
    return result;
}

bool Digit(char c) { return c >= '0' && c <= '9'; }
void ValidateNumberToken(std::string_view token, std::size_t offset) {
    const auto fail = [&](const char* reason) {
        throw std::invalid_argument("JSON number at byte " + std::to_string(offset + 1) + ": " + reason);
    };
    if (token.size() > maximum_json_number_bytes) fail("number token exceeds 1024 bytes");
    std::size_t i = token.front() == '-' ? 1 : 0;
    std::string digits;
    while (i < token.size() && Digit(token[i])) digits += token[i++];
    int fraction{};
    if (i < token.size() && token[i] == '.') {
        ++i;
        while (i < token.size() && Digit(token[i])) { digits += token[i++]; ++fraction; }
    }
    int exponent{};
    if (i < token.size()) {
        ++i; // e/E; the scanner has checked the token grammar.
        const bool negative = token[i] == '-';
        if (token[i] == '-' || token[i] == '+') ++i;
        // Only comparisons with at most token.size() digits matter. Saturation
        // handles arbitrarily long exponents without integer overflow or pow().
        const int cap = static_cast<int>(token.size()) + 32;
        for (; i < token.size(); ++i) exponent = std::min(cap, exponent * 10 + token[i] - '0');
        if (negative) exponent = -exponent;
    }
    const auto first = digits.find_first_not_of('0');
    if (first == std::string::npos) return; // 0e+huge and 0e-huge remain exact zero.
    digits.erase(0, first);
    int shift = exponent - fraction;
    while (digits.back() == '0') { digits.pop_back(); ++shift; }
    if (shift >= 0) {
        constexpr std::string_view bound = "9007199254740991";
        const auto length = digits.size() + static_cast<std::size_t>(shift);
        if (length > bound.size()) fail("integer exceeds +/-9007199254740991; use a string for exact identifiers");
        if (length == bound.size()) {
            digits.append(static_cast<std::size_t>(shift), '0');
            if (digits > bound) fail("integer exceeds +/-9007199254740991; use a string for exact identifiers");
        }
    }
    // Raw exact-integer admission above MUST precede any floating conversion.
    double value{};
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value, std::chars_format::general);
    if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() || !std::isfinite(value))
        fail("number overflow or nonzero underflow is not supported");
    if (value == 0) fail("nonzero number underflows to zero");
    if (std::trunc(value) == value && std::abs(value) > maximum_json_integer)
        fail("rounded Lua integer exceeds +/-9007199254740991; use a string for exact identifiers");
}
void ValidateNumberTokens(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == '"') {
            ++i;
            while (i < text.size()) {
                if (text[i] == '\\') { i += std::min<std::size_t>(2, text.size() - i); }
                else if (text[i++] == '"') break;
            }
        } else if (text[i] == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            i += 2; while (i < text.size() && text[i] != '\n' && text[i] != '\r') ++i;
        } else if (text[i] == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            const auto end = text.find("*/", i + 2);
            i = end == std::string_view::npos ? text.size() : end + 2;
        } else if (text[i] == '-' || Digit(text[i])) {
            const auto start = i;
            if (text[i] == '-') ++i;
            if (i == text.size() || !Digit(text[i])) continue; // Parser diagnoses invalid syntax.
            if (text[i] == '0') ++i;
            else while (i < text.size() && Digit(text[i])) ++i;
            if (i < text.size() && text[i] == '.') {
                ++i; const auto begin = i;
                while (i < text.size() && Digit(text[i])) ++i;
                if (i == begin) continue;
            }
            if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
                ++i;
                if (i < text.size() && (text[i] == '+' || text[i] == '-')) ++i;
                const auto begin = i;
                while (i < text.size() && Digit(text[i])) ++i;
                if (i == begin) continue;
            }
            ValidateNumberToken(text.substr(start, i - start), start);
        } else ++i;
    }
}
struct Budget {
    std::size_t nodes{}, bytes{};
    void Node() {
        if (++nodes > maximum_json_nodes) throw std::invalid_argument("JSON exceeds 262144 value nodes");
    }
    void String(std::string_view text) {
        if (text.size() > maximum_json_bytes - bytes) throw std::invalid_argument("JSON strings exceed 8 MiB");
        bytes += text.size();
        ValidateUtf8(text);
    }
};
class BoundedOutput : public std::streambuf {
public:
    std::string bytes;
protected:
    std::streamsize xsputn(const char* source, std::streamsize count) override {
        if (count < 0 || static_cast<std::size_t>(count) > maximum_json_bytes - bytes.size())
            throw std::invalid_argument("JSON output exceeds 8 MiB");
        bytes.append(source, static_cast<std::size_t>(count));
        return count;
    }
    int_type overflow(int_type value) override {
        if (traits_type::eq_int_type(value, traits_type::eof())) return traits_type::not_eof(value);
        const auto byte = traits_type::to_char_type(value);
        xsputn(&byte, 1);
        return value;
    }
};
void ValidateJson(const JsonValue& value, int depth, Budget& budget) {
    if (depth > maximum_json_depth) throw std::invalid_argument("JSON nesting exceeds 128 levels");
    budget.Node();
    if (value.is_number()) {
        if (value.is_number_unsigned() && value.get<std::uint64_t>() > 9007199254740991ULL)
            throw std::invalid_argument("JSON integer exceeds +/-9007199254740991; use a string for exact identifiers");
        if (value.is_number_integer() && !value.is_number_unsigned()) {
            const auto integer = value.get<std::int64_t>();
            if (integer < -9007199254740991LL || integer > 9007199254740991LL)
                throw std::invalid_argument("JSON integer exceeds +/-9007199254740991; use a string for exact identifiers");
        }
        const auto number = value.get<double>();
        ValidateJsonNumber(number);
    } else if (value.is_string()) budget.String(value.get_ref<const std::string&>());
    else if (value.is_object()) {
        for (const auto& child : value.items()) { budget.String(child.key()); ValidateJson(child.value(), depth + 1, budget); }
    } else if (value.is_array()) {
        for (const auto& child : value) ValidateJson(child, depth + 1, budget);
    } else if (!value.is_boolean() && !value.is_null()) throw std::invalid_argument("unsupported JSON value");
}
void CanonicalizeZero(JsonValue& value) {
    if (value.is_number() && value.get<double>() == 0) value = 0;
    else if (value.is_structured()) for (auto& child : value) CanonicalizeZero(child);
}

std::string WholeDecimal(double value) {
    // Even DBL_MAX can be formatted without an overflowing integer conversion.
    // Fixed precision 0 has at most 309 digits for a finite nonnegative double.
    std::array<char, 320> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::fixed, 0);
    if (converted.ec != std::errc{}) throw std::runtime_error("duration formatting failed");
    return std::string(buffer.data(), converted.ptr);
}
unsigned DivideDecimal(std::string& value, unsigned divisor) {
    unsigned remainder = 0;
    for (char& digit : value) {
        const unsigned next = remainder * 10 + static_cast<unsigned>(digit - '0');
        digit = static_cast<char>('0' + next / divisor);
        remainder = next % divisor;
    }
    const auto first = value.find_first_not_of('0');
    value.erase(0, first == std::string::npos ? value.size() - 1 : first);
    return remainder;
}
void IncrementDecimal(std::string& value) {
    for (std::size_t i = value.size(); i > 0; --i) {
        if (value[i - 1] != '9') { ++value[i - 1]; return; }
        value[i - 1] = '0';
    }
    value.insert(value.begin(), '1');
}
std::string PadTwo(std::string value) { return value.size() < 2 ? "0" + value : value; }
std::string Two(unsigned value) { return PadTwo(std::to_string(value)); }
}

JsonValue DecodeJson(std::string_view text, std::string_view source) {
    try {
        if (text.size() > maximum_json_bytes) throw std::invalid_argument("JSON input exceeds 8 MiB");
        // The bundled parser treats a raw NUL as end-of-input even when given
        // an explicit iterator range. Reject it before parsing so a valid
        // prefix cannot hide malformed trailing bytes. Escaped \u0000 remains
        // legal JSON and decodes to an ordinary binary-safe Lua string.
        if (const auto nul = text.find('\0'); nul != std::string_view::npos)
            throw std::invalid_argument("unescaped NUL in JSON at byte " + std::to_string(nul + 1));
        ValidateUtf8(text);
        ValidateNumberTokens(text);
        const auto normalized = AllowTrailingCommas(text);
        std::vector<std::unordered_set<std::string>> objectKeys;
        std::size_t nodes{};
        const auto admission = [&](int depth, JsonValue::parse_event_t event, JsonValue& value) {
            if (depth > maximum_json_depth) throw std::invalid_argument("JSON nesting exceeds 128 levels");
            using Event = JsonValue::parse_event_t;
            if (event == Event::object_start || event == Event::array_start || event == Event::value)
                if (++nodes > maximum_json_nodes) throw std::invalid_argument("JSON exceeds 262144 value nodes");
            if (event == Event::object_start) objectKeys.emplace_back();
            else if (event == Event::object_end) objectKeys.pop_back();
            else if (event == Event::key) {
                const auto& key = value.get_ref<const std::string&>();
                if (!objectKeys.back().insert(key).second)
                    throw std::invalid_argument("duplicate JSON field " + JsonFieldPath("$", key));
            }
            return true;
        };
        auto value = JsonValue::parse(normalized.begin(), normalized.end(), admission, true, true);
        Budget budget;
        ValidateJson(value, 0, budget);
        return value;
    } catch (const std::exception& error) {
        throw std::invalid_argument(std::string(source) + ": " + error.what());
    }
}
std::string EncodeJson(const JsonValue& value, int indent) {
    if (indent < -1 || indent > 8) throw std::invalid_argument("JSON indent must be -1..8");
    Budget budget;
    ValidateJson(value, 0, budget); // Admission precedes even the normalized copy.
    auto normalized = value;
    CanonicalizeZero(normalized);
    BoundedOutput output;
    std::ostream stream(&output);
    stream.exceptions(std::ios::badbit | std::ios::failbit);
    // Same serializer as this pinned json.hpp's dump/operator<<, with bounded
    // writes instead of constructing an unbounded temporary escaped string.
    nlohmann::detail::serializer<JsonValue> serializer(nlohmann::detail::output_adapter<char>(stream),
        ' ', JsonValue::error_handler_t::strict);
    serializer.dump(normalized, indent >= 0, false, static_cast<unsigned>(std::max(indent, 0)));
    return std::move(output.bytes);
}

void ValidateJsonString(std::string_view text) { ValidateUtf8(text); }
void ValidateJsonNumber(double value) {
    if (!std::isfinite(value)) throw std::invalid_argument("JSON number must be finite");
    if (std::trunc(value) == value && std::abs(value) > maximum_json_integer)
        throw std::invalid_argument("JSON integer exceeds +/-9007199254740991; use a string for exact identifiers");
}
std::string JsonFieldPath(std::string_view parent, std::string_view key) {
    // Escape binary keys without invoking JSON serialization or Lua metamethods.
    // Limit diagnostic size; it must not be truncated at an embedded NUL.
    std::string result(parent);
    result += "[\"";
    constexpr char hex[] = "0123456789abcdef";
    const auto count = std::min<std::size_t>(key.size(), 128);
    for (std::size_t i = 0; i < count; ++i) {
        const auto c = static_cast<unsigned char>(key[i]);
        if (c == '"' || c == '\\') { result += '\\'; result += static_cast<char>(c); }
        else if (c < 32 || c >= 127) {
            result += "\\x"; result += hex[c >> 4]; result += hex[c & 15];
        } else result += static_cast<char>(c);
    }
    if (count != key.size()) result += "...";
    result += "\"]";
    return result;
}

std::string FormatTime(double seconds, std::string_view format) {
    if (!std::isfinite(seconds) || seconds < 0) throw std::invalid_argument("duration must be a finite nonnegative number");
    const bool fraction = format == "mm:ss.ms";
    if (!fraction && format != "mm:ss" && format != "hh:mm:ss" && format != "dd:hh:mm:ss")
        throw std::invalid_argument("format must be mm:ss, hh:mm:ss, dd:hh:mm:ss, or mm:ss.ms");

    double whole = std::floor(seconds);
    unsigned hundredths = 0;
    if (fraction) {
        // Prefer rounding the duration as a whole; splitting first can amplify
        // cancellation at ordinary decimal ties. Large values use a bounded
        // fractional part so seconds * 100 never overflows or loses whole units.
        if (seconds <= 90071992547409.0) {
            const double rounded = std::round(seconds * 100.0);
            whole = std::floor(rounded / 100.0);
            hundredths = static_cast<unsigned>(std::fmod(rounded, 100.0));
        } else {
            hundredths = static_cast<unsigned>(std::round((seconds - whole) * 100.0));
        }
    }
    std::string total = WholeDecimal(whole == 0 ? 0 : whole);
    if (hundredths == 100) { IncrementDecimal(total); hundredths = 0; }
    const auto secs = DivideDecimal(total, 60);
    if (format == "mm:ss" || fraction)
        return PadTwo(total) + ":" + Two(secs) + (fraction ? "." + Two(hundredths) : "");
    const auto minutes = DivideDecimal(total, 60);
    if (format == "hh:mm:ss") return PadTwo(total) + ":" + Two(minutes) + ":" + Two(secs);
    const auto hours = DivideDecimal(total, 24);
    return PadTwo(total) + ":" + Two(hours) + ":" + Two(minutes) + ":" + Two(secs);
}
}
