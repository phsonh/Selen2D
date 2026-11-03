#include "Object.h"

namespace core::modules::Object {
	void Object::reset() noexcept {
		*this = {};
	}
}
