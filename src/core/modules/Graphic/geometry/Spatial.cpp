#include "Spatial.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace core::modules::Graphic::Geometry {
namespace {
constexpr double directionTolerance=1e-12,coplanarTolerance=1e-8;
double Dot(Point3 a,Point3 b) { return std::fma(a[0],b[0],std::fma(a[1],b[1],a[2]*b[2])); }
Point3 Cross(Point3 a,Point3 b) { return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]}; }
double MaxComponent(Point3 p) { return std::max({std::abs(p[0]),std::abs(p[1]),std::abs(p[2])}); }
Point3 Unit(Point3 p) {
    ValidateDirection(p);
    const double scale=MaxComponent(p);
    for(auto& v:p) v/=scale;
    const double length=std::hypot(p[0],p[1],p[2]);
    for(auto& v:p) v/=length;
    return p;
}
Point3 Difference(Point3 a,Point3 b) {
    Point3 result{a[0]-b[0],a[1]-b[1],a[2]-b[2]}; ValidatePoint3(result); return result;
}
}
void ValidatePoint3(const Point3& p) {
    for(double v:p) if(!std::isfinite(v)) throw std::invalid_argument("3D coordinates must be finite and within numeric range");
}
void ValidateDirection(const Point3& p) {
    ValidatePoint3(p);
    if(MaxComponent(p)==0) throw std::invalid_argument("Camera direction must be nonzero");
}
void ValidatePerspective(const Perspective& p) {
    if(!std::isfinite(p.fov) || !std::isfinite(p.nearPlane) || !std::isfinite(p.farPlane) ||
        p.fov<=0 || p.fov>=180 || p.nearPlane<=0 || p.farPlane<=p.nearPlane)
        throw std::invalid_argument("Perspective requires 0<fov<180, 0<near<far and finite values");
}
void ValidateFog(const Fog& f) {
    if(!std::isfinite(f.start) || !std::isfinite(f.end) || !std::isfinite(f.end-f.start) ||
        f.start>=f.end || f.mode<0 || f.mode>6 ||
        f.rgb<FogRGB::Lerp || f.rgb>FogRGB::Screen || f.alpha<FogAlpha::Keep || f.alpha>FogAlpha::Fade)
        throw std::invalid_argument("Fog requires finite start<end, mode 0..6 and valid RGB/Alpha operations");
}
View MakeView(const Camera& camera,const Perspective& perspective,std::uint32_t width,std::uint32_t height) {
    ValidatePoint3(camera.position); ValidatePerspective(perspective);
    if(!width || !height) throw std::invalid_argument("Perspective Canvas dimensions must be positive");
    const auto forward=Unit(camera.look),upHint=Unit(camera.up);
    const auto cross=Cross(forward,upHint);
    if(std::hypot(cross[0],cross[1],cross[2])<=directionTolerance)
        throw std::invalid_argument("Camera look and up must not be parallel or numerically unresolved");
    const auto right=Unit(cross),up=Unit(Cross(right,forward));
    // (right,up,-forward) is a right-handed view basis, no preferred world axis.
    const double sy=1/std::tan(perspective.fov*0.008726646259971647885);
    View view{camera.position,right,up,forward,sy*double(height)/width,sy,
        perspective.farPlane/(perspective.farPlane-perspective.nearPlane),perspective.nearPlane};
    for(double v:{view.sx,view.sy,view.depthScale})
        if(!std::isfinite(v)) throw std::invalid_argument("Perspective exceeds numeric range");
    return view;
}
Projected3 Project(const View& view,const Point3& point) {
    ValidatePoint3(point);
    Projected3 result{}; result.relative=Difference(point,view.origin);
    const double z=Dot(result.relative,view.forward);
    result.clip={Dot(result.relative,view.right)*view.sx,Dot(result.relative,view.up)*view.sy,
        (z-view.nearPlane)*view.depthScale,z};
    for(double v:result.clip) if(!std::isfinite(v)) throw std::invalid_argument("3D clip coordinates exceed numeric range");
    return result;
}
Quad FlattenQuad(const Quad3& points) {
    Quad3 local{}; double scale{};
    for(std::size_t i=0;i<4;++i) {
        ValidatePoint3(points[i]); local[i]=Difference(points[i],points[0]); scale=std::max(scale,MaxComponent(local[i]));
    }
    if(!scale) throw std::invalid_argument("3D Quad requires four distinct contour points");
    for(auto& p:local) for(auto& v:p) v/=scale;
    const auto normalRaw=Cross(local[1],local[2]);
    if(std::hypot(normalRaw[0],normalRaw[1],normalRaw[2])<=directionTolerance)
        throw std::invalid_argument("3D Quad has collinear or unresolved edges");
    const auto normal=Unit(normalRaw),x=Unit(local[1]),y=Unit(Cross(normal,x));
    Quad flat{};
    for(std::size_t i=0;i<4;++i) {
        if(std::abs(Dot(local[i],normal))>coplanarTolerance)
            throw std::invalid_argument("3D Quad vertices must be coplanar");
        flat[i]={Dot(local[i],x),Dot(local[i],y)};
    }
    ValidateQuad(flat); return flat;
}
double FogAmount(const Fog& fog,double distance) {
    ValidateFog(fog);
    if(!std::isfinite(distance) || distance<0) throw std::invalid_argument("Fog distance must be finite and nonnegative");
    const double t=distance<=fog.start?0:distance>=fog.end?1:(distance-fog.start)/(fog.end-fog.start);
    double factor=t;
    if(fog.mode>=1 && fog.mode<=3) {
        factor=t*t;
        for(int i=1;i<fog.mode;++i) factor*=t;
    } else if(fog.mode>=4) {
        const double s=1-t; factor=s*s;
        for(int i=4;i<fog.mode;++i) factor*=s;
        factor=1-factor;
    }
    return factor;
}
std::array<double,4> ApplyFog(const Fog& fog,double distance,std::array<double,4> pma) {
    for(double c:pma) if(!std::isfinite(c)) throw std::invalid_argument("Fog source color must be finite");
    if(pma[3]<0 || pma[3]>1) throw std::invalid_argument("Fog source alpha must be 0..1");
    const double amount=FogAmount(fog,distance);
    const double sourceAlpha=pma[3];
    double alpha=sourceAlpha;
    if(fog.alpha==FogAlpha::Mul) alpha*=1-amount+amount*double(fog.color>>24)/255;
    else if(fog.alpha==FogAlpha::Fade) alpha*=1-amount;
    for(int i=0;i<3;++i) {
        const double source=sourceAlpha>0 ? pma[i]/sourceAlpha : 0;
        const double target=double((fog.color>>(16-i*8))&255)/255;
        double color{};
        switch(fog.rgb) {
        case FogRGB::Lerp: color=std::lerp(source,target,amount); break;
        case FogRGB::Add: color=source+target*amount; break;
        case FogRGB::Sub: color=source-target*amount; break;
        case FogRGB::Mul: color=source*(1-amount+target*amount); break;
        case FogRGB::Screen: color=std::lerp(source,1-(1-source)*(1-target),amount); break;
        }
        pma[i]=std::clamp(color,0.,1.)*alpha;
    }
    pma[3]=alpha;
    return pma;
}
}
