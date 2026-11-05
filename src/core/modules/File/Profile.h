#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// Private, opt-in diagnostics. No filenames, configuration or key material are
// collected. Timed scopes are inclusive: nested durations must not be summed.
namespace core::modules::File::profile {
#define VFS_PROFILE_METRICS(X) \
    X(Construct) X(Scan) X(AddDisk) X(Child) X(Stat) X(Open) X(ListDirectory) \
    X(CheckCodeSource) X(CreateDirectory) X(Remove) X(Rename) X(Copy) X(Move) \
    X(Replace) X(ReadSnapshot) X(OpenReadSource) X(DirectoriesVisited) \
    X(DirectoryEnumerations) X(EntriesEnumerated) X(FilesVisited) \
    X(AttributeQueries) X(IdentityQueries) X(AnchorCreations) \
    X(PackageViewPasses) X(PackageViewEntries) X(NativeOpen) X(NativeRead) \
    X(NativeReadBytes) X(NativeSize) X(NativeModified) X(PackageLoad) \
    X(PackageRead) X(PackageReadBytes) X(StartupLoad) X(EngineInit) X(GpuInit) \
    X(AudioInit) X(ScriptLoad) X(ScriptExecute) X(LuaCompile) X(Entry) \
    X(BeforeEntryElapsed) X(WindowShownElapsed) X(FirstFrameSubmitElapsed)
enum class Metric {
#define VFS_PROFILE_ENUM(name) name,
    VFS_PROFILE_METRICS(VFS_PROFILE_ENUM)
#undef VFS_PROFILE_ENUM
    Count
};
inline constexpr std::size_t Count=static_cast<std::size_t>(Metric::Count);
struct Value { std::uint64_t count{}, nanoseconds{}; };
using Snapshot=std::array<Value,Count>;
inline constexpr std::array<std::string_view,Count> Names{
#define VFS_PROFILE_NAME(name) #name,
    VFS_PROFILE_METRICS(VFS_PROFILE_NAME)
#undef VFS_PROFILE_NAME
};
#undef VFS_PROFILE_METRICS
inline std::string_view Name(std::size_t i) noexcept { return Names[i]; }
#ifdef ENGINE_VFS_PROFILE
void Initialize() noexcept;
void Flush() noexcept;
void Add(Metric metric,std::uint64_t count) noexcept;
void Mark(Metric metric) noexcept;
Snapshot Capture() noexcept;
class Scope final {
public:
    explicit Scope(Metric metric) noexcept;
    ~Scope();
    Scope(const Scope&)=delete;
    Scope& operator=(const Scope&)=delete;
private:
    Metric metric_;
    std::uint64_t start_;
};
#else
inline void Initialize() noexcept {}
inline void Flush() noexcept {}
inline Snapshot Capture() noexcept { return {}; }
#endif
}
#define VFS_PROFILE_JOIN_(a,b) a##b
#define VFS_PROFILE_JOIN(a,b) VFS_PROFILE_JOIN_(a,b)
#ifdef ENGINE_VFS_PROFILE
#define VFS_PROFILE_SCOPE(name) ::core::modules::File::profile::Scope VFS_PROFILE_JOIN(vfsProfileScope_,__LINE__)(::core::modules::File::profile::Metric::name)
#define VFS_PROFILE_ADD(name,count) ::core::modules::File::profile::Add(::core::modules::File::profile::Metric::name,count)
#define VFS_PROFILE_MARK(name) ::core::modules::File::profile::Mark(::core::modules::File::profile::Metric::name)
#else
#define VFS_PROFILE_SCOPE(name) ((void)0)
#define VFS_PROFILE_ADD(name,count) ((void)0)
#define VFS_PROFILE_MARK(name) ((void)0)
#endif
