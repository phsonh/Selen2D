#pragma once
#include <SDL3/SDL_iostream.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <memory>
#include <system_error>

namespace core::modules::File {
// Only ordinary, authorized I/O failures become File's nil/false,err results.
class IoError : public std::runtime_error {
public:
    std::string code;
    IoError(std::string category, std::string message)
        : std::runtime_error(std::move(message)), code(std::move(category)) {}
};
class InvalidPath : public std::invalid_argument { public: using std::invalid_argument::invalid_argument; };
class AccessError : public std::runtime_error { public: using std::runtime_error::runtime_error; };
class ConflictError : public std::runtime_error { public: using std::runtime_error::runtime_error; };
inline constexpr char StreamErrorCodeProperty[] = "engine.io.error-code";
struct StreamErrorState { std::string code{"io_error"}; };
void SetStreamErrorState(SDL_IOStream*, const std::shared_ptr<StreamErrorState>&);
// SDL callbacks cannot throw. A backend records a stable category on its stream
// properties; callers translate SDL error status with this function.
IoError GetStreamError(SDL_IOStream* stream, std::string_view operation,
                       std::string_view virtualPath = {});
// Always closes/invalidate the stream, even when it throws. Retains its error
// state across SDL destroying the properties after the close callback.
bool CloseStream(SDL_IOStream* stream, std::string_view virtualPath = {});
std::string IoErrorCode(const std::error_code& error);
}
