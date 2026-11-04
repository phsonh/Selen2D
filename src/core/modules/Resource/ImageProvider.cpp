#include "ImageProvider.h"
#include "core/modules/Graphic/image/MipChain.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <list>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace core::modules::Resource::Images {
namespace Mip = core::modules::Graphic::Image::MipChain;
namespace {
struct Gate {
    const std::thread::id owner{std::this_thread::get_id()};
    bool active{true}, ticking{};
    void Owner() const {
        if (owner != std::this_thread::get_id()) throw std::logic_error("ImageProvider requires its owner thread");
    }
};
struct Node {
    Handle handle;
    Request request;
    // Owner-thread normalization happens only while this node is still pending.
    // Once admitted to a Wave this is immutable, including for CPU workers.
    ResolvedImage image;
    Atlas::Rect coverage; // Integer source pixels containing the exact logical region.
    bool dimensionsResolved{};
    std::optional<SourceTicket> dimensions;
    std::atomic_bool cancelled{}; // The ONLY mutable field read by CPU workers.
    Stage stage{Stage::Queued};
    bool done{}, scheduled{}; // Owner thread; admission includes physical queue residency.
    std::optional<Error> error;
    std::shared_ptr<const Asset> asset;
};
bool Live(const Node& node) { return !node.done && !node.cancelled.load(std::memory_order_acquire); }
class ImageOperation final : public Operation {
public:
    ImageOperation(std::shared_ptr<Gate> gate_, std::shared_ptr<Node> node_)
        : gate(std::move(gate_)), node(std::move(node_)) {}
    ~ImageOperation() override { Cancel(); }
    PollResult Poll() override {
        gate->Owner();
        if (!gate->active || node->cancelled.load(std::memory_order_acquire))
            return {node->stage, {}, Error{node->request.id, node->request.source,
                "image_cancelled", "Image preparation was cancelled or provider shut down", node->stage}};
        return {node->stage, node->asset, node->error};
    }
    void Cancel() noexcept override {
        if (gate->owner != std::this_thread::get_id()) std::terminate();
        // Service destroys a Prepared operation after copying its Asset. This
        // must not undo a successful hand-off or cancel other page consumers.
        if (!node->done) {
            node->cancelled.store(true, std::memory_order_release);
            node->dimensions.reset(); // Release a pending whole-source subscription promptly.
        }
    }
private:
    std::shared_ptr<Gate> gate;
    std::shared_ptr<Node> node;
};
enum class WaveKind { Atlas, Crop, MipAtlas, MipCrop };
bool Packed(WaveKind kind) { return kind == WaveKind::Atlas || kind == WaveKind::MipAtlas; }
enum class Phase { Gather, Layout, LayoutRunning, Source, SourceRunning, Copy, CopyRunning, Complete };
struct Pixels {
    Jobs::Reservation credit; // Pixel storage dies before admission is returned.
    std::vector<std::uint8_t> bytes;
    std::uint32_t width{}, height{};
};
struct Entry {
    Atlas::Placement placement;
    Mip::Layout mip;
    std::uint32_t sourceWidth{}, sourceHeight{};
    bool copied{};
    std::string error, code;
};
struct BuildData {
    std::size_t accepted{};
    Atlas::Plan plan;
    std::vector<Entry> entries;
    std::vector<std::shared_ptr<Pixels>> pages;
};
struct PageTransfer {
    std::optional<Upload::Ticket> ticket;
    std::string error;
};
struct Wave {
    WaveKind kind{};
    Phase phase{Phase::Gather};
    std::vector<std::shared_ptr<Node>> nodes;
    std::uint64_t pixels{}, opened{};
    std::shared_ptr<BuildData> data = std::make_shared<BuildData>();
    std::optional<Jobs::Ticket> task;
    std::optional<SourceTicket> source;
    std::shared_ptr<const SourcePixels> sourcePixels;
    std::vector<std::size_t> sourceIndices;
    std::vector<bool> sourceDone;
    std::vector<PageTransfer> transfers;
    std::size_t uploadCursor{};
};
bool Cancelled(void* value) noexcept { return static_cast<const Jobs::StopToken*>(value)->IsStopRequested(); }
void* CancelData(const Jobs::StopToken& stop) { return const_cast<Jobs::StopToken*>(&stop); }
std::string Explain(std::exception_ptr error) {
    try { if (error) std::rethrow_exception(error); }
    catch (const std::exception& exception) { return exception.what(); }
    catch (...) { return "unknown native image preparation failure"; }
    return "native image preparation was cancelled";
}
std::shared_ptr<Pixels> Allocate(std::uint32_t width, std::uint32_t height,
    Jobs::MemoryBudget memory, std::size_t limit, const Jobs::StopToken& stop) {
    const auto bytes = std::uint64_t(width) * height * 4;
    if (!width || !height || bytes > limit || bytes > std::numeric_limits<std::size_t>::max())
        throw std::length_error("image backing exceeds wave byte limit");
    auto result = std::make_shared<Pixels>();
    result->credit = memory.Acquire(static_cast<std::size_t>(bytes), stop);
    stop.Check();
    result->bytes.resize(static_cast<std::size_t>(bytes)); // Zeroed on worker.
    result->width = width; result->height = height;
    stop.Check();
    return result;
}
void Layout(const std::vector<std::shared_ptr<Node>>& nodes, const std::shared_ptr<BuildData>& data,
    WaveKind kind, Config config, Jobs::MemoryBudget memory, const Jobs::StopToken& stop) {
    std::vector<Atlas::Item> items;
    std::vector<Mip::Layout> chains;
    items.reserve(nodes.size());
    for (const auto& node : nodes) {
        const auto& image = node->coverage;
        if (kind == WaveKind::MipAtlas) {
            chains.push_back(Mip::Describe(image.width, image.height, config.atlas.extrusion));
            items.push_back({chains.back().width, chains.back().height});
        } else items.push_back({image.width, image.height});
    }
    // Each level already has its own guard pixels inside the chain rectangle.
    if (kind == WaveKind::MipAtlas) config.atlas.extrusion = 0;
    config.atlas.maxOutputBytes = std::min<std::uint64_t>(config.atlas.maxOutputBytes, config.maxWaveBytes);
    // A Manifest is not one indivisible atlas. Split a finite wave BEFORE
    // admitting pixels if its layout/work/page caps require a smaller prefix.
    for (;;) {
        stop.Check();
        try { data->plan = Atlas::Pack(items, config.atlas, Cancelled, CancelData(stop)); break; }
        catch (const std::length_error& error) {
            if (items.size() == 1) {
                // An individually impossible request must not poison other
                // nodes merely because they were collected into this wave.
                data->accepted = 1;
                data->entries.resize(1);
                data->entries[0].error = error.what();
                data->entries[0].code = "image_layout";
                return;
            }
            items.resize(std::max<std::size_t>(1, items.size() / 2));
        }
    }
    data->accepted = items.size();
    data->entries.resize(items.size());
    data->pages.reserve(data->plan.pages.size());
    for (std::size_t i = 0; i < items.size(); ++i) {
        auto& entry = data->entries[i];
        entry.placement = data->plan.placements[i];
        if (kind == WaveKind::MipAtlas) {
            entry.mip = chains[i];
            const auto base = entry.mip.regions[0];
            entry.placement.content = {entry.placement.allocation.x + base.x,
                entry.placement.allocation.y + base.y, base.width, base.height};
        }
    }
    for (const auto& page : data->plan.pages)
        data->pages.push_back(Allocate(page.width, page.height, memory, config.maxWaveBytes, stop));
}
void ValidateRegion(const ResolvedImage& image, const SourcePixels& source) {
    if (!std::isfinite(image.x) || !std::isfinite(image.y) || !std::isfinite(image.width) || !std::isfinite(image.height) ||
        image.x < 0 || image.y < 0 || image.width <= 0 || image.height <= 0 ||
        image.x >= source.image.width || image.y >= source.image.height ||
        image.width > source.image.width || image.height > source.image.height ||
        image.x > source.image.width - image.width || image.y > source.image.height - image.height)
        throw std::invalid_argument("image region is outside the decoded source");
}
void CopySource(const std::vector<std::shared_ptr<Node>>& nodes, const std::vector<std::size_t>& indices,
    const std::shared_ptr<BuildData>& data, const std::shared_ptr<const SourcePixels>& source,
    WaveKind kind, Config config, Jobs::MemoryBudget memory, const Jobs::StopToken& stop) {
    const auto& pixels = source->image;
    const Atlas::RgbaView view{pixels.Bytes(), pixels.width, pixels.height, std::size_t(pixels.width) * 4};
    std::size_t valid{};
    for (const auto index : indices) {
        stop.Check();
        if (nodes[index]->cancelled.load(std::memory_order_acquire)) continue;
        auto& entry = data->entries[index];
        entry.sourceWidth = pixels.width; entry.sourceHeight = pixels.height;
        try { ValidateRegion(nodes[index]->image, *source); ++valid; }
        catch (const std::invalid_argument& error) { entry.error = error.what(); entry.code = "image_region"; }
    }
    if (!valid) return;
    for (const auto index : indices) {
        stop.Check();
        if (nodes[index]->cancelled.load(std::memory_order_acquire)) continue;
        auto& entry = data->entries[index];
        if (!entry.error.empty()) continue;
        const auto& image = nodes[index]->coverage;
        const Atlas::Rect region{image.x, image.y, image.width, image.height};
        if (!Packed(kind)) {
            const Atlas::Rect crop{0, 0, image.width, image.height};
            entry.placement = {data->pages.size(), crop, crop};
            data->pages.push_back(Allocate(image.width, image.height, memory, config.maxWaveBytes, stop));
        }
        auto& page = *data->pages[entry.placement.page];
        if (kind == WaveKind::MipAtlas) {
            Mip::Copy(page.bytes, page.width, page.height, entry.placement.allocation, view, region,
                entry.mip, Cancelled, CancelData(stop));
        } else {
            // Large mip images keep a standalone hardware chain, generated
            // only from this crop; never downsample a whole source sheet.
            Atlas::CopyExtruded(page.bytes, page.width, page.height, entry.placement, view, region,
                kind == WaveKind::Atlas ? config.atlas.extrusion : 0, Cancelled, CancelData(stop));
        }
        entry.copied = true;
    }
}
}

