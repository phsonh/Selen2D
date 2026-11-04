#include "Path.h"
#include <utf8proc.h>
#include <cstdlib>
#include <memory>
#include <stdexcept>

namespace core::modules::File {
namespace {
std::optional<std::string> Map(std::string_view input, utf8proc_option_t options) {
    utf8proc_uint8_t* output{};
    const auto size=utf8proc_map(reinterpret_cast<const utf8proc_uint8_t*>(input.data()),
        static_cast<utf8proc_ssize_t>(input.size()), &output, options);
    std::unique_ptr<utf8proc_uint8_t,decltype(&std::free)> owner(output,&std::free);
    if(size<0) {
        if(size==UTF8PROC_ERROR_NOMEM) throw std::bad_alloc();
        return {};
    }
    return std::string(reinterpret_cast<const char*>(output),static_cast<std::size_t>(size));
}
bool Component(std::string_view part) {
    if(part.empty() || part=="." || part=="..") return false;
    for(const auto c:part) if(std::string_view(":*?/\\").find(c)!=std::string_view::npos) return false;
    return true;
}
bool Syntax(std::string_view input) {
    if(input.empty()||input.size()>4096||input.front()=='/'||input.back()=='/') return false;
    for(std::size_t i=0;i<input.size();) {
        utf8proc_int32_t scalar{};
        const auto n=utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t*>(input.data()+i),
            static_cast<utf8proc_ssize_t>(input.size()-i),&scalar);
        if(n<=0 || scalar==0 || utf8proc_category(scalar)==UTF8PROC_CATEGORY_CC) return false;
        i+=static_cast<std::size_t>(n);
    }
    for(std::size_t start=0;start<input.size();) {
        auto end=input.find('/',start); if(end==std::string_view::npos) end=input.size();
        if(!Component(input.substr(start,end-start))) return false;
        start=end+1;
    }
    return true;
}
}
std::optional<std::string> Normalize(std::string_view input) {
    if(!Syntax(input)) return {};
    auto result=Map(input,static_cast<utf8proc_option_t>(UTF8PROC_STABLE|UTF8PROC_COMPOSE));
    if(!result||!Syntax(*result)) return {};
    return result;
}
std::string FoldPath(std::string_view path) {
    auto result=Map(path,static_cast<utf8proc_option_t>(UTF8PROC_STABLE|UTF8PROC_COMPOSE|UTF8PROC_CASEFOLD));
    if(!result) throw std::invalid_argument("invalid UTF-8 collision key");
    return std::move(*result);
}
bool PathContains(std::string_view parent,std::string_view child) noexcept {
    return parent.empty() || parent==child || (child.size()>parent.size()&&child.starts_with(parent)&&child[parent.size()]=='/');
}
}
