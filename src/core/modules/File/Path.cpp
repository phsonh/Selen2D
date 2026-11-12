#include "Path.h"
#include "IoError.h"
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
namespace Path {
namespace {
std::string Checked(std::string_view input) {
    auto value=Normalize(input);
    if(!value) throw InvalidPath("expected a nonempty canonical relative UTF-8 path");
    return std::move(*value);
}
std::size_t ExtensionOffset(std::string_view name) noexcept {
    const auto first=name.find_first_not_of('.');
    if(first==std::string_view::npos) return name.size();
    const auto dot=name.rfind('.');
    return dot==std::string_view::npos||dot<first?name.size():dot;
}
}
std::string Join(std::span<const std::string_view> parts) {
    if(parts.empty()) throw InvalidPath("File.Path.Join requires at least one path");
    std::string result;
    for(const auto part:parts) {
        const auto normalized=Checked(part);
        const auto separator=result.empty()?0u:1u;
        if(normalized.size()+separator>4096-result.size()) throw InvalidPath("joined path exceeds 4096 UTF-8 bytes");
        if(separator) result.push_back('/');
        result+=normalized;
    }
    return Checked(result);
}
std::string GetFileName(std::string_view path) {
    const auto normalized=Checked(path);
    const auto slash=normalized.rfind('/');
    return normalized.substr(slash==std::string::npos?0:slash+1);
}
std::optional<std::string> GetParent(std::string_view path) {
    auto normalized=Checked(path);
    const auto slash=normalized.rfind('/');
    if(slash==std::string::npos) return {};
    normalized.resize(slash); return normalized;
}
std::string GetExtension(std::string_view path) {
    const auto name=GetFileName(path);
    return name.substr(ExtensionOffset(name));
}
std::string ReplaceExtension(std::string_view path,std::string_view extension) {
    auto normalized=Checked(path);
    std::string suffix;
    if(!extension.empty()) {
        if(extension.front()!='.'||extension.find('/')!=extension.npos||extension.find('\\')!=extension.npos)
            throw InvalidPath("extension must start with a dot and contain no path separator");
        // Prefix permits the explicitly valid suffix "." without treating it
        // as the forbidden standalone path component. Backend names are later
        // checked by the actual VFS mutation, not by these pure string tools.
        if(extension.size()>4096) throw InvalidPath("extension exceeds 4096 UTF-8 bytes");
        suffix=Checked(std::string("x")+std::string(extension)).substr(1);
    }
    const auto slash=normalized.rfind('/');
    const auto begin=slash==std::string::npos?0:slash+1;
    normalized.resize(begin+ExtensionOffset(std::string_view(normalized).substr(begin)));
    if(suffix.size()>4096-normalized.size()) throw InvalidPath("renamed path exceeds 4096 UTF-8 bytes");
    normalized+=suffix;
    return Checked(normalized);
}
}
}
