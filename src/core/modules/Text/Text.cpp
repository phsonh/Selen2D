#include "Text.h"
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <stdexcept>

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

void ValidateJson(const JsonValue& value, int depth) {
    if (depth > maximum_json_depth) throw std::invalid_argument("JSON nesting exceeds 128 levels");
    if (value.is_number_float() && !std::isfinite(value.get<double>()))
        throw std::invalid_argument("JSON number must be finite");
    if (value.is_structured()) for (const auto& child : value) ValidateJson(child, depth + 1);
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
        // The bundled parser treats a raw NUL as end-of-input even when given
        // an explicit iterator range. Reject it before parsing so a valid
        // prefix cannot hide malformed trailing bytes. Escaped \u0000 remains
        // legal JSON and decodes to an ordinary binary-safe Lua string.
        if (const auto nul = text.find('\0'); nul != std::string_view::npos)
            throw std::invalid_argument("unescaped NUL in JSON at byte " + std::to_string(nul + 1));
        ValidateUtf8(text);
        const auto normalized = AllowTrailingCommas(text);
        const auto limit_depth = [](int depth, JsonValue::parse_event_t, JsonValue&) {
            if (depth > maximum_json_depth) throw std::invalid_argument("JSON nesting exceeds 128 levels");
            return true;
        };
        return JsonValue::parse(normalized.begin(), normalized.end(), limit_depth, true, true);
    } catch (const std::exception& error) {
        throw std::invalid_argument(std::string(source) + ": " + error.what());
    }
}
std::string EncodeJson(const JsonValue& value, int indent) {
    if (indent < -1 || indent > 8) throw std::invalid_argument("JSON indent must be -1..8");
    ValidateJson(value, 0);
    return value.dump(indent, ' ', false, JsonValue::error_handler_t::strict);
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
