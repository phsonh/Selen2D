#include "Collision.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace core::modules::Object::Collision {
namespace {
constexpr double epsilon=64*std::numeric_limits<double>::epsilon();
struct Vec { double x{},y{}; };
double Dot(Vec a,Vec b) { return std::fma(a.x,b.x,a.y*b.y); }
double Magnitude(Vec a,Vec b) { return std::abs(a.x*b.x)+std::abs(a.y*b.y); }
Vec Other(Vec a) { return {-a.y,a.x}; }
Vec Axis(double degrees) {
    const double d=std::remainder(degrees,360.);
    if(d==0) return {1,0};
    if(d==90) return {0,1};
    if(d==-90) return {0,-1};
    if(std::abs(d)==180) return {-1,0};
    const double r=d*0.01745329251994329577;
    return {std::cos(r),std::sin(r)};
}
void Validate(const Collider& c) {
    for(double v:{c.x,c.y,c.a,c.b,c.rotationDegrees})
        if(!std::isfinite(v)) throw std::invalid_argument("Collider fields must be finite");
    if(c.a<0 || c.b<0) throw std::invalid_argument("Collider a/b must be non-negative");
}
bool Within(double distance,double radius,double errorScale) {
    return distance<=radius || distance-radius<=epsilon*errorScale;
}
struct Shape {
    double a{},b{};
    Vec axis;
    double Radius() const { return (a+b)*.5; } // Extents already normalized; no overflow.
    double Projection(Vec direction) const { return a*std::abs(Dot(axis,direction))+b*std::abs(Dot(Other(axis),direction)); }
};
Shape Make(const Collider& c,double scale) {
    Shape s{c.a/scale,c.b/scale,c.rectangle?Axis(c.rotationDegrees):Vec{1,0}};
    if((c.a>0 && s.a==0) || (c.b>0 && s.b==0))
        throw std::overflow_error("Collider scale exceeds double precision");
    return s;
}
}
double ConservativeRadius(const Collider& c) noexcept {
    constexpr double maximum=0x1p20,minimum=0x1p-20;
    const auto extent=[](double v) { return v==0 || (v>=minimum && v<=maximum); };
    if(!(std::abs(c.x)<=maximum && std::abs(c.y)<=maximum)
        || !extent(c.a) || !extent(c.b) || !std::isfinite(c.rotationDegrees)) return -1;
    if(c.rectangle) {
        // Use the same axes as the narrow phase. Check their actual norm so the
        // bound does not depend on a platform libm's advertised sin/cos accuracy.
        const auto axis=Axis(c.rotationDegrees);
        const double norm=Dot(axis,axis);
        constexpr double rounding=8*std::numeric_limits<double>::epsilon();
        if(!(norm>=1-rounding && norm<=1+rounding)) return -1;
        return c.a+c.b;
    }
    return (c.a+c.b)*.5;
}
bool Separated(double dx,double dy,double firstRadius,double secondRadius) noexcept {
    if(firstRadius<0 || secondRadius<0) return false;
    // In the admitted range scale <= 2^21 and nonzero normalized extents >=
    // 2^-41: the narrow phase cannot overflow/underflow. Its largest normalized
    // errorScale is 10 before rounding (OBB SAT). A conservative allowance of
    // 4096 machine epsilons times scale (<= 2^-19) covers its contact slack, the
    // checked axis norm and rounding in normalization/projections/subtraction.
    // The 2^-16 enclosure is larger still; it only admits more narrow tests.
    // Outside that range ConservativeRadius returns -1, preserving old errors.
    const double radius=firstRadius+secondRadius+0x1p-16;
    return std::abs(dx)>radius || std::abs(dy)>radius;
}
bool Intersects(const Collider& first,const Collider& second) {
    Validate(first); Validate(second);
    Vec d{second.x-first.x,second.y-first.y};
    if(!std::isfinite(d.x) || !std::isfinite(d.y)) throw std::overflow_error("Collider center difference overflow");
    const double scale=std::max({std::abs(d.x),std::abs(d.y),first.a,first.b,second.a,second.b});
    if(scale==0) return true;
    d={d.x/scale,d.y/scale};
    const auto a=Make(first,scale),b=Make(second,scale);
    if(!first.rectangle && !second.rectangle) {
        const double distance=std::hypot(d.x,d.y),radius=a.Radius()+b.Radius();
        return Within(distance,radius,std::max(distance,radius));
    }
    if(first.rectangle && second.rectangle) {
        for(auto axis:std::array<Vec,4>{a.axis,Other(a.axis),b.axis,Other(b.axis)}) {
            const double distance=std::abs(Dot(d,axis)),radius=a.Projection(axis)+b.Projection(axis);
            const double error= Magnitude(d,axis)+a.a*Magnitude(a.axis,axis)+a.b*Magnitude(Other(a.axis),axis)
                +b.a*Magnitude(b.axis,axis)+b.b*Magnitude(Other(b.axis),axis);
            if(!Within(distance,radius,error)) return false;
        }
        return true;
    }
    const auto& box=first.rectangle?a:b;
    const auto& circle=first.rectangle?b:a;
    // Closest point in the rectangle's local axes. Abs makes delta orientation irrelevant.
    const double x=std::abs(Dot(d,box.axis)),y=std::abs(Dot(d,Other(box.axis)));
    const double distance=std::hypot(std::max(0.,x-box.a),std::max(0.,y-box.b));
    const double radius=circle.Radius();
    return Within(distance,radius,Magnitude(d,box.axis)+Magnitude(d,Other(box.axis))+box.a+box.b+radius);
}
}
