#include "Startup.h"
#include "core/modules/File/File.h"
#include "core/modules/File/Vfs.h"
#include "core/modules/Text/Text.h"
#include <stdexcept>

namespace engine::Startup {
namespace {
std::string Utf8(const std::filesystem::path& path) {
    const auto text=path.generic_u8string(); return {reinterpret_cast<const char*>(text.data()),text.size()};
}
}
Config Load(const std::filesystem::path& executableDirectory) {
    Config config;
    config.directory=std::filesystem::absolute(executableDirectory).lexically_normal();
    config.entry="launch.lua";
    config.files=std::make_shared<core::modules::File::FileSystem>(
        config.directory,std::vector<core::modules::File::MountSpec>{});
    if(config.files->Stat(config.entry).type!=core::modules::File::NodeType::File)
        throw std::runtime_error("Lua entry not found in executable directory: "+Utf8(config.directory/config.entry));
    return config;
}
Config LoadManifest(const std::filesystem::path& executableDirectory) {
    Config config;
    config.directory=std::filesystem::absolute(executableDirectory).lexically_normal();
    const auto filename=Utf8(config.directory/"launch");
    if(!std::filesystem::is_regular_file(config.directory/"launch"))
        throw std::runtime_error("Missing startup configuration: "+filename+"; provide entry and mounts explicitly");
    core::modules::File::ReadOptions options; options.maxBytes=64*1024;
    const auto snapshot=core::modules::File::ReadSnapshot(filename,options);
    const auto json=core::modules::Text::DecodeJson(snapshot.bytes,filename);
    if(!json.is_object() || !json.contains("entry") || !json["entry"].is_string() ||
       !json.contains("mounts") || !json["mounts"].is_array())
        throw std::runtime_error(filename+": expected string 'entry' and array 'mounts'");
    for(const auto& [key,value]:json.items()) {
        (void)value;
        if(key!="entry" && key!="mounts") throw std::runtime_error(filename+": unknown startup field '"+key+"'");
    }
    const auto entryText=json["entry"].get<std::string>();
    std::vector<core::modules::File::MountSpec> mounts;
    for(const auto& mount:json["mounts"]) {
        if(!mount.is_object() || mount.size()!=2 || !mount.contains("source") || !mount["source"].is_string() ||
           !mount.contains("target") || !mount["target"].is_string())
            throw std::runtime_error(filename+": each mount requires exactly string 'source' and 'target'");
        const auto source=mount["source"].get<std::string>();
        if(source.empty() || source.size()>4096 || source.find('\0')!=std::string::npos)
            throw std::runtime_error(filename+": invalid mount source path");
        mounts.push_back({std::filesystem::u8path(source),mount["target"].get<std::string>()});
    }
    config.files=std::make_shared<core::modules::File::FileSystem>(config.directory,std::move(mounts));
    const auto entry=config.files->NormalizePath(entryText);
    if(!entry || entry->empty()) throw std::runtime_error(filename+": 'entry' must be a valid nonempty virtual path");
    if(config.files->Stat(*entry).type!=core::modules::File::NodeType::File)
        throw std::runtime_error(filename+": Lua entry not found in mounted VFS: "+*entry);
    config.entry=*entry;
    return config;
}
}
