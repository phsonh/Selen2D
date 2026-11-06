#pragma once
#include <cstdint>

struct _GUID;

namespace core::modules::Graphic::Shader::DxcRuntime {
// Initialize once from the executable's deployment directory. Failure is sticky;
// the successful module and factory remain alive until process termination.
void Initialize();

// Internal DXC factory boundary. No native loader handle or shutdown operation
// escapes this module. CPU-only callers share the same lazy initialization.
std::int32_t Create(const _GUID& clsid, const _GUID& iid, void** object);
}
