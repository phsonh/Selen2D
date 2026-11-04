#include "Resource.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <list>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace core::modules::Resource {
namespace detail {

bool Terminal(State state) {
    return state == State::Ready || state == State::Failed || state == State::Cancelled;
}
struct Transaction {
    State state{State::Queued};
    // Sticky publication witness, independent of external Job handles.
    bool committedEver{};
    Progress progress;
    std::optional<Error> error;
};
struct ManifestState {
    std::list<Request> entries;
    std::unordered_set<std::string> names;
    bool sealed{};
};
struct Work {
    std::shared_ptr<Transaction> transaction;
    std::list<Handle> resources;
    std::list<Handle>::iterator scan;
    std::size_t preparedInPass{};
    bool scanning{};
};
struct Record {
    Handle handle;
    Request request;
    std::shared_ptr<Transaction> transaction;
    std::unique_ptr<Operation> operation;
    std::shared_ptr<const Asset> asset;
    std::optional<Error> error;
    Stage stage{Stage::Queued};
    bool retired{};
};
struct Slot {
    std::uint64_t generation{};
    std::unique_ptr<Record> record;
};
std::size_t Bytes(const Request& request) {
    return sizeof(Request) + request.id.name.size() + request.source.size();
}
void ValidateText(const std::string& value, std::size_t limit, const char* field) {
    if (value.empty() || value.size() > limit || value.find('\0') != std::string::npos)
        throw std::invalid_argument(std::string("invalid resource ") + field);
}
bool Finite(Vec2 value) { return std::isfinite(value.x) && std::isfinite(value.y); }
std::uint64_t AllocateIdentity() {
    static std::atomic<std::uint64_t> next{1};
    auto value = next.load(std::memory_order_relaxed);
    for (;;) {
        if (value == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("resource service identity exhausted");
        if (next.compare_exchange_weak(value, value + 1, std::memory_order_relaxed)) return value;
    }
}

struct Core {
    explicit Core(std::unique_ptr<Provider> provider_, Limits limits_)
        : provider(std::move(provider_)), limits(limits_), thread(std::this_thread::get_id()) {
        if (!provider || !limits.manifests || !limits.activeJobs ||
            !limits.records || limits.records > std::numeric_limits<std::uint32_t>::max() ||
            !limits.declarations || !limits.declarationBytes ||
            limits.activeJobs > std::numeric_limits<std::size_t>::max() / 2)
            throw std::invalid_argument("invalid resource service limits/provider");
        // Reserve table/slot capacity once, not on the final publication frame.
        slots.reserve(limits.records);
        freeSlots.reserve(limits.records);
        index.reserve(limits.records);
        maintenanceJob = commitJob = jobs.end();
    }
    std::unique_ptr<Provider> provider;
    const std::uint64_t identity{AllocateIdentity()};
    Limits limits;
    std::thread::id thread;
    bool running{true}, ticking{};
    Stats stats;
    std::list<std::shared_ptr<ManifestState>> manifests;
    std::list<std::unique_ptr<Work>> jobs;
    using JobCursor = decltype(jobs)::iterator;
    JobCursor maintenanceJob, commitJob;
    std::vector<Slot> slots;
    std::vector<std::uint32_t> freeSlots;
    // One namespace for every resource kind; names are reserved at Submit.
    std::unordered_map<std::string, Handle> index;
    std::size_t pollCursor{}, pollEnd{}, recordCursor{}, recordEnd{}, maintenancePhase{};

    void Owner() const {
        if (thread != std::this_thread::get_id())
            throw std::logic_error("resource service must be accessed on its owner thread");
    }
    void Running() const {
        Owner();
        if (!running) throw std::logic_error("resource service has shut down");
        if (ticking) throw std::logic_error("reentrant resource service access from provider");
    }
    Record* Get(Handle handle) {
        if (handle.owner != identity || handle.slot >= slots.size()) return nullptr;
        auto& slot = slots[handle.slot];
        return slot.generation == handle.generation ? slot.record.get() : nullptr;
    }
    void Retire(Record& record) {
        if (record.retired) return;
        record.retired = true;
        ++stats.pendingRetirement;
        if (record.asset) record.asset->Retire();
        const auto found = index.find(record.request.id.name);
        // Delayed retirement of an old generation must never erase its reload.
        if (found != index.end() && found->second == record.handle) index.erase(found);
    }
    bool Refresh(Record& record) {
        if (record.retired) return false;
        // The shared transaction is the publication gate, not a Lua handle.
        // Failed/cancelled reservations are immediately reclaimable by every
        // lookup/Submit, even before budgeted physical retirement has run.
        if (Terminal(record.transaction->state) && !record.transaction->committedEver)
            Retire(record);
        return !record.retired && record.transaction->committedEver && record.asset != nullptr;
    }
    Record* Lookup(const std::string& name) {
        const auto found = index.find(name);
        if (found == index.end()) return nullptr;
        auto* record = Get(found->second);
        if (record) Refresh(*record); // May erase the index, not the Record.
        return record && !record->retired ? record : nullptr;
    }
    void Cancel(Transaction& transaction) {
        if (!Terminal(transaction.state)) {
            transaction.state = State::Cancelled;
            --stats.activeJobs;
            ++stats.cancellations;
            ++stats.pendingRetirement;
        }
    }
    bool BeginFailure(Transaction& transaction) noexcept {
        if (Terminal(transaction.state)) return false;
        transaction.state = State::Failed;
        --stats.activeJobs;
        ++stats.pendingRetirement;
        return true;
    }
    void Fail(Transaction& transaction, Error error) {
        if (BeginFailure(transaction)) transaction.error = std::move(error);
    }
    void RemoveEntry(std::list<Request>& entries) {
        stats.declarationBytes -= Bytes(entries.front());
        --stats.declarations;
        entries.pop_front();
    }
    void Append(ManifestState& manifest, Request request) {
        Running();
        if (manifest.sealed) throw std::logic_error("resource manifest is sealed");
        ValidateText(request.id.name, 512, "name");
        ValidateText(request.source, 4096, "source path");
        if (manifest.names.contains(request.id.name))
            throw std::invalid_argument("duplicate resource name in loader: " + request.id.name);
        const auto bytes = Bytes(request);
        if (stats.declarations >= limits.declarations ||
            bytes > limits.declarationBytes - stats.declarationBytes)
            throw std::length_error("resource declaration capacity exceeded");
        const auto name = manifest.names.insert(request.id.name).first;
        try { manifest.entries.push_back(std::move(request)); }
        catch (...) { manifest.names.erase(name); throw; }
        ++stats.declarations;
        stats.declarationBytes += bytes;
    }
    Handle Reserve(const Request& request, const std::shared_ptr<Transaction>& transaction) {
        if (stats.records >= limits.records) throw std::length_error("resource record capacity exceeded");
        auto record = std::make_unique<Record>();
        record->request = request;
        record->transaction = transaction;
        std::uint32_t slotIndex{};
        if (freeSlots.empty()) {
            slotIndex = static_cast<std::uint32_t>(slots.size());
            slots.emplace_back();
        } else {
            slotIndex = freeSlots.back();
            freeSlots.pop_back();
        }
        auto& slot = slots[slotIndex];
        if (slot.generation == std::numeric_limits<std::uint64_t>::max()) {
            freeSlots.push_back(slotIndex);
            throw std::overflow_error("resource generation exhausted");
        }
        record->handle = {slotIndex, ++slot.generation, identity};
        const auto handle = record->handle;
        try {
            if (!index.emplace(record->request.id.name, handle).second)
                throw std::logic_error("duplicate resource reservation");
        } catch (...) {
            freeSlots.push_back(slotIndex);
            throw;
        }
        slot.record = std::move(record);
        ++stats.records;
        return handle;
    }
    JobCursor NextJob(JobCursor& cursor) {
        if (jobs.empty()) return jobs.end();
        if (cursor == jobs.end()) cursor = jobs.begin();
        return cursor++;
    }
    void EraseJob(JobCursor entry) {
        const auto next = std::next(entry);
        if (maintenanceJob == entry) maintenanceJob = next;
        if (commitJob == entry) commitJob = next;
        jobs.erase(entry);
    }
    Record* NextRecord(std::size_t& cursor, std::size_t& end) {
        if (slots.empty()) return nullptr;
        // Freeze the pass end. Otherwise one new slot per tick plus one poll
        // per tick could chase new entries forever without revisiting old work.
        if (cursor >= end) { cursor = 0; end = slots.size(); }
        return slots[cursor++].record.get();
    }
    void ProviderFailed(Record& record, Error error) {
        record.error = std::move(error);
        Fail(*record.transaction, *record.error);
    }
    void PollOne() {
        auto* record = NextRecord(pollCursor, pollEnd);
        if (!record) return;
        Refresh(*record);
        if (record->retired || record->asset || record->error) return;
        record->transaction->state = State::Loading;
        try {
            if (!record->operation) {
                record->operation = provider->Start(record->handle, record->request);
                ++stats.starts;
                if (!record->operation) throw std::runtime_error("provider returned null operation");
                return; // Scheduling and polling consume different budget units.
            }
            auto result = record->operation->Poll();
            ++stats.polls;
            record->stage = result.stage;
            if (result.error) {
                result.error->id = record->request.id;
                result.error->file = record->request.source;
                result.error->stage = result.stage;
                ProviderFailed(*record, std::move(*result.error));
            } else if (result.stage == Stage::Prepared) {
                if (!result.asset) throw std::runtime_error("provider Prepared without payload");
                record->asset = std::move(result.asset);
                // Count each request exactly once, only after its provider's
                // CPU preparation and any GPU fences have actually completed.
                ++record->transaction->progress.prepared;
            } else if (result.asset) {
                throw std::runtime_error("provider supplied payload before Prepared");
            }
        } catch (const std::exception& exception) {
            ProviderFailed(*record, Error{record->request.id, record->request.source,
                "provider_failed", exception.what(), record->stage});
        } catch (...) {
            ProviderFailed(*record, Error{record->request.id, record->request.source,
                "provider_failed", "unknown provider exception", record->stage});
        }
    }
    void MaintainJob() {
        const auto entry = NextJob(maintenanceJob);
        if (entry == jobs.end()) return;
        auto& work = **entry;
        auto& transaction = *work.transaction;
        if (Terminal(transaction.state)) {
            if (!work.resources.empty()) work.resources.pop_front();
            else {
                --stats.pendingRetirement;
                EraseJob(entry);
                return;
            }
        } else if (transaction.state != State::Publishing) {
            // Bounded readiness scan; one gate publishes the whole batch.
            if (!work.scanning) {
                work.scan = work.resources.begin();
                work.preparedInPass = 0;
                work.scanning = true;
            }
            if (work.scan != work.resources.end()) {
                auto* record = Get(*work.scan++);
                if (!record || record->retired) {
                    Fail(transaction, {{}, {}, "resource_retired", "reserved generation is no longer valid", Stage::Queued});
                } else if (record->error) Fail(transaction, *record->error);
                else if (record->asset) ++work.preparedInPass;
            }
            if (!Terminal(transaction.state) && work.scan == work.resources.end()) {
                if (work.preparedInPass == work.resources.size())
                    transaction.state = State::Publishing;
                work.scanning = false;
            }
        }
    }
    void MaintainRecord() {
        auto* record = NextRecord(recordCursor, recordEnd);
        if (!record) return;
        Refresh(*record);
        if (record->retired) {
            if (record->operation) {
                record->operation->Cancel();
                record->operation.reset();
            } else {
                const auto slot = record->handle.slot;
                // This release is owner-thread deferred; real providers must
                // themselves protect in-flight use and observe their byte budget.
                slots[slot].record.reset();
                freeSlots.push_back(slot);
                --stats.records;
                --stats.pendingRetirement;
                ++stats.retirements;
            }
            return;
        }
        if ((record->asset || record->error) && record->operation) {
            // Completed operations no longer own registration or residency.
            // Failed work is cancelled explicitly before its wrapper is retired.
            if (record->error) record->operation->Cancel();
            record->operation.reset();
        }
    }
    void MaintainManifest() {
        if (manifests.empty()) return;
        auto& manifest = manifests.front();
        if (manifest.use_count() == 1) {
            if (!manifest->entries.empty()) {
                manifest->names.erase(manifest->entries.front().id.name);
                RemoveEntry(manifest->entries);
            }
            else { manifests.pop_front(); --stats.manifests; return; }
        }
        manifests.splice(manifests.end(), manifests, manifests.begin());
    }
    void CommitOne() {
        const auto entry = NextJob(commitJob);
        if (entry == jobs.end()) return;
        auto& transaction = *(*entry)->transaction;
        if (transaction.state == State::Publishing) {
            // No resource/member traversal or provider calls at publication.
            transaction.committedEver = true;
            transaction.state = State::Ready;
            --stats.activeJobs;
            ++stats.commits;
            ++stats.pendingRetirement;
        }
    }
};

std::shared_ptr<Core> Require(const std::weak_ptr<Core>& owner) {
    auto core = owner.lock();
    if (!core) throw std::logic_error("resource service has been destroyed");
    core->Running();
    return core;
}
}

using namespace detail;

Manifest& Manifest::LoadImage(std::string path, std::string name, std::uint32_t x,
    std::uint32_t y, std::uint32_t width, std::uint32_t height, ImageOptions options) {
    ImageRequest request;
    request.x = x; request.y = y; request.width = width; request.height = height;
    request.options = options;
    return LoadImage(std::move(path), std::move(name), std::move(request));
}
Manifest& Manifest::LoadImage(std::string path, std::string name, ImageRequest request) {
    auto core = Require(owner);
    if (!state) throw std::logic_error("invalid manifest");
    auto& options = request.options;
    const auto finite = [](const std::optional<double>& value) { return !value || std::isfinite(*value); };
    if (!std::isfinite(request.x) || !std::isfinite(request.y) || request.x < 0 || request.y < 0 ||
        !finite(request.width) || !finite(request.height) ||
        (request.width && (*request.width <= 0 || !std::isfinite(request.x + *request.width))) ||
        (request.height && (*request.height <= 0 || !std::isfinite(request.y + *request.height))) ||
        !finite(request.centerX) || !finite(request.centerY) || !Finite(options.scale) ||
        (options.anchor && !Finite(*options.anchor)) ||
        (options.filter != Filter::Linear && options.filter != Filter::Point) ||
        (options.address != Address::Clamp && options.address != Address::Repeat))
        throw std::invalid_argument("invalid image region/options");
    // Native helper centers use the same independent axes as the Lua request.
    // Omission stays explicit until source dimensions are known; no rounding.
    if (options.anchor) {
        if (!request.centerX) request.centerX = options.anchor->x;
        if (!request.centerY) request.centerY = options.anchor->y;
        options.anchor.reset();
    }
    core->Append(*state, {{Kind::Image, std::move(name)}, std::move(path), std::move(request)});
    return *this;
}
Manifest& Manifest::LoadAudio(std::string path, std::string name, AudioRequest options) {
    auto core = Require(owner);
    if (!state) throw std::logic_error("invalid manifest");
    if ((options.mode != AudioMode::Decoded && options.mode != AudioMode::Stream) ||
        (options.loop && (!std::isfinite(options.loop->start) || options.loop->start < 0 ||
            (options.loop->finish && (!std::isfinite(*options.loop->finish) || *options.loop->finish <= options.loop->start)))))
        throw std::invalid_argument("invalid audio loading options");
    core->Append(*state, {{options.mode == AudioMode::Decoded ? Kind::Sound : Kind::Music,
        std::move(name)}, std::move(path), options});
    return *this;
}
Manifest& Manifest::LoadImage(std::string path, std::string name, ImageOptions options) {
    ImageRequest request;
    request.options = options;
    return LoadImage(std::move(path), std::move(name), std::move(request));
}
bool Manifest::IsSealed() const {
    auto core = Require(owner);
    if (!state) throw std::logic_error("invalid manifest");
    return state->sealed;
}
Manifest& Manifest::LoadFont(std::string path, std::string name) {
    auto core = Require(owner);
    if (!state) throw std::logic_error("invalid manifest");
    core->Append(*state, {{Kind::Font, std::move(name)}, std::move(path), FileRequest{}});
    return *this;
}
Manifest& Manifest::LoadShader(std::string path, std::string name) {
    auto core = Require(owner);
    if (!state) throw std::logic_error("invalid manifest");
    core->Append(*state, {{Kind::Shader, std::move(name)}, std::move(path), FileRequest{}});
    return *this;
}
State Job::GetState() const {
    if (!state) throw std::logic_error("invalid resource job");
    if (auto core = owner.lock()) core->Owner();
    return state->state;
}
bool Job::IsFinished() const { return Terminal(GetState()); }
bool Job::Cancel() {
    if (IsFinished()) return false;
    auto core = Require(owner);
    core->Cancel(*state);
    return true;
}
Progress Job::GetProgress() const { (void)GetState(); return state->progress; }
std::optional<Error> Job::GetError() const { (void)GetState(); return state->error; }
void Job::CheckResult() const {
    const auto current = GetState();
    if (current == State::Ready) return;
    if (current == State::Failed) {
        if (!state->error) throw std::runtime_error("resource loading failed");
        const auto& error = *state->error;
        throw std::runtime_error("resource '" + (error.id ? error.id->name : std::string{"?"}) +
            "' (" + error.file + "): " + error.message);
    }
    if (current == State::Cancelled) throw std::runtime_error("resource loading cancelled");
    throw std::logic_error("resource loading has not finished");
}

Service::Service(std::unique_ptr<Provider> provider, Limits limits)
    : core(std::make_shared<Core>(std::move(provider), limits)) {}
Service::~Service() {
    // Service lifetime is an owner-thread contract, like the renderer/device.
    if (core->thread != std::this_thread::get_id()) std::terminate();
    Shutdown();
}
Manifest Service::CreateManifest() {
    core->Running();
    // Failed synchronous calls may release their scratch manifest before the
    // next engine tick. Reclaim empty/single-request orphans on admission so
    // repeated argument/name errors cannot prevent the next valid load. This
    // only retires declarations: no provider polling, Lua or GPU frame reentry.
    // Two bounded sweeps leave large abandoned batches to regular maintenance.
    if (core->stats.manifests >= core->limits.manifests) {
        const auto count = core->manifests.size();
        for (int sweep = 0; sweep < 2; ++sweep)
            for (std::size_t i = 0; i < count; ++i) core->MaintainManifest();
    }
    if (core->stats.manifests >= core->limits.manifests) throw std::length_error("resource manifest capacity exceeded");
    auto state = std::make_shared<ManifestState>();
    core->manifests.push_back(state);
    ++core->stats.manifests;
    Manifest result;
    result.owner = core;
    result.state = std::move(state);
    return result;
}
Job Service::Submit(Manifest& manifest) {
    core->Running();
    if (manifest.owner.lock() != core || !manifest.state)
        throw std::invalid_argument("manifest belongs to a different resource service");
    if (manifest.state->sealed) throw std::logic_error("resource manifest is sealed");
    // Empty accepted batches have nothing to prepare or retire, and must not
    // wait for a host Tick or consume the work queue just to publish no names.
    if (manifest.state->entries.empty()) {
        auto transaction = std::make_shared<Transaction>();
        transaction->committedEver = true;
        transaction->state = State::Ready;
        manifest.state->sealed = true;
        ++core->stats.commits;
        Job result;
        result.owner = core; result.state = std::move(transaction);
        return result;
    }
    // Check before seal, so a full queue is retryable with the same manifest.
    if (core->stats.activeJobs >= core->limits.activeJobs || core->jobs.size() >= core->limits.activeJobs * 2)
        throw std::length_error("resource_queue_full");
    // Preflight the entire name set before creating any records. This check is
    // independent of decode speed, and shared sources never imply shared names.
    for (const auto& request : manifest.state->entries) {
        if (core->Lookup(request.id.name))
            throw std::invalid_argument("resource name already registered or reserved: " + request.id.name);
    }
    if (manifest.state->entries.size() > core->limits.records - core->stats.records)
        throw std::length_error("resource record capacity exceeded");
    auto transaction = std::make_shared<Transaction>();
    transaction->progress.requests = manifest.state->entries.size();
    auto work = std::make_unique<Work>();
    work->transaction = transaction;
    // All fallible allocation precedes sealing. A failed Submit leaves the
    // caller's manifest intact and removes every reservation made by this call.
    try {
        for (const auto& request : manifest.state->entries) {
            work->resources.push_back({});
            try { work->resources.back() = core->Reserve(request, transaction); }
            catch (...) { work->resources.pop_back(); throw; }
        }
        // Allocate the list node before transferring ownership of work.
        core->jobs.push_back(nullptr);
    } catch (...) {
        for (const auto handle : work->resources) {
            auto* record = core->Get(handle);
            core->index.erase(record->request.id.name);
            core->slots[handle.slot].record.reset();
            core->freeSlots.push_back(handle.slot);
            --core->stats.records;
        }
        throw;
    }
    core->jobs.back() = std::move(work);
    transaction->progress.registered = transaction->progress.requests;
    while (!manifest.state->entries.empty()) core->RemoveEntry(manifest.state->entries);
    manifest.state->names.clear();
    manifest.state->sealed = true;
    ++core->stats.activeJobs;
    Job result;
    result.owner = core;
    result.state = std::move(transaction);
    return result;
}
bool Service::Unload(const Id& id) {
    core->Running();
    auto* record = core->Lookup(id.name);
    if (!record || record->request.id.kind != id.kind)
        throw std::invalid_argument("resource not found or wrong type: " + id.name);
    return Unload(id.name);
}
bool Service::Unload(const std::string& name) {
    core->Running();
    auto* record = core->Lookup(name);
    if (!record) throw std::invalid_argument("resource not found: " + name);
    if (!core->Refresh(*record)) throw std::logic_error("resource is still loading: " + name);
    core->Retire(*record); // Immediately revoke this logical generation only.
    return true;
}
void Service::Tick(Budget budget) {
    core->Running();
    core->ticking = true;
    struct Exit { Core& core; ~Exit() { core.ticking = false; } } exit{*core};
    for (std::size_t i = 0; i < budget.polls; ++i) core->PollOne();
    for (std::size_t i = 0; i < budget.maintenance; ++i) {
        switch (core->maintenancePhase++ % 3) {
        case 0: core->MaintainJob(); break;
        case 1: core->MaintainRecord(); break;
        default: core->MaintainManifest(); break;
        }
    }
    for (std::size_t i = 0; i < budget.commits; ++i) core->CommitOne();
}
std::optional<Handle> Service::Find(const Id& id) {
    const auto handle = Find(id.name);
    if (handle && core->Get(*handle)->request.id.kind != id.kind) return {};
    return handle;
}
std::optional<Handle> Service::Find(const std::string& name) {
    core->Running();
    auto* record = core->Lookup(name);
    if (!record) return {};
    if (!core->Refresh(*record)) throw std::logic_error("resource is still loading: " + name);
    return record->handle;
}
std::optional<Kind> Service::PublishedKind(const std::string& name) const {
    core->Running();
    const auto found = core->index.find(name);
    if (found == core->index.end()) return {};
    const auto* record = core->Get(found->second);
    // Do not use Lookup/Refresh: this public observation must neither advance
    // a batch nor retire its private resources as a side effect of querying.
    if (!record || record->retired || !record->transaction->committedEver || !record->asset) return {};
    return record->request.id.kind;
}
bool Service::IsResident(Handle handle) {
    core->Running();
    auto* record = core->Get(handle);
    return record && core->Refresh(*record);
}
std::shared_ptr<const Asset> Service::Acquire(Handle handle) {
    if (!IsResident(handle)) throw std::invalid_argument("resource handle is stale or not resident");
    return core->Get(handle)->asset;
}
Request Service::GetInfo(Handle handle) {
    if (!IsResident(handle)) throw std::invalid_argument("resource handle is stale or not resident");
    return core->Get(handle)->request;
}
void Service::ReplaceAsset(Handle handle, std::shared_ptr<const Asset> asset) {
    if (!asset) throw std::invalid_argument("replacement resource payload is null");
    if (!IsResident(handle)) throw std::invalid_argument("resource handle is stale or not resident");
    core->Get(handle)->asset = std::move(asset);
}
Stats Service::GetStats() const { core->Owner(); return core->stats; }
void Service::Shutdown() {
    core->Owner();
    if (!core->running) return;
    if (core->ticking) throw std::logic_error("cannot shut down resource service from provider");
    core->running = false;
    for (auto& work : core->jobs) core->Cancel(*work->transaction);
    for (auto& slot : core->slots) {
        if (slot.record) core->Retire(*slot.record);
        if (slot.record && slot.record->operation) slot.record->operation->Cancel();
    }
    core->provider->Shutdown(); // Join/drain before destroying anonymous payloads.
    core->jobs.clear();
    // External Manifest wrappers may outlive Service. Leave only their small
    // sealed state, not a declaration list whose later GC could do bulk work.
    for (auto& manifest : core->manifests) {
        manifest->entries.clear();
        manifest->names.clear();
        manifest->sealed = true;
    }
    core->manifests.clear();
    core->index.clear();
    core->slots.clear();
    core->freeSlots.clear();
    core->stats = {};
}
}
