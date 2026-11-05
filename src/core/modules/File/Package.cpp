#include "Package.h"
#include "Profile.h"
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_properties.h>
#include <sodium.h>
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <mutex>
#include <set>
#include <utility>
#include <cerrno>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace core::modules::File {
namespace {
constexpr uint64_t IndexLimit = 64ULL * 1024 * 1024;
constexpr uint32_t CountLimit = 1048576, MinBlock = 65536, MaxBlock = 1048576;
constexpr char IndexDomain[] = "SLPK2-INDEX-AAD!", BlockDomain[] = "SLPK2-BLOCK-AAD!";
[[noreturn]] void Invalid(const char *why) { throw std::runtime_error(std::string("Resource package: ") + why); }
void Require(bool ok, const char *why) {
    if (!ok)
        Invalid(why);
}
[[noreturn]] void Corrupt(const char *why) { throw IoError("corrupt_data", std::string("Resource package: ") + why); }
uint64_t LE(const uint8_t *p, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i)
        v |= uint64_t(p[i]) << (8 * i);
    return v;
}
void Put32(std::vector<uint8_t> &out, uint32_t v) {
    for (int i = 0; i < 4; ++i)
        out.push_back(uint8_t(v >> (i * 8)));
}
bool Zero(const uint8_t *p, size_t n) {
    return std::all_of(p, p + n, [](uint8_t c) { return c == 0; });
}
uint32_t Crc(const uint8_t *p, size_t n) {
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int j = 0; j < 8; ++j)
                c = (c >> 1) ^ ((c & 1) ? 0xedb88320U : 0);
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = ~0U;
    for (size_t i = 0; i < n; ++i)
        c = table[(c ^ p[i]) & 255] ^ (c >> 8);
    return ~c;
}
// Budgets apply across all active packages/streams, including temporary buffers.
struct Budget {
    std::atomic<uint64_t> used{};
    uint64_t limit;
};
Budget indices{{0}, 256ULL * 1024 * 1024}, caches{{0}, 64ULL * 1024 * 1024}, work{{0}, 64ULL * 1024 * 1024};
struct Lease {
    Budget *budget{};
    uint64_t bytes{};
    explicit Lease(Budget &b) : budget(&b) {}
    ~Lease() { budget->used.fetch_sub(bytes, std::memory_order_relaxed); }
    Lease(const Lease &) = delete;
    Lease &operator=(const Lease &) = delete;
    void Add(uint64_t n) {
        uint64_t old = budget->used.load(std::memory_order_relaxed);
        do {
            if (n > budget->limit || old > budget->limit - n)
                throw IoError("io_error", "Resource package: memory budget exceeded");
        } while (!budget->used.compare_exchange_weak(old, old + n, std::memory_order_relaxed));
        bytes += n;
    }
    void Clear() {
        budget->used.fetch_sub(bytes, std::memory_order_relaxed);
        bytes = 0;
    }
};
class Source {
#ifdef _WIN32
    HANDLE handle_{INVALID_HANDLE_VALUE};
    FILE_ID_INFO identity_{};
#else
    int handle_{-1};
    dev_t device_{};
    ino_t inode_{};
#endif
    mutable std::mutex mutex_;

  public:
    uint64_t size{};
    explicit Source(const std::filesystem::path &path) {
#ifdef _WIN32
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                               FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            Invalid("cannot open package source");
        BY_HANDLE_FILE_INFORMATION basic{};
        LARGE_INTEGER length{};
        bool ok = GetFileType(h) == FILE_TYPE_DISK && GetFileInformationByHandle(h, &basic) &&
                  !(basic.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) &&
                  GetFileInformationByHandleEx(h, FileIdInfo, &identity_, sizeof(identity_)) &&
                  GetFileSizeEx(h, &length) && length.QuadPart >= 0;
        if (!ok) {
            CloseHandle(h);
            Invalid("source must be a regular non-reparse file");
        }
        handle_ = h;
        size = uint64_t(length.QuadPart);
#else
        handle_ = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW);
        struct stat info{};
        if (handle_ < 0 || fstat(handle_, &info) || !S_ISREG(info.st_mode) || info.st_size < 0) {
            if (handle_ >= 0)
                ::close(handle_);
            handle_ = -1;
            Invalid("cannot open regular package source");
        }
        device_ = info.st_dev;
        inode_ = info.st_ino;
        size = uint64_t(info.st_size);
#endif
        if (size > MaxFilePosition) {
#ifdef _WIN32
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
#else
            ::close(handle_);
            handle_ = -1;
#endif
            Invalid("physical size exceeds exact-position limit");
        }
    }
    ~Source() {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE)
            CloseHandle(handle_);