struct Provider::Impl {
    Jobs::Executor& executor;
    ImageSources& sources;
    Upload::Queue& uploads;
    Jobs::MemoryBudget memory;
    Config config;
    std::shared_ptr<Gate> gate = std::make_shared<Gate>();
    std::list<std::shared_ptr<Node>> nodes, pending;
    std::list<std::shared_ptr<Node>>::iterator cleanup{nodes.end()};
    std::shared_ptr<Wave> builder, uploading;
    Stats stats;
    std::uint64_t ticks{};
    std::size_t scheduling{};

    Impl(Jobs::Executor& executor_, ImageSources& sources_, Upload::Queue& uploads_, Jobs::MemoryBudget memory_, Config config_)
        : executor(executor_), sources(sources_), uploads(uploads_), memory(std::move(memory_)), config(config_) {
        const auto& a = config.atlas;
        const auto power2 = [](std::uint32_t value) { return value && !(value & (value - 1)); };
        if (!config.capacity || !config.waveItems || config.waveItems > config.capacity ||
            config.waveItems > 8192 || !config.activeWaves || config.activeWaves > 2 ||
            !config.wavePixels || !config.maxWaveBytes || config.maxWaveBytes > std::numeric_limits<std::size_t>::max() / 2 ||
            memory.GetStats().limit < config.maxWaveBytes * 2 ||
            !power2(a.targetSide) || !power2(a.maxSide) || a.targetSide > a.maxSide || a.maxSide > 16384 ||
            std::uint64_t(a.extrusion) * 2 >= a.maxSide || !a.maxItems || !a.maxPages || !a.maxFreeRects || !a.maxWork || !a.maxOutputBytes)
            throw std::invalid_argument("invalid ImageProvider limits or page memory (requires at least two wave budgets)");
    }
    void Running() const {
        gate->Owner();
        if (!gate->active) throw std::logic_error("ImageProvider has shut down");
        if (gate->ticking) throw std::logic_error("ImageProvider access must not be reentrant");
    }
    void Fail(Node& node, std::string code, std::string message) {
        if (!Live(node)) return;
        node.error = Error{node.request.id, node.request.source, std::move(code), std::move(message), node.stage};
        node.done = true;
    }
    void FailWave(Wave& wave, const std::string& code, const std::string& message) {
        for (auto& node : wave.nodes) Fail(*node, code, message);
    }
    void StageWave(Wave& wave, Stage stage) {
        for (auto& node : wave.nodes) if (Live(*node)) node->stage = stage;
    }
    WaveKind KindOf(const Node& node) const {
        const auto& image = node.coverage;
        if (node.image.options.mipmap) {
            if (std::uint64_t(image.width) + config.atlas.extrusion * 2ULL > config.atlas.maxSide ||
                std::uint64_t(image.height) + config.atlas.extrusion * 2ULL > config.atlas.maxSide)
                return WaveKind::MipCrop;
            const auto chain = Mip::Describe(image.width, image.height, config.atlas.extrusion);
            return chain.width <= config.atlas.maxSide && chain.height <= config.atlas.maxSide
                ? WaveKind::MipAtlas : WaveKind::MipCrop;
        }
        if (std::uint64_t(image.width) + config.atlas.extrusion * 2ULL > config.atlas.maxSide ||
            std::uint64_t(image.height) + config.atlas.extrusion * 2ULL > config.atlas.maxSide) return WaveKind::Crop;
        return WaveKind::Atlas;
    }
    bool ResolveDimensions(Node& node) {
        if (node.dimensionsResolved) return true;
        try {
            // TryRequest only schedules work or joins the shared source cache.
            // File access and PNG decode never run in Start or this owner tick.
            if (!node.dimensions) {
                node.dimensions = sources.TryRequest(node.request.source);
                if (!node.dimensions) return false;
            }
            const auto stage = node.dimensions->GetStage();
            if (stage == SourceStage::Failed || stage == SourceStage::Cancelled) {
                Fail(node, "image_source", Explain(node.dimensions->GetError()));
                node.dimensions.reset();
                return false;
            }
            if (stage != SourceStage::CpuReady) {
                node.stage = stage == SourceStage::Decoding ? Stage::Decoding : Stage::Reading;
                return false;
            }
            const auto source = node.dimensions->GetPixels();
            const auto& request = std::get<ImageRequest>(node.request.data);
            node.image = {request.x, request.y,
                request.width.value_or(source->image.width - request.x),
                request.height.value_or(source->image.height - request.y), request.options};
            ValidateRegion(node.image, *source);
            node.image.options.anchor = Vec2{
                request.centerX.value_or(node.image.width * 0.5),
                request.centerY.value_or(node.image.height * 0.5)};
            const auto left = static_cast<std::uint32_t>(std::floor(node.image.x));
            const auto top = static_cast<std::uint32_t>(std::floor(node.image.y));
            // A positive sub-pixel extent can be smaller than the ULP of its
            // origin. It still covers the origin pixel, even if x+w rounds to x.
            const auto right = std::max(left + 1,
                static_cast<std::uint32_t>(std::ceil(node.image.x + node.image.width)));
            const auto bottom = std::max(top + 1,
                static_cast<std::uint32_t>(std::ceil(node.image.y + node.image.height)));
            node.coverage = {left, top, right - left, bottom - top};
            node.dimensionsResolved = true;
            node.stage = Stage::CpuReady;
            // Do not pin every decoded source while gathering a Wave: that can
            // exhaust decode credit before layout/copy starts. ImageSources may
            // retain this result under its bounded cache, shared by later crops.
            node.dimensions.reset();
            return true;
        } catch (...) {
            Fail(node, "image_source", Explain(std::current_exception()));
            node.dimensions.reset();
            return false;
        }
    }
    void CleanupOne() {
        if (nodes.empty()) return;
        if (cleanup == nodes.end()) cleanup = nodes.begin();
        const auto entry = cleanup++;
        if (Live(**entry) || (*entry)->scheduled) return;
        ++stats.finished;
        if ((*entry)->cancelled.load(std::memory_order_acquire)) ++stats.cancelled;
        else if ((*entry)->error) ++stats.failed;
        nodes.erase(entry);
    }
    void Seal(Wave& wave) {
        // Gather can time out while the next whole-image source is decoding.
        // It belongs to the next wave, so do not let that subscription pin
        // decode credit while this wave still needs to read its own sources.
        if (!pending.empty()) pending.front()->dimensions.reset();
        if (!Packed(wave.kind)) {
            wave.data->entries.resize(wave.nodes.size());
            wave.sourceDone.resize(wave.nodes.size());
            wave.data->accepted = wave.nodes.size();
        }
        StageWave(wave, Stage::Packing);
        wave.phase = Packed(wave.kind) ? Phase::Layout : Phase::Source;
    }
    bool Abandoned(const Wave& wave) const {
        return !wave.nodes.empty() && std::none_of(wave.nodes.begin(), wave.nodes.end(), [](const auto& node) { return node && Live(*node); });
    }
    void RetireNodes(Wave& wave) {
        for (auto& node : wave.nodes) if (node) node->scheduled = false;
    }
    void RetireUploadNode(Wave& wave, std::size_t index) {
        if (!wave.nodes[index]) return;
        wave.nodes[index]->scheduled = false;
        wave.nodes[index].reset();
        const auto pageIndex = wave.data->entries[index].placement.page;
        if (pageIndex >= wave.transfers.size()) return;
        for (std::size_t i = 0; i < wave.nodes.size(); ++i)
            if (wave.nodes[i] && Live(*wave.nodes[i]) && wave.data->entries[i].copied &&
                wave.data->entries[i].placement.page == pageIndex) return;
        auto& transfer = wave.transfers[pageIndex];
        if (transfer.ticket) transfer.ticket->Cancel();
        transfer.ticket.reset();
        wave.data->pages[pageIndex].reset();
    }
    void CancelWork(Wave& wave) {
        if (wave.task) wave.task->Cancel();
        if (wave.source) wave.source->Cancel();
        wave.source.reset(); wave.sourcePixels.reset();
        for (auto& transfer : wave.transfers) if (transfer.ticket) transfer.ticket->Cancel();
        RetireNodes(wave);
    }
    void AdvanceCpu() {
        if (!builder) {
            if (pending.empty() || (uploading && config.activeWaves == 1)) return;
            if (!Live(*pending.front())) { pending.front()->scheduled = false; pending.pop_front(); return; }
            if (!ResolveDimensions(*pending.front())) return;
            builder = std::make_shared<Wave>();
            builder->kind = KindOf(*pending.front());
            builder->opened = ticks;
        }
        auto& wave = *builder;
        if (Abandoned(wave)) { CancelWork(wave); builder.reset(); return; }
        switch (wave.phase) {
        case Phase::Gather: {
            if (pending.empty() && wave.nodes.empty()) { builder.reset(); return; }
            if (!wave.nodes.empty() && ticks - wave.opened >= config.maxDelayTicks) { Seal(wave); return; }
            if (!pending.empty() && !Live(*pending.front())) { pending.front()->scheduled = false; pending.pop_front(); return; }
            if (!pending.empty() && !ResolveDimensions(*pending.front())) return;
            const bool compatible = !pending.empty() && KindOf(*pending.front()) == wave.kind;
            if (compatible && wave.nodes.size() < config.waveItems && (wave.nodes.empty() || wave.pixels < config.wavePixels)) {
                const auto& coverage = pending.front()->coverage;
                const auto area = std::uint64_t(coverage.width) * coverage.height;
                wave.nodes.push_back(std::move(pending.front())); pending.pop_front(); wave.pixels += area;
                if (!Packed(wave.kind) || wave.nodes.size() == config.waveItems || wave.pixels >= config.wavePixels) Seal(wave);
                return;
            }
            if (!wave.nodes.empty() && (!pending.empty() || ticks - wave.opened >= config.maxDelayTicks)) Seal(wave);
            return;
        }
        case Phase::Layout: {
            const auto data = wave.data;
            auto task = executor.TrySubmit([input = wave.nodes, data, kind = wave.kind, options = config, memory = memory](const Jobs::StopToken& stop) {
                Layout(input, data, kind, options, memory, stop);
            });
            if (task) { wave.task = std::move(task); wave.phase = Phase::LayoutRunning; }
            return;
        }
        case Phase::LayoutRunning: {
            if (!wave.task->IsFinished()) return;
            if (wave.task->GetState() != Jobs::State::Succeeded) {
                FailWave(wave, "image_layout", Explain(wave.task->GetError())); RetireNodes(wave); builder.reset(); return;
            }
            // Requeue the unadmitted suffix without reading or decoding it.
            // Prepare allocating metadata before changing the adopted task or
            // queue, so an allocation exception cannot strand a half-moved wave.
            std::list<std::shared_ptr<Node>> suffix;
            for (std::size_t i = wave.data->accepted; i < wave.nodes.size(); ++i) suffix.push_back(wave.nodes[i]);
            std::vector<bool> sourceDone(wave.data->accepted);
            for (std::size_t i = 0; i < wave.data->accepted; ++i)
                if (!wave.data->entries[i].error.empty())
                    Fail(*wave.nodes[i], wave.data->entries[i].code, wave.data->entries[i].error);
            pending.splice(pending.begin(), suffix);
            wave.nodes.resize(wave.data->accepted); wave.sourceDone = std::move(sourceDone); wave.task.reset();
            wave.phase = Phase::Source;
            return;
        }
        case Phase::Source: {
            wave.sourceIndices.clear();
            std::string path;
            for (std::size_t i = 0; i < wave.nodes.size(); ++i) {
                if (wave.sourceDone[i] || !Live(*wave.nodes[i])) continue;
                if (wave.sourceIndices.empty()) path = wave.nodes[i]->request.source;
                if (wave.nodes[i]->request.source == path) wave.sourceIndices.push_back(i);
            }
            if (wave.sourceIndices.empty()) {
                wave.transfers.resize(wave.data->pages.size());
                wave.phase = Phase::Complete;
                ++stats.wavesBuilt;
                return;
            }
            try { wave.source = sources.TryRequest(std::move(path)); }
            catch (...) {
                const auto message = Explain(std::current_exception());
                for (const auto index : wave.sourceIndices) { Fail(*wave.nodes[index], "image_source", message); wave.sourceDone[index] = true; }
                return;
            }
            if (wave.source) wave.phase = Phase::SourceRunning;
            return;
        }
        case Phase::SourceRunning: {
            const auto state = wave.source->GetStage();
            const auto stage = state == SourceStage::Decoding ? Stage::Decoding : Stage::Reading;
            for (const auto index : wave.sourceIndices) if (Live(*wave.nodes[index])) wave.nodes[index]->stage = stage;
            if (state == SourceStage::Failed || state == SourceStage::Cancelled) {
                const auto message = Explain(wave.source->GetError());
                for (const auto index : wave.sourceIndices) { Fail(*wave.nodes[index], "image_source", message); wave.sourceDone[index] = true; }
                wave.source.reset(); wave.phase = Phase::Source; return;
            }
            if (state == SourceStage::CpuReady) {
                wave.sourcePixels = wave.source->GetPixels();
                wave.phase = Phase::Copy;
                for (const auto index : wave.sourceIndices) if (Live(*wave.nodes[index])) wave.nodes[index]->stage = Stage::CpuReady;
            }
            return;
        }
        case Phase::Copy: {
            auto task = executor.TrySubmit([input = wave.nodes, indices = wave.sourceIndices, data = wave.data,
                source = wave.sourcePixels, kind = wave.kind, options = config, memory = memory](const Jobs::StopToken& stop) {
                CopySource(input, indices, data, source, kind, options, memory, stop);
            });
            if (task) {
                wave.task = std::move(task); wave.sourcePixels.reset(); wave.phase = Phase::CopyRunning;
                for (const auto index : wave.sourceIndices) if (Live(*wave.nodes[index])) wave.nodes[index]->stage = Stage::Packing;
            }
            return;
        }
        case Phase::CopyRunning: {
            if (!wave.task->IsFinished()) return;
            const bool success = wave.task->GetState() == Jobs::State::Succeeded;
            const auto message = success ? std::string{} : Explain(wave.task->GetError());
            for (const auto index : wave.sourceIndices) {
                if (!success) Fail(*wave.nodes[index], "image_copy", message);
                else if (!wave.data->entries[index].error.empty())
                    Fail(*wave.nodes[index], wave.data->entries[index].code, wave.data->entries[index].error);
                wave.sourceDone[index] = true;
            }
            // A CpuReady SourceTicket.Cancel() does not release its subscription.
            // Destroy ALL ticket/pixel copies before requesting another source.
            wave.task.reset(); wave.source.reset(); wave.sourcePixels.reset(); wave.phase = Phase::Source;
            return;
        }
        case Phase::Complete:
            if (!uploading) {
                StageWave(wave, Stage::UploadQueued);
                uploading = std::move(builder);
            }
            return;
        }
    }
    void AdvanceUpload(Budget& budget) {
        if (!uploading) return;
        auto& wave = *uploading;
        if (Abandoned(wave)) { CancelWork(wave); uploading.reset(); return; }
        const auto index = wave.uploadCursor;
        wave.uploadCursor = (wave.uploadCursor + 1) % wave.nodes.size();
        if (!wave.nodes[index]) return;
        auto& node = *wave.nodes[index];
        const auto& entry = wave.data->entries[index];
        if (!Live(node)) { RetireUploadNode(wave, index); return; }
        if (!entry.copied) { Fail(node, "image_copy", "image region was not copied into its backing"); RetireUploadNode(wave, index); return; }
        const auto pageIndex = entry.placement.page;
        auto& transfer = wave.transfers[pageIndex];
        if (!transfer.error.empty()) { Fail(node, "image_upload", transfer.error); RetireUploadNode(wave, index); return; }
        try {
            if (!transfer.ticket) {
                if (!budget.uploadRequests) return;
                --budget.uploadRequests;
                const auto& page = wave.data->pages[pageIndex];
                transfer.ticket = uploads.TryEnqueue({page->width, page->height, page->bytes, page, wave.kind == WaveKind::MipCrop});
                if (!transfer.ticket) return; // Bounded upload queue backpressure.
                wave.data->pages[pageIndex].reset();
                ++stats.pagesUploaded;
            }
            switch (transfer.ticket->GetState()) {
            case Upload::State::Failed: case Upload::State::Cancelled:
                transfer.error = transfer.ticket->GetError();
                if (transfer.error.empty()) transfer.error = "image upload was cancelled";
                Fail(node, "image_upload", transfer.error); RetireUploadNode(wave, index); return;
            case Upload::State::Ready: {
                auto asset = std::make_shared<ImageAsset>();
                asset->texture = transfer.ticket->GetTexture();
                asset->content = entry.placement.content;
                asset->image = node.image;
                asset->sourceWidth = entry.sourceWidth; asset->sourceHeight = entry.sourceHeight;
                asset->storage = Packed(wave.kind) ? Storage::Atlas : Storage::Standalone;
                asset->mipLevels = entry.mip.levels;
                asset->mipExtrusion = entry.mip.extrusion;
                node.asset = std::move(asset); node.stage = Stage::Prepared; node.done = true;
                RetireUploadNode(wave, index); return;
            }
            case Upload::State::GpuPending: node.stage = Stage::GpuPending; return;
            case Upload::State::Queued: node.stage = Stage::UploadQueued; return;
            default: node.stage = Stage::Uploading; return;
            }
        } catch (...) {
            transfer.error = Explain(std::current_exception());
            Fail(node, "image_upload", transfer.error);
            RetireUploadNode(wave, index);
        }
    }
    void Shutdown() noexcept {
        if (gate->owner != std::this_thread::get_id() || gate->ticking) std::terminate();
        if (!gate->active) return;
        gate->active = false;
        for (const auto& node : nodes) if (!node->done) {
            node->cancelled.store(true, std::memory_order_release);
            node->dimensions.reset();
        }
        try {
            if (builder) CancelWork(*builder);
            if (uploading) CancelWork(*uploading);
        } catch (...) { std::terminate(); }
        // CPU work owns detached immutable inputs/results, never this Impl.
        // No join or borrowed-service shutdown occurs here.
        builder.reset(); uploading.reset(); pending.clear(); nodes.clear(); cleanup = nodes.end();
    }
};

