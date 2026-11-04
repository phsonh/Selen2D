#include "Startup.h"
#include "UserDataRoot.h"
#include "launch.h"
#include "core/modules/File/Vfs.h"
#include "core/modules/File/PackageCrypto.h"
#include <utf8proc.h>
#include <set>
#include <map>
#include <stdexcept>

namespace engine::Startup {
namespace {
namespace File = core::modules::File;
std::string Path(std::string_view value) {
    const auto path = File::Normalize(value);
    if(!path || path->empty()) throw std::invalid_argument("Invalid path in compiled launch configuration");
    return *path;
}
void ValidateSecret(std::span<const std::uint8_t> bytes) {
    std::size_t offset{};
    while(offset < bytes.size()) {
        utf8proc_int32_t scalar{};
        const auto size = utf8proc_iterate(bytes.data() + offset,
            static_cast<utf8proc_ssize_t>(bytes.size() - offset), &scalar);
        if(size <= 0) throw std::invalid_argument("Compiled secret is not valid UTF-8");
        offset += static_cast<std::size_t>(size);
    }
}
}
using Names = std::map<std::string, std::string, std::less<>>;
void RecordNames(Names& names, std::string_view path) {
    (void)Path(path);
    std::size_t end{};
    do {
        end = path.find('/', end);
        const std::string raw(path.substr(0, end));
        const auto key = File::FoldPath(raw);
        const auto [entry, added] = names.emplace(key, raw);
        if(!added && entry->second != raw)
            throw std::invalid_argument("Compiled mapping names have an NFC or Unicode case collision");
        if(end == std::string_view::npos) break;
        ++end;
    } while(true);
}
Config Load(const std::filesystem::path& directory) { return Load(directory, launch_config::value); }

Config Load(const std::filesystem::path& directory, const launch::Config& declaration) {
    if(!directory.is_absolute()) throw std::invalid_argument("Deployment root must be an absolute executable directory");
    if(declaration.mounts.size() > 4096 || declaration.directories.size() > 4096)
        throw std::invalid_argument("Compiled launch configuration has too many records");
    Config config;
    config.directory = directory.lexically_normal();
    config.entry = Path(declaration.entry);
    std::vector<File::MountSpec> mounts;
    std::set<std::uint32_t> parts;
    Names virtualNames, deploymentNames, userNames;
    bool protectedCode{};
    for(const auto& record : declaration.mounts) {
        if(!parts.insert(record.parts).second) throw std::invalid_argument("Compiled package parts must be globally unique");
        RecordNames(virtualNames, record.target);
        RecordNames(deploymentNames, record.source);
        mounts.push_back({std::filesystem::u8path(Path(record.source)), Path(record.target), record.parts, record.encrypted});
        protectedCode |= record.encrypted;
    }
    if(protectedCode && !declaration.secret) throw std::invalid_argument("Encrypted packages require an explicit compiled secret");
    std::optional<File::PackageKey> key;
    if(declaration.secret) {
        ValidateSecret(*declaration.secret);
        const auto bytes = *declaration.secret;
        // Explicit length includes an intentional embedded NUL, excludes any
        // C string terminator. Missing and explicitly empty remain distinct.
        const std::string_view secret = bytes.empty() ? std::string_view{} :
            std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        key = File::DerivePackageKey(secret);
    }
    std::optional<std::filesystem::path> userRoot;
    std::vector<File::DirectorySpec> directories;
    for(const auto& record : declaration.directories) {
        std::filesystem::path base;
        switch(record.base) {
        case launch::Base::Deployment: base = config.directory; break;
        case launch::Base::UserData:
            if(!userRoot) userRoot = ResolveUserDataRoot();
            base = *userRoot;
            break;
        default: throw std::invalid_argument("Unknown base in compiled directory rule");
        }
        RecordNames(virtualNames, record.target);
        RecordNames(record.base == launch::Base::Deployment ? deploymentNames : userNames, record.path);
        directories.push_back({std::move(base), Path(record.path), Path(record.target)});
    }
    config.files = std::make_shared<File::FileSystem>(config.directory, std::move(mounts), std::move(directories), key);
    config.files->CheckCodeSource(config.entry);
    if(config.files->Stat(config.entry).type != File::NodeType::File)
        throw std::runtime_error("Compiled Lua entry is not an authorized file: " + config.entry);
    return config;
}
}
