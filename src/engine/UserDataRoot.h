#pragma once
#include <filesystem>
namespace engine::Startup {
// Resolves a system-defined base without creating directories. No CWD fallback.
std::filesystem::path ResolveUserDataRoot();
}
