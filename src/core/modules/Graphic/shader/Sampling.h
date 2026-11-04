#pragma once
#include "Model.h"
#include <functional>
#include <string>
#include <vector>

namespace core::modules::Graphic::Shader::Sampling {
// Input must have passed Parse: one VFS-preprocessed buffer, only trusted
// source #line records, no residual directives capable of opening files.
// DXC semantic cursors identify actual global resource references. Unsupported
// color-resource escapes fail here rather than bypassing color conversion.
std::vector<SamplingCall> Analyze(const Model&, const std::function<bool()>& cancelled);
std::string Function(std::string_view texture,bool explicitLod);
std::string Prototypes(const Model&);
std::string Definitions(const Model&);
inline constexpr const char* BufferName="EngineSampling";
}