#else
        if (handle_ >= 0)
            ::close(handle_);
#endif
    }
    void Read(uint64_t offset, void *output, size_t count) const {
        VFS_PROFILE_SCOPE(PackageRead);
        if (offset > size || count > size - offset)
            Corrupt("truncated record");
        std::lock_guard lock(mutex_);
#ifdef _WIN32
        LARGE_INTEGER p{};
        p.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(handle_, p, nullptr, FILE_BEGIN))
            throw IoError("io_error", "Resource package: source seek failed");
        auto *dst = static_cast<uint8_t *>(output);
        while (count) {
            DWORD chunk = static_cast<DWORD>(std::min<size_t>(count, 1U << 30)), got = 0;
            if (!ReadFile(handle_, dst, chunk, &got, nullptr))
                throw IoError("io_error", "Resource package: source read failed");
            VFS_PROFILE_ADD(PackageReadBytes,got);
            if (got != chunk)
                Corrupt("source truncated after mounting");
            dst += got;
            count -= got;
        }
#else
        auto *dst = static_cast<uint8_t *>(output);
        while (count) {
            auto n = pread(handle_, dst, count, static_cast<off_t>(offset));
            if (n < 0)
                throw IoError("io_error", "Resource package: source read failed");
            VFS_PROFILE_ADD(PackageReadBytes,n);
            if (!n)
                Corrupt("source truncated after mounting");
            dst += n;
            offset += n;
            count -= n;
        }
#endif
    }
    bool Matches(const std::filesystem::path &path) const {
#ifdef _WIN32
        HANDLE h = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            const auto error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
                return false;
            throw IoError("io_error", "Resource package: cannot verify candidate file identity");
        }
        FILE_ID_INFO other{};
        bool ok = GetFileInformationByHandleEx(h, FileIdInfo, &other, sizeof(other)) != FALSE;
        CloseHandle(h);
        if (!ok)
            throw IoError("io_error", "Resource package: cannot verify candidate file identity");
        return other.VolumeSerialNumber == identity_.VolumeSerialNumber &&
               std::memcmp(&other.FileId, &identity_.FileId, sizeof(other.FileId)) == 0;
#else
        struct stat other{};
        if (lstat(path.c_str(), &other) != 0) {
            if (errno == ENOENT || errno == ENOTDIR)
                return false;
            throw IoError("io_error", "Resource package: cannot verify candidate file identity");
        }
        return other.st_dev == device_ && other.st_ino == inode_;
