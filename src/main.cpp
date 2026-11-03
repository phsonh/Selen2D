#include "engine/Engine.h"
#include "engine/Startup.h"
#include "BuildInfo.h"
#include <SDL3/SDL.h>
#include <charconv>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

int main(int argc, char* argv[]) {
    engine::Startup::Config config;
    std::uint64_t frame_limit{};
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--frames" && i + 1 < argc) {
            const std::string_view value(argv[++i]);
            const auto result = std::from_chars(value.data(), value.data() + value.size(), frame_limit);
            if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || frame_limit == 0) {
                std::cerr << "--frames requires a positive integer\n";
                return 2;
            }
        } else {
            std::cerr << "Usage: " << engine::build::Product << " [--frames <positive integer>]; runs launch.lua beside the executable\n";
            return 2;
        }
    }
    try {
        const auto* directory=SDL_GetBasePath();
        if(!directory) throw std::runtime_error(std::string("Cannot locate executable directory: ")+SDL_GetError());
        config=engine::Startup::Load(std::filesystem::path(
            std::u8string(directory, directory + std::char_traits<char>::length(directory))));
    } catch(const std::exception& error) {
        std::cerr<<"Startup error: "<<error.what()<<'\n'; return 2;
    }
    const bool initialized = engine::Init(config.entry,config.files);
    const bool ran = initialized && engine::Run(frame_limit);
    const bool closed = engine::Shutdown();
    return initialized && ran && closed ? 0 : 1;
}
