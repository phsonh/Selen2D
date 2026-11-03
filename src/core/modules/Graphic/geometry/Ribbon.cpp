#include "Ribbon.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace core::modules::Graphic::Geometry {
namespace {
constexpr double epsilon=64*std::numeric_limits<double>::epsilon();
Point Sub(Point a,Point b) { return {a[0]-b[0],a[1]-b[1]}; }
double Length(Point p) { return std::hypot(p[0],p[1]); }
double Dot(Point a,Point b) { return a[0]*b[0]+a[1]*b[1]; }
double Cross(Point a,Point b) { return a[0]*b[1]-a[1]*b[0]; }
Point Normal(Point p) { return {-p[1],p[0]}; }
Point Offset(Point p,Point n,double amount) {
    Point result{p[0]+n[0]*amount,p[1]+n[1]*amount};
    if(!std::isfinite(result[0]) || !std::isfinite(result[1])) throw std::invalid_argument("Ribbon offset exceeds numeric range");
    return result;
}
}
std::span<const RibbonVertex> RibbonBuilder::Build(std::span<const Point> input,double width,bool closed) {
    vertices_.clear(); points_.clear();
    if(input.size()>maximumPoints) throw std::invalid_argument("Ribbon exceeds 16384 input points");
    if(input.size()<2 || !std::isfinite(width) || width<=0)
        throw std::invalid_argument("Ribbon requires positive finite width and valid points");
    double extent{};
    for(auto p:input) {
        if(!std::isfinite(p[0]) || !std::isfinite(p[1])) throw std::invalid_argument("Ribbon points must be finite");
        extent=std::max(extent,Length(Sub(p,input.front())));
    }
    if(!std::isfinite(extent) || extent==0) throw std::invalid_argument("Ribbon path has no representable extent");
    const double tolerance=extent*epsilon;
    points_.reserve(input.size());
    for(auto p:input) if(points_.empty() || Length(Sub(p,points_.back()))>tolerance) points_.push_back(p);
    while(closed && points_.size()>1 && Length(Sub(points_.back(),points_.front()))<=tolerance) points_.pop_back();
    const auto count=points_.size();
    if(count<(closed?3u:2u)) throw std::invalid_argument("Ribbon needs two distinct points (three when closed)");
    const auto segments=closed?count:count-1;
    tangents_.resize(segments); distance_.assign(count,0);
    double total{};
    for(std::size_t i=0;i<segments;++i) {
        const auto delta=Sub(points_[(i+1)%count],points_[i]);
        const auto length=Length(delta);
        if(!std::isfinite(length) || length<=tolerance) throw std::invalid_argument("Ribbon contains an unresolved segment");
        tangents_[i]={delta[0]/length,delta[1]/length};
        total+=length;
        if(i+1<count) distance_[i+1]=total;
    }
    if(!std::isfinite(total)) throw std::invalid_argument("Ribbon arc length exceeds numeric range");
    if(closed) {
        // Signed area is unsuitable here: a valid self-crossing contour can
        // have zero area. Reject only an entirely collinear centerline.
        bool noncollinear{};
        for(std::size_t i=2;i<count;++i) {
            const auto d=Sub(points_[i],points_.front());
            if(std::abs(Cross(tangents_.front(),{d[0]/extent,d[1]/extent}))>epsilon) { noncollinear=true; break; }
        }
        if(!noncollinear) throw std::invalid_argument("Closed Ribbon cannot be collinear");
    }
    const double half=width/2;
    if(half==0) throw std::invalid_argument("Ribbon width is too small to represent");
    joins_.resize(count);
    for(std::size_t i=0;i<count;++i) {
        auto& join=joins_[i]; join={}; const auto p=points_[i];
        if(!closed && (i==0 || i==count-1)) {
            const auto n=Normal(tangents_[i==0?0:segments-1]);
            join.incoming=join.outgoing={Offset(p,n,half),Offset(p,n,-half)}; continue;
        }
        const auto incoming=tangents_[(i+segments-1)%segments],outgoing=tangents_[i%segments];
        const Point sum{incoming[0]+outgoing[0],incoming[1]+outgoing[1]};
        const auto sumLength=Length(sum);
        if(sumLength<=epsilon) throw std::invalid_argument("Ribbon cannot contain a 180-degree reversal");
        const Point miter=Normal({sum[0]/sumLength,sum[1]/sumLength});
        const double denominator=Dot(miter,Normal(outgoing));
        if(denominator>1/miterLimit) {
            const auto offset=half/denominator;
            join.incoming=join.outgoing={Offset(p,miter,offset),Offset(p,miter,-offset)};
        } else {
            // A sharp return uses an outer bevel and an inner bevel at the
            // centerline. Keeping the huge inner intersection would still spike
            // or invert short segments even after limiting the outer miter.
            join.bevel=true; join.leftTurn=Cross(incoming,outgoing)>0;
            if(join.leftTurn) {
                join.incoming={p,Offset(p,Normal(incoming),-half)};
                join.outgoing={p,Offset(p,Normal(outgoing),-half)};
            } else {
                join.incoming={Offset(p,Normal(incoming),half),p};
                join.outgoing={Offset(p,Normal(outgoing),half),p};
            }
        }
    }
    vertices_.reserve(segments*6+count*3);
    // U spans the width; V follows normalized arc length. Both seam copies
    // retain identical positions/width and differ only in V=0 versus V=1.
    auto push=[&](Point p,double along,double across) { vertices_.push_back({p,across,along}); };
    for(std::size_t i=0;i<segments;++i) {
        const auto& j=joins_[i]; const auto& next=joins_[(i+1)%count];
        const double u=distance_[i]/total, end=i+1==count?1:distance_[i+1]/total;
        if(j.bevel) {
            if(j.leftTurn) {
                push(j.incoming.left,u,0); push(j.incoming.right,u,1); push(j.outgoing.right,u,1);
            } else {
                push(j.incoming.right,u,1); push(j.outgoing.left,u,0); push(j.incoming.left,u,0);
            }
        }
        push(j.outgoing.left,u,0); push(j.outgoing.right,u,1); push(next.incoming.right,end,1);
        push(j.outgoing.left,u,0); push(next.incoming.right,end,1); push(next.incoming.left,end,0);
    }
    return vertices_;
}
}
