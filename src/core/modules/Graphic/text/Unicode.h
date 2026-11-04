#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
namespace core::modules::Graphic::Text::detail {
struct UnicodeText {
    std::string text;
    std::vector<std::uint32_t> points;
    std::vector<std::uint32_t> bytes; // size points+1; expanded tab shares one source cluster
    std::vector<bool> grapheme, lineBreak, emergency; // boundary BEFORE point i
    std::vector<bool> connection; // required Unicode cursive connection across boundary
};
UnicodeText Decode(std::string_view, std::size_t maxBytes);
// UAX50: 0=U/Tu, 1=R, 2=Tr (prefer vertical substitution, otherwise rotate).
int VerticalOrientation(std::uint32_t);
struct UnicodeRun { std::size_t begin{}, end{}; std::uint32_t script{}; bool rtl{}; };
struct UnicodeRuns { std::vector<UnicodeRun> runs; bool rtl{}; };
UnicodeRuns Analyze(const std::vector<std::uint32_t>&, std::size_t begin, std::size_t end);
}
