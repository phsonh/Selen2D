#include "Profile.h"
#ifdef ENGINE_VFS_PROFILE
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace core::modules::File::profile {
namespace {
struct Counter { std::atomic<std::uint64_t> count{}, nanoseconds{}; };
std::array<Counter,Count> counters;
std::uint64_t Now() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
std::atomic<std::uint64_t> epoch{Now()};
}
void Initialize() noexcept { epoch.store(Now(),std::memory_order_relaxed); }
void Add(Metric metric,std::uint64_t count) noexcept {
    counters[static_cast<std::size_t>(metric)].count.fetch_add(count,std::memory_order_relaxed);
}
Scope::Scope(Metric metric) noexcept : metric_(metric),start_(Now()) {}
Scope::~Scope() {
    auto& c=counters[static_cast<std::size_t>(metric_)];
    c.nanoseconds.fetch_add(Now()-start_,std::memory_order_relaxed);
    c.count.fetch_add(1,std::memory_order_relaxed);
}
void Mark(Metric metric) noexcept {
    auto& c=counters[static_cast<std::size_t>(metric)];
    // Only the first occurrence is an elapsed startup marker.
    if(c.count.fetch_add(1,std::memory_order_relaxed)==0)
        c.nanoseconds.store(Now()-epoch.load(std::memory_order_relaxed),std::memory_order_relaxed);
}
Snapshot Capture() noexcept {
    Snapshot result;
    for(std::size_t i=0;i<Count;++i) result[i]={
        counters[i].count.load(std::memory_order_relaxed),
        counters[i].nanoseconds.load(std::memory_order_relaxed)};
    return result;
}
void Flush() noexcept {
    try {
        std::filesystem::path path;
#ifdef _WIN32
        const auto* value=_wgetenv(L"ENGINE_VFS_PROFILE_PATH");
#else
        const auto* value=std::getenv("ENGINE_VFS_PROFILE_PATH");
#endif
        if(!value || !*value) return;
        path=value;
        std::ofstream output(path,std::ios::trunc);
        if(!output) throw 0;
        output<<"metric,count,nanoseconds\n";
        const auto snapshot=Capture();
        for(std::size_t i=0;i<Count;++i)
            output<<Name(i)<<','<<snapshot[i].count<<','<<snapshot[i].nanoseconds<<'\n';
        output.flush();
        if(!output) throw 0;
    } catch(...) { std::fputs("VFS profile output failed\n",stderr); }
}
}
#endif
