#pragma once
#include "Object.h"
namespace core::modules::Object {
// One scaled semi-implicit Euler step, independent of callbacks/timer/rot.
// Invalid inputs or overflow leave all fields unchanged.
void IntegrateMotion(Object& object);
}
