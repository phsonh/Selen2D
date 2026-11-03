#include "Motion.h"
#include <cmath>
#include <initializer_list>
#include <stdexcept>

namespace core::modules::Object {
void IntegrateMotion(Object& object) {
    const auto& t=object.transform; const auto& m=object.motion;
    for(double value:{t.x,t.y,m.vx,m.vy,m.ax,m.ay,m.rate})
        if(!std::isfinite(value)) throw std::invalid_argument("Object motion fields must be finite");
    if(m.rate<0) throw std::invalid_argument("Object rate must be non-negative");
    // New Object contract: scaled semi-implicit Euler, acceleration first.
    // No unit-rate branch, rotation, limits or reverse-resolved velocity.
    const double vx=m.vx+m.ax*m.rate,vy=m.vy+m.ay*m.rate;
    const double x=t.x+vx*m.rate,y=t.y+vy*m.rate;
    for(double value:{vx,vy,x,y})
        if(!std::isfinite(value)) throw std::overflow_error("Object motion result is not finite");
    object.motion.vx=vx; object.motion.vy=vy;
    object.transform.x=x; object.transform.y=y;
}
}
