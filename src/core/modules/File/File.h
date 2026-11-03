#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace core::modules::File {
    struct Snapshot {
        std::string bytes;
        std::uint64_t modified{}; // Observed SDL modification timestamp, not a file identity.
    };
    struct ReadOptions {
        std::size_t maxBytes{};
        std::size_t chunkBytes{256 * 1024};
        std::size_t prefixBytes{33};
        std::function<bool()> cancelled;
        // Borrowed prefix is valid only for this call. Invoked after the bounded
        // stack prefix read and before the full byte buffer is allocated. May
        // reserve memory credit or reject a file by throwing. Runs on caller's
        // thread; must not retain the prefix or touch a Lua VM from a worker.
        std::function<void(std::uint64_t size, std::string_view prefix)> admit;
    };
    class ReadCancelled : public std::runtime_error {
    public:
        ReadCancelled() : std::runtime_error("file snapshot read cancelled") {}
    };
    // For regular asset files on a worker. One opened stream, bounded allocation,
    // cooperative cancellation between I/O calls, and observed-change rejection.
    // Does not promise atomic snapshots of files concurrently rewritten on every
    // filesystem, or interrupt an OS call already blocked on slow storage.
    Snapshot ReadSnapshot(std::string_view path, const ReadOptions& options);
}