#endif
    }
};
struct Block {
    uint64_t offset{};
    uint32_t stored{}, raw{}, crc{};
    uint8_t codec{};
    std::array<uint8_t, 48> descriptor{};
};
struct Entry {
    uint32_t index{};
    uint64_t size{};
    std::vector<Block> blocks;
};
std::vector<uint8_t> BlockAAD(const std::array<uint8_t, 112> &h, uint32_t entry, uint32_t block, const Block &b) {
    std::vector<uint8_t> a;
    a.reserve(120);
    a.insert(a.end(), BlockDomain, BlockDomain + 16);
    a.insert(a.end(), h.begin(), h.begin() + 32);
    a.insert(a.end(), h.begin() + 56, h.begin() + 72);
    Put32(a, entry);
    Put32(a, block);
    a.insert(a.end(), b.descriptor.begin(), b.descriptor.end());
    return a;
}
void Decrypt(const uint8_t *cipher, size_t count, const uint8_t *nonce, const uint8_t *aad, size_t aadSize,
             const PackageKey &key, uint8_t *plain) {
    unsigned long long result{};
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(plain, &result, nullptr, cipher, count, aad, aadSize, nonce,
                                                   key.data()) != 0 ||
        result != count - 16)
        Corrupt("authentication failed (key mismatch or damaged/modified content)");
}
struct Cursor {
    const std::vector<uint8_t> &bytes;
    size_t at{};
    const uint8_t *Take(size_t n) {
        Require(n <= bytes.size() - at, "truncated index");
        const auto *p = bytes.data() + at;
        at += n;
        return p;
    }
};
} // namespace
struct Package::Impl {
    std::shared_ptr<Source> source;
    Lease memory{indices};
    std::array<uint8_t, 112> header{};
    std::optional<PackageKey> key;
    bool encrypted{};
    uint32_t blockSize{};
    std::map<std::string, NodeInfo, std::less<>> nodes;
    std::map<std::string, Entry, std::less<>> files;
    ~Impl() {
        if (key)
            sodium_memzero(key->data(), key->size());
    }
    void Decode(const Entry &entry, uint32_t index, std::vector<uint8_t> &raw) const {
        const auto &b = entry.blocks[index];
        Lease temporary(work);
        temporary.Add(uint64_t(blockSize) + 65536 + ZSTD_estimateDStreamSize(blockSize) + (encrypted ? b.stored : 0));
        std::vector<uint8_t> plain;
        if (encrypted) {
            std::vector<uint8_t> cipher(b.stored);
            source->Read(b.offset, cipher.data(), cipher.size());
            plain.resize(b.stored - 16);
            auto aad = BlockAAD(header, entry.index, index, b);
            Decrypt(cipher.data(), cipher.size(), b.descriptor.data() + 24, aad.data(), aad.size(), *key, plain.data());
        }
        raw.resize(b.raw);
        if (!b.codec) {
            if (encrypted)
                std::memcpy(raw.data(), plain.data(), raw.size());
            else
                source->Read(b.offset, raw.data(), raw.size());
        } else {
            uint64_t stored = encrypted ? plain.size() : b.stored;
            std::array<uint8_t, ZSTD_FRAMEHEADERSIZE_MAX> prefix{};
            const size_t prefixSize = static_cast<size_t>(std::min<uint64_t>(stored, prefix.size()));
            if (encrypted)
                std::memcpy(prefix.data(), plain.data(), prefixSize);
            else
                source->Read(b.offset, prefix.data(), prefixSize);
            ZSTD_frameHeader frame{};
            auto code = ZSTD_getFrameHeader(&frame, prefix.data(), prefixSize);
            if (code != 0 || frame.frameType != ZSTD_frame || frame.dictID != 0 ||
                (frame.frameContentSize != b.raw &&
                 (encrypted || frame.frameContentSize != ZSTD_CONTENTSIZE_UNKNOWN)) ||
                (encrypted && frame.windowSize > blockSize))
                Corrupt("invalid Zstd frame header");
            // v1 permits independent frames without a content-size field. Its
            // working memory is still bounded; v2 has the stricter wire limit.
            if (!encrypted && frame.windowSize > blockSize) {
                if (frame.windowSize > 64ULL * 1024 * 1024)
                    throw IoError("io_error", "Resource package: Zstd work memory budget exceeded");
                const auto estimate = ZSTD_estimateDStreamSize(static_cast<size_t>(frame.windowSize));
                if (ZSTD_isError(estimate))
                    throw IoError("io_error", "Resource package: invalid Zstd work estimate");
                temporary.Add(estimate - ZSTD_estimateDStreamSize(blockSize));
            }
            auto context = std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)>(ZSTD_createDCtx(), ZSTD_freeDCtx);
            if (!context)
                throw IoError("io_error", "Resource package: cannot allocate Zstd decoder");
            if (ZSTD_isError(ZSTD_DCtx_setParameter(context.get(), ZSTD_d_windowLogMax, encrypted ? 20 : 26)))
                Corrupt("invalid Zstd window");
            std::array<uint8_t, 65536> chunk{};
            uint64_t consumed = 0;
            ZSTD_outBuffer output{raw.data(), raw.size(), 0};
            bool ended = false;
            while (consumed < stored) {
                size_t n = static_cast<size_t>(std::min<uint64_t>(chunk.size(), stored - consumed));
                const uint8_t *inputData{};
                if (encrypted)
                    inputData = plain.data() + consumed;
                else {
                    source->Read(b.offset + consumed, chunk.data(), n);
                    inputData = chunk.data();
                }
                ZSTD_inBuffer input{inputData, n, 0};
                while (input.pos < input.size) {
                    auto oldIn = input.pos, oldOut = output.pos;
                    code = ZSTD_decompressStream(context.get(), &output, &input);
                    if (ZSTD_isError(code))
                        Corrupt("Zstd decoding failed");
                    if (code == 0) {
                        if (input.pos != input.size || consumed + n != stored)
                            Corrupt("extra Zstd frames or trailing bytes");
                        ended = true;
                        break;
                    }
                    if (oldIn == input.pos && oldOut == output.pos)
                        Corrupt("Zstd decoded size exceeds descriptor");
                }
                consumed += n;
            }
            if (!ended || output.pos != raw.size())
                Corrupt("Zstd decoded size mismatch");
        }
        if (Crc(raw.data(), raw.size()) != b.crc)
            Corrupt("CRC32 mismatch");
    }
    struct Reader {
        std::shared_ptr<const Package> owner;
        const Entry *entry{};
        uint64_t position{};
        std::vector<uint8_t> cache;
        uint32_t cached = UINT32_MAX;
        Lease cacheMemory{caches};
        SDL_PropertiesID properties{};
        void Error(const char *category, const char *message, SDL_IOStatus *status = nullptr) noexcept {
            SDL_SetStringProperty(properties, StreamErrorCodeProperty, category);
            SDL_SetError("%s", message);
            if (status)
                *status = SDL_IO_STATUS_ERROR;
        }
        static Sint64 SDLCALL Size(void *p) { return static_cast<Sint64>(static_cast<Reader *>(p)->entry->size); }
        static Sint64 SDLCALL Seek(void *p, Sint64 offset, SDL_IOWhence whence) {
            auto &r = *static_cast<Reader *>(p);
            uint64_t base{};
            if (whence == SDL_IO_SEEK_SET)
                base = 0;
            else if (whence == SDL_IO_SEEK_CUR)
                base = r.position;
            else if (whence == SDL_IO_SEEK_END)
                base = r.entry->size;
            else {
                r.Error("io_error", "Resource package: invalid seek origin");
                return -1;
            }
            if ((offset < 0 && uint64_t(-(offset + 1)) + 1 > base) ||
                (offset >= 0 && uint64_t(offset) > MaxFilePosition - base)) {
                r.Error("out_of_range", "Resource package: seek outside exact-position range");
                return -1;
            }
            r.position = offset < 0 ? base - (uint64_t(-(offset + 1)) + 1) : base + uint64_t(offset);
            return static_cast<Sint64>(r.position);
        }
        static size_t SDLCALL Read(void *p, void *data, size_t requested, SDL_IOStatus *status) {
            auto &r = *static_cast<Reader *>(p);
            size_t done = 0;
            try {
                if (r.position >= r.entry->size) {
                    *status = SDL_IO_STATUS_EOF;
                    return 0;
                }
                const auto &impl = *r.owner->impl_;
                size_t need = static_cast<size_t>(std::min<uint64_t>(requested, r.entry->size - r.position));
                while (done < need) {
                    auto index = static_cast<uint32_t>(r.position / impl.blockSize);
                    if (r.cached != index) {
                        r.cached = UINT32_MAX;
                        std::vector<uint8_t>().swap(r.cache);
                        r.cacheMemory.Clear();
                        r.cacheMemory.Add(r.entry->blocks[index].raw);
                        impl.Decode(*r.entry, index, r.cache);
                        r.cached = index;
                    }
                    size_t start = static_cast<size_t>(r.position % impl.blockSize),
                           n = std::min(need - done, r.cache.size() - start);
                    std::memcpy(static_cast<uint8_t *>(data) + done, r.cache.data() + start, n);
                    r.position += n;
                    done += n;
                }
                *status = done < requested ? SDL_IO_STATUS_EOF : SDL_IO_STATUS_READY;
                return done;
            } catch (const IoError &e) {
                r.cached = UINT32_MAX;
                r.Error(e.code.c_str(), e.what(), status);
            } catch (const std::exception &e) {
                r.cached = UINT32_MAX;
                r.Error("io_error", e.what(), status);
            } catch (...) {
                r.cached = UINT32_MAX;
                r.Error("io_error", "Resource package: read failed", status);
            }
            return done;
        }
        static bool SDLCALL Close(void *p) {
            delete static_cast<Reader *>(p);
            return true;
        }
    };
};
Package::Package(std::unique_ptr<Impl> p) : impl_(std::move(p)) {}
Package::~Package() = default;
std::shared_ptr<Package> Package::Load(const std::filesystem::path &path, bool encrypted,
                                       std::optional<PackageKey> key) {
    VFS_PROFILE_SCOPE(PackageLoad);
    auto p = std::make_unique<Impl>();
    p->source = std::make_shared<Source>(path);
    p->encrypted = encrypted;
    Require(p->source->size >= 48, "truncated header");
    p->source->Read(0, p->header.data(), 48);
    const auto *h = p->header.data();
    Require(std::memcmp(h, "SLPK", 4) == 0, "unsupported magic");
    auto version = LE(h + 4, 2);
    Require(version == (encrypted ? 2 : 1), "package version does not match configured encryption");
    const size_t headerSize = encrypted ? 112 : 48;
    Require(LE(h + 8, 4) == headerSize, "invalid header size");
    Require(p->source->size >= headerSize, "truncated header");
    if (encrypted) {
        Require(key.has_value(), "encrypted package requires an explicit key");
        InitializePackageCrypto();
        p->key = key;
        p->source->Read(48, p->header.data() + 48, 64);
    }
    Require(LE(h + 6, 2) == (encrypted ? 1 : 0), "unsupported flags");
    uint64_t count = LE(h + 12, 4);
    Require(count <= CountLimit, "entry count exceeds limit");
    p->blockSize = static_cast<uint32_t>(LE(h + 16, 4));
    Require(p->blockSize >= MinBlock && p->blockSize <= MaxBlock && (p->blockSize & (p->blockSize - 1)) == 0,
            "invalid block size");
    uint64_t indexOffset{}, plainSize{}, storedSize{};
    if (encrypted) {
        Require(LE(h + 20, 2) == 1 && LE(h + 22, 2) == 1, "unsupported encryption/key algorithm");
        Require(LE(h + 24, 8) == 112 && Zero(h + 96, 16), "invalid data offset or reserved fields");
        indexOffset = LE(h + 32, 8);
        plainSize = LE(h + 40, 8);
        storedSize = LE(h + 48, 8);
        Require(plainSize <= IndexLimit && storedSize == plainSize + 16, "invalid encrypted index size");
    } else {
        indexOffset = LE(h + 20, 8);
        plainSize = storedSize = LE(h + 28, 8);
        Require(LE(h + 36, 8) == 48 && Zero(h + 44, 4), "invalid data offset or reserved fields");
        Require(plainSize <= IndexLimit, "index size exceeds limit");
    }
    Require(indexOffset >= headerSize && indexOffset <= p->source->size && storedSize == p->source->size - indexOffset,
            "index bounds or trailing bytes invalid");
    Lease buffers(indices);
    buffers.Add(storedSize + (encrypted ? plainSize : 0));
    std::vector<uint8_t> encoded(static_cast<size_t>(storedSize));
    p->source->Read(indexOffset, encoded.data(), encoded.size());
    std::vector<uint8_t> decoded;
    if (encrypted) {
        decoded.resize(static_cast<size_t>(plainSize));
        std::array<uint8_t, 128> aad{};
        std::memcpy(aad.data(), IndexDomain, 16);
        std::memcpy(aad.data() + 16, h, 112);
        try {
            Decrypt(encoded.data(), encoded.size(), h + 72, aad.data(), aad.size(), *p->key, decoded.data());
        } catch (const IoError &e) {
            throw std::runtime_error(e.what());
        }
    }
    const auto &data = encrypted ? decoded : encoded;
    Cursor c{data};
    std::set<std::string, std::less<>> explicitPaths;
    std::map<std::string, std::string, std::less<>> folded;
    std::set<std::array<uint8_t, 24>> nonces;
    std::vector<std::pair<uint64_t, uint64_t>> intervals;
    Lease validation(indices);
    if (encrypted) {
        std::array<uint8_t, 24> nonce{};
        std::memcpy(nonce.data(), h + 72, 24);
        nonces.insert(nonce);
        validation.Add(128);
    }
    auto addNode = [&](const std::string &name, NodeInfo info) {
        auto old = p->nodes.find(name);
        if (old != p->nodes.end()) {
            Require(old->second.type == info.type, "file/directory ancestor conflict");
            return;
        }
        auto fold = FoldPath(name);
        auto found = folded.find(fold);
        Require(found == folded.end() || found->second == name, "Unicode case-fold path collision");
        p->memory.Add(256 + name.size() * 2);
        validation.Add(256 + name.size() * 2 + fold.size());
        folded.emplace(std::move(fold), name);
        p->nodes.emplace(name, info);
    };
    uint64_t totalBlocks = 0, nextOffset = headerSize;
    std::string previous;
    for (uint32_t entryIndex = 0; entryIndex < count; ++entryIndex) {
        const auto *fixed = c.Take(24);
        auto kind = fixed[0];
        uint64_t pathSize = LE(fixed + 4, 4), fileSize = LE(fixed + 8, 8), blocks = LE(fixed + 16, 4);
        Require(kind <= 1 && fixed[1] == 0 && Zero(fixed + 2, 2) && Zero(fixed + 20, 4),
                "invalid entry flags/kind/reserved fields");
        Require(pathSize >= 1 && pathSize <= 4096 && fileSize <= MaxFilePosition, "invalid path or logical size");
        Require(blocks <= CountLimit - totalBlocks, "block count exceeds limit");
        totalBlocks += blocks;
        const auto *pathBytes = c.Take(static_cast<size_t>(pathSize));
        std::string name(reinterpret_cast<const char *>(pathBytes), static_cast<size_t>(pathSize));
        auto canonical = Normalize(name);
        Require(canonical && *canonical == name, "noncanonical package path");
        Require(!encrypted || previous.empty() || previous < name, "v2 paths are not strictly byte-sorted");
        previous = name;
        validation.Add(128 + name.size());
        Require(explicitPaths.insert(name).second, "duplicate path");
        Require(kind == 0 ? blocks == fileSize / p->blockSize + (fileSize % p->blockSize != 0)
                          : fileSize == 0 && blocks == 0,
                "invalid file/directory block count");
        addNode(name, {kind ? NodeType::Directory : NodeType::File, fileSize, 0});
        for (auto slash = name.find('/'); slash != std::string::npos; slash = name.find('/', slash + 1))
            addNode(name.substr(0, slash), {NodeType::Directory, 0, 0});
        Entry entry;
        entry.index = entryIndex;
        entry.size = fileSize;
        p->memory.Add(192 + name.size() + blocks * sizeof(Block));
        entry.blocks.reserve(static_cast<size_t>(blocks));
        uint64_t sum = 0;
        for (uint32_t blockIndex = 0; blockIndex < blocks; ++blockIndex) {
            const auto *d = c.Take(encrypted ? 48 : 24);
            Block b;
            b.offset = LE(d, 8);
            b.stored = static_cast<uint32_t>(LE(d + 8, 4));
            b.raw = static_cast<uint32_t>(LE(d + 12, 4));
            b.crc = static_cast<uint32_t>(LE(d + 16, 4));
            b.codec = d[20];
            std::memcpy(b.descriptor.data(), d, encrypted ? 48 : 24);
            Require(b.raw >= 1 && b.raw <= p->blockSize && (blockIndex + 1 == blocks || b.raw == p->blockSize),
                    "invalid block raw size");
            Require(b.codec <= 1 && Zero(d + 21, 3), "invalid codec/reserved fields");
            Require(b.stored > 0 && b.offset >= headerSize && b.offset <= indexOffset &&
                        b.stored <= indexOffset - b.offset,
                    "block outside payload");
            Require(b.codec != 0 || b.stored == b.raw + (encrypted ? 16 : 0), "raw block stored size mismatch");
            if (encrypted) {
                Require(b.stored > 16 && b.stored <= b.raw + 16 && (b.codec == 0 || b.stored - 16 < b.raw),
                        "encrypted block stored size invalid");
                Require(b.offset == nextOffset, "v2 blocks are not tightly ordered");
                nextOffset += b.stored;
                std::array<uint8_t, 24> nonce{};
                std::memcpy(nonce.data(), d + 24, 24);
                validation.Add(128);
                Require(nonces.insert(nonce).second, "duplicate nonce");
            } else {
                validation.Add(sizeof(std::pair<uint64_t, uint64_t>) * 2);
                intervals.emplace_back(b.offset, b.offset + b.stored);
            }
            sum += b.raw;
            entry.blocks.push_back(b);
        }
        Require(sum == fileSize, "file size does not equal block sizes");
        if (!kind)
            p->files.emplace(std::move(name), std::move(entry));
    }
    Require(c.at == data.size(), "extra index bytes");
    if (encrypted)
        Require(nextOffset == indexOffset, "v2 payload gap");
    else {
        std::sort(intervals.begin(), intervals.end());
        uint64_t end = headerSize;
        for (auto interval : intervals) {
            Require(interval.first >= end, "overlapping payload blocks");
            end = interval.second;
        }
    }
    return std::shared_ptr<Package>(new Package(std::move(p)));
}
const std::map<std::string, NodeInfo, std::less<>> &Package::Entries() const noexcept { return impl_->nodes; }
NodeInfo Package::Stat(std::string_view name) const {
    if (name.empty())
        return {NodeType::Directory, 0, 0};
    auto found = impl_->nodes.find(name);
    return found == impl_->nodes.end() ? NodeInfo{} : found->second;
}
std::optional<std::vector<std::string>> Package::ListDirectory(std::string_view name) const {
    if (Stat(name).type != NodeType::Directory)
        return std::nullopt;
    std::vector<std::string> out;
    std::string prefix(name);
    if (!prefix.empty())
        prefix += '/';
    for (auto it = impl_->nodes.lower_bound(prefix); it != impl_->nodes.end() && it->first.starts_with(prefix); ++it) {
        auto child = std::string_view(it->first).substr(prefix.size());
        if (!child.empty() && child.find('/') == std::string_view::npos)
            out.emplace_back(child);
    }
    return out;
}
SDL_IOStream *Package::Open(std::string_view name) const {
    auto found = impl_->files.find(name);
    if (found == impl_->files.end())
        throw IoError(Stat(name).type == NodeType::Directory ? "is_directory" : "not_found",
                      "Resource package: requested file is unavailable");
    auto reader = std::make_unique<Impl::Reader>();
    reader->owner = shared_from_this();
    reader->entry = &found->second;
    SDL_IOStreamInterface callbacks;
    SDL_INIT_INTERFACE(&callbacks);
    callbacks.size = Impl::Reader::Size;
    callbacks.seek = Impl::Reader::Seek;
    callbacks.read = Impl::Reader::Read;
    callbacks.close = Impl::Reader::Close;
    auto stream = SDL_OpenIO(&callbacks, reader.get());
    if (!stream)
        throw IoError("io_error", "Resource package: cannot create read stream");
    reader->properties = SDL_GetIOProperties(stream);
    reader.release();
    return stream;
}
bool Package::IsBackingFile(const std::filesystem::path &path) const { return impl_->source->Matches(path); }
} // namespace core::modules::File
