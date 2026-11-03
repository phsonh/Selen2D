#include "Atlas.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <tuple>

namespace core::modules::Graphic::Image::Atlas {
namespace {
using U = std::uint64_t;
bool PowerOfTwo(std::uint32_t value) { return value && !(value & (value - 1)); }
std::uint32_t Rounded(std::uint32_t value) {
    std::uint32_t result = 1;
    while (result < value) result *= 2;
    return result;
}
U Right(Rect r) { return U(r.x) + r.width; }
U Bottom(Rect r) { return U(r.y) + r.height; }
bool Inside(Rect a, Rect b) {
    return a.x >= b.x && a.y >= b.y && Right(a) <= Right(b) && Bottom(a) <= Bottom(b);
}
bool Overlap(Rect a, Rect b) {
    return a.x < Right(b) && b.x < Right(a) && a.y < Bottom(b) && b.y < Bottom(a);
}
void Cancel(CancelFn cancel, void* data) {
    if (cancel && cancel(data)) throw Cancelled();
}
struct Work {
    U count{}, maximum;
    CancelFn cancel;
    void* data;
    void Step() {
        if (count == maximum) throw std::length_error("atlas packing work limit exceeded");
        if (!(count++ & 255)) Cancel(cancel, data);
    }
};
struct WorkingPage {
    std::vector<Rect> free;
    std::uint32_t usedX{}, usedY{};
};
void Split(WorkingPage& page, Rect placed, Work& work, std::size_t maxFree) {
    std::vector<Rect> next;
    const auto add = [&](Rect rectangle) {
        if (!rectangle.width || !rectangle.height) return;
        if (next.size() >= maxFree) throw std::length_error("atlas free rectangle limit exceeded");
        next.push_back(rectangle);
    };
    for (const auto rectangle : page.free) {
        work.Step();
        if (!Overlap(rectangle, placed)) { add(rectangle); continue; }
        const auto right = static_cast<std::uint32_t>(Right(rectangle));
        const auto bottom = static_cast<std::uint32_t>(Bottom(rectangle));
        const auto placedRight = static_cast<std::uint32_t>(Right(placed));
        const auto placedBottom = static_cast<std::uint32_t>(Bottom(placed));
        if (placed.x > rectangle.x) add({rectangle.x, rectangle.y, placed.x - rectangle.x, rectangle.height});
        if (placedRight < right) add({placedRight, rectangle.y, right - placedRight, rectangle.height});
        if (placed.y > rectangle.y) add({rectangle.x, rectangle.y, rectangle.width, placed.y - rectangle.y});
        if (placedBottom < bottom) add({rectangle.x, placedBottom, rectangle.width, bottom - placedBottom});
    }
    // MaxRects free rectangles may overlap each other, never placed rectangles.
    // Remove contained/duplicate rectangles deterministically without swap erase.
    std::vector<Rect> pruned;
    for (std::size_t i = 0; i < next.size(); ++i) {
        bool contained = false;
        for (std::size_t j = 0; j < next.size(); ++j) {
            work.Step();
            if (i != j && Inside(next[i], next[j]) && (next[i] != next[j] || j < i)) {
                contained = true;
                break;
            }
        }
        if (!contained) pruned.push_back(next[i]);
    }
    page.free = std::move(pruned);
    page.usedX = std::max(page.usedX, static_cast<std::uint32_t>(Right(placed)));
    page.usedY = std::max(page.usedY, static_cast<std::uint32_t>(Bottom(placed)));
}
std::size_t RgbaBytes(std::uint32_t width, std::uint32_t height) {
    if (!width || !height || U(width) * height > std::numeric_limits<std::size_t>::max() / 4)
        throw std::invalid_argument("invalid RGBA dimensions");
    return static_cast<std::size_t>(U(width) * height * 4);
}
}

Plan Pack(std::span<const Item> items, Config config, CancelFn cancel, void* data) {
    if (!PowerOfTwo(config.targetSide) || !PowerOfTwo(config.maxSide) ||
        config.targetSide > config.maxSide || config.maxSide > 16384 ||
        !config.maxItems || !config.maxPages || !config.maxFreeRects || !config.maxWork || !config.maxOutputBytes ||
        U(config.extrusion) * 2 >= config.maxSide)
        throw std::invalid_argument("invalid atlas packing configuration");
    if (items.size() > config.maxItems) throw std::length_error("atlas item limit exceeded");
    Cancel(cancel, data);
    Plan plan;
    plan.extrusion = config.extrusion;
    plan.placements.resize(items.size());
    std::vector<std::size_t> order(items.size());
    std::iota(order.begin(), order.end(), 0);
    for (const auto item : items) {
        if (!item.width || !item.height) throw std::invalid_argument("empty atlas image");
        if (U(item.width) + U(config.extrusion) * 2 > config.maxSide ||
            U(item.height) + U(config.extrusion) * 2 > config.maxSide)
            throw std::length_error("image plus extrusion exceeds atlas page limit; use standalone backing");
    }
    // Original index breaks area ties; anchor, scale and logical name never
    // enter pixel layout. stable output mapping uses the original input order.
    std::sort(order.begin(), order.end(), [&](auto a, auto b) {
        const U aa = U(items[a].width) * items[a].height;
        const U bb = U(items[b].width) * items[b].height;
        return aa != bb ? aa > bb : a < b;
    });
    std::vector<WorkingPage> pages;
    Work work{0, config.maxWork, cancel, data};
    for (const auto index : order) {
        Cancel(cancel, data);
        const auto item = items[index];
        const auto width = item.width + 2 * config.extrusion;
        const auto height = item.height + 2 * config.extrusion;
        using Score = std::tuple<std::uint32_t, std::uint32_t, std::size_t, std::uint32_t, std::uint32_t>;
        auto best = Score{UINT32_MAX, UINT32_MAX, SIZE_MAX, UINT32_MAX, UINT32_MAX};
        Rect chosen;
        std::size_t pageIndex = SIZE_MAX;
        for (std::size_t p = 0; p < pages.size(); ++p) {
            for (const auto space : pages[p].free) {
                work.Step();
                if (width > space.width || height > space.height) continue;
                const auto dx = space.width - width, dy = space.height - height;
                const auto score = Score{std::min(dx, dy), std::max(dx, dy), p, space.y, space.x};
                if (score < best) {
                    best = score;
                    pageIndex = p;
                    chosen = {space.x, space.y, width, height};
                }
            }
        }
        if (pageIndex == SIZE_MAX) {
            if (pages.size() >= config.maxPages) throw std::length_error("atlas page limit exceeded");
            const auto side = std::max(config.targetSide, Rounded(std::max(width, height)));
            pageIndex = pages.size();
            pages.push_back({{{0, 0, side, side}}});
            plan.pages.emplace_back();
            chosen = {0, 0, width, height};
        }
        Split(pages[pageIndex], chosen, work, config.maxFreeRects);
        plan.placements[index] = {pageIndex, chosen,
            {chosen.x + config.extrusion, chosen.y + config.extrusion, item.width, item.height}};
        auto& page = plan.pages[pageIndex];
        page.contentPixels += U(item.width) * item.height;
        page.allocatedPixels += U(width) * height;
    }
    U bytes = 0;
    for (std::size_t i = 0; i < pages.size(); ++i) {
        plan.pages[i].width = Rounded(pages[i].usedX);
        plan.pages[i].height = Rounded(pages[i].usedY);
        const auto pageBytes = U(plan.pages[i].width) * plan.pages[i].height * 4;
        if (pageBytes > config.maxOutputBytes - bytes) throw std::length_error("atlas output byte limit exceeded");
        bytes += pageBytes;
    }
    Cancel(cancel, data);
    return plan;
}

void CopyExtruded(std::span<std::uint8_t> destination, std::uint32_t pageWidth,
    std::uint32_t pageHeight, const Placement& placement, RgbaView source, Rect region,
    std::uint32_t extrusion, CancelFn cancel, void* data) {
    const auto required = RgbaBytes(pageWidth, pageHeight);
    if (destination.size() < required) throw std::invalid_argument("atlas destination is too short");
    const auto sourceRow = RgbaBytes(source.width, 1);
    if (!source.height || source.stride < sourceRow ||
        (source.height > 1 && source.stride > (std::numeric_limits<std::size_t>::max() - sourceRow) / (source.height - 1)) ||
        source.bytes.size() < (source.height - 1) * source.stride + sourceRow)
        throw std::invalid_argument("invalid source RGBA span/stride");
    const auto allocation = placement.allocation, content = placement.content;
    if (!region.width || !region.height || !Inside(region, {0, 0, source.width, source.height}) ||
        content.width != region.width || content.height != region.height ||
        U(allocation.width) != U(content.width) + U(extrusion) * 2 ||
        U(allocation.height) != U(content.height) + U(extrusion) * 2 ||
        U(allocation.x) + extrusion != content.x || U(allocation.y) + extrusion != content.y ||
        !Inside(allocation, {0, 0, pageWidth, pageHeight}))
        throw std::invalid_argument("invalid atlas placement/source region");
    // Source/destination aliasing is unsupported: copy order must not change the
    // source edges while extruding. uintptr comparisons avoid unrelated ptr <.
    const auto from = reinterpret_cast<std::uintptr_t>(source.bytes.data());
    const auto to = reinterpret_cast<std::uintptr_t>(destination.data());
    if ((from <= to && to - from < source.bytes.size()) || (to < from && from - to < required))
        throw std::invalid_argument("atlas source and destination overlap");
    const auto pitch = static_cast<std::size_t>(pageWidth) * 4;
    Cancel(cancel, data);
    for (std::uint32_t row = 0; row < allocation.height; ++row) {
        Cancel(cancel, data);
        const auto sourceY = region.y + (row < extrusion ? 0 : std::min(row - extrusion, region.height - 1));
        const auto* input = source.bytes.data() + static_cast<std::size_t>(sourceY) * source.stride + static_cast<std::size_t>(region.x) * 4;
        auto* output = destination.data() + static_cast<std::size_t>(allocation.y + row) * pitch + static_cast<std::size_t>(allocation.x) * 4;
        for (std::uint32_t x = 0; x < extrusion; ++x) std::memcpy(output + static_cast<std::size_t>(x) * 4, input, 4);
        std::memcpy(output + static_cast<std::size_t>(extrusion) * 4, input, static_cast<std::size_t>(region.width) * 4);
        const auto* last = input + static_cast<std::size_t>(region.width - 1) * 4;
        for (std::uint32_t x = 0; x < extrusion; ++x)
            std::memcpy(output + (static_cast<std::size_t>(extrusion) + region.width + x) * 4, last, 4);
    }
    Cancel(cancel, data);
}
}