Vec2 ImageAsset::MapUV(Vec2 local) const {
    if (!texture || !texture->Width() || !texture->Height() || !std::isfinite(local.x) || !std::isfinite(local.y))
        throw std::invalid_argument("invalid Image UV mapping");
    return {(content.x + image.x - std::floor(image.x) + local.x * image.width) / texture->Width(),
        (content.y + image.y - std::floor(image.y) + local.y * image.height) / texture->Height()};
}
Provider::Provider(Jobs::Executor& executor, ImageSources& sources, Upload::Queue& uploads, Jobs::MemoryBudget pages, Config config)
    : impl(std::make_unique<Impl>(executor, sources, uploads, std::move(pages), config)) {}
Provider::~Provider() { Shutdown(); }
std::unique_ptr<Operation> Provider::Start(Handle handle, const Request& request) {
    impl->Running();
    if (impl->nodes.size() >= impl->config.capacity) throw std::length_error("ImageProvider request capacity exhausted");
    if (request.id.kind != Kind::Image || !std::holds_alternative<ImageRequest>(request.data))
        throw std::invalid_argument("ImageProvider accepts only Image requests");
    auto node = std::make_shared<Node>();
    node->handle = handle; node->request = request;
    auto operation = std::make_unique<ImageOperation>(impl->gate, node);
    impl->nodes.push_back(node);
    try { impl->pending.push_back(std::move(node)); }
    catch (...) { impl->nodes.pop_back(); throw; }
    impl->nodes.back()->scheduled = true;
    ++impl->stats.started;
    return operation;
}
void Provider::Tick(Budget budget) {
    impl->Running();
    struct Guard { Gate& gate; ~Guard() { gate.ticking = false; } } guard{*impl->gate};
    impl->gate->ticking = true;
    ++impl->ticks;
    impl->sources.Collect(budget.sourceCollect);
    for (std::size_t i = 0; i < budget.visits; ++i) {
        switch (impl->scheduling++ % 3) {
        case 0: impl->CleanupOne(); break;
        case 1: impl->AdvanceCpu(); break;
        default: impl->AdvanceUpload(budget); break;
        }
    }
}
Stats Provider::GetStats() const {
    impl->gate->Owner();
    auto stats = impl->stats; stats.pending = impl->nodes.size();
    stats.waves = std::size_t(impl->builder != nullptr) + std::size_t(impl->uploading != nullptr);
    return stats;
}
void Provider::Shutdown() noexcept { if (impl) impl->Shutdown(); }
}
