#include "core/modules/File/Package.h"
#include "BuildInfo.h"
#include <charconv>
#include <iostream>

namespace {
    int Pack(const std::filesystem::path& source, const std::filesystem::path& output, std::string_view chunk) {
        try {
            std::uint32_t bytes = 256 * 1024;
            if (!chunk.empty()) {
                const auto [end, error] = std::from_chars(chunk.data(), chunk.data() + chunk.size(), bytes);
                if (error != std::errc{} || end != chunk.data() + chunk.size())
                    throw std::invalid_argument("chunkBytes must be a decimal integer");
            }
            core::modules::File::BuildPackage(source, output, bytes);
            std::cout << "Package created successfully\n";
            return 0;
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
    int Usage() {
        std::cerr << "Usage: " << engine::build::PackName << " <source-directory> <new-package-file> [chunkBytes]\n"
            << "Default chunkBytes: 262144; allowed: 4096 through 4194304. Output must not exist.\n";
        return 2;
    }
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    if (argc != 3 && argc != 4) return Usage();
    std::string chunk;
    if (argc == 4) {
        const std::wstring text(argv[3]);
        for (wchar_t c : text) {
            if (c < L'0' || c > L'9') return Usage();
            chunk.push_back(static_cast<char>(c));
        }
    }
    return Pack(argv[1], argv[2], chunk);
}
#else
int main(int argc, char** argv) {
    if (argc != 3 && argc != 4) return Usage();
    return Pack(argv[1], argv[2], argc == 4 ? argv[3] : "");
}
#endif
