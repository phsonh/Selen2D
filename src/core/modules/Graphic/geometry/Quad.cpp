#include "Quad.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace core::modules::Graphic::Geometry {
namespace {
constexpr double geometryEpsilon=1e-12;
constexpr double solveEpsilon=1e-12;
constexpr double checkEpsilon=1e-8;
double Cross(Point a,Point b) { return a[0]*b[1]-a[1]*b[0]; }
Point Difference(Point a,Point b) { return {a[0]-b[0],a[1]-b[1]}; }
Quad Normalize(const Quad& input) {
    Quad output{}; double scale{};
    for(const auto& p:input) for(double v:p)
        if(!std::isfinite(v)) throw std::invalid_argument("Quad positions and UV must be finite");
    // Subtract a nearby point first; large translations should not dominate
    // the tolerance. Reject overflow instead of letting it reach the solver.
    for(std::size_t i=0;i<4;++i) for(std::size_t c=0;c<2;++c) {
        const double d=input[i][c]-input[0][c];
        if(!std::isfinite(d)) throw std::invalid_argument("Quad coordinate span exceeds numeric range");
        output[i][c]=d; scale=std::max(scale,std::abs(d));
    }
    if(scale==0) return output; // Constant UV; geometry validation rejects this for positions.
    Point center{};
    for(auto& p:output) for(std::size_t c=0;c<2;++c) { p[c]/=scale; center[c]+=p[c]/4; }
    for(auto& p:output) for(std::size_t c=0;c<2;++c) p[c]-=center[c];
    return output;
}
void ValidateNormalized(const Quad& p) {
    double sign{};
    for(std::size_t i=0;i<4;++i) {
        const auto a=Difference(p[(i+1)%4],p[i]);
        const auto b=Difference(p[(i+2)%4],p[(i+1)%4]);
        const double cross=Cross(a,b);
        if(std::abs(cross)<=geometryEpsilon || (i && std::signbit(cross)!=std::signbit(sign)))
            throw std::invalid_argument("Quad must be strictly convex with distinct, non-collinear contour vertices");
        sign=cross;
    }
}
bool Affine(const Quad& p,const Quad& uv) {
    const auto a=Difference(p[1],p[0]),b=Difference(p[2],p[0]),c=Difference(p[3],p[0]);
    const double determinant=Cross(a,b);
    for(std::size_t axis=0;axis<2;++axis) {
        const double u=uv[1][axis]-uv[0][axis],v=uv[2][axis]-uv[0][axis];
        const double dx=(u*b[1]-v*a[1])/determinant,dy=(a[0]*v-b[0]*u)/determinant;
        if(std::abs(uv[0][axis]+dx*c[0]+dy*c[1]-uv[3][axis])>1e-11) return false;
    }
    return true;
}
}
void ValidateQuad(const Quad& positions) { ValidateNormalized(Normalize(positions)); }
std::array<double,4> ProjectiveWeights(const Quad& positions,const Quad& texture) {
    const auto p=Normalize(positions),uv=Normalize(texture);
    ValidateNormalized(p);
    if(Affine(p,uv)) return {1,1,1,1};
    // h33=1 is safe for a pole-free map: p's centroid is zero and lies inside
    // the convex domain, hence D(0,0) cannot be zero for any accepted solution.
    std::array<std::array<double,9>,8> matrix{};
    for(std::size_t i=0;i<4;++i) {
        const double x=p[i][0],y=p[i][1],u=uv[i][0],v=uv[i][1];
        matrix[2*i]  ={x,y,1,0,0,0,-u*x,-u*y,u};
        matrix[2*i+1]={0,0,0,x,y,1,-v*x,-v*y,v};
    }
    for(std::size_t column=0;column<8;++column) {
        std::size_t pivot=column;
        for(std::size_t row=column+1;row<8;++row)
            if(std::abs(matrix[row][column])>std::abs(matrix[pivot][column])) pivot=row;
        if(std::abs(matrix[pivot][column])<=solveEpsilon)
            throw std::invalid_argument("Quad UV mapping is nonunique or numerically singular");
        std::swap(matrix[column],matrix[pivot]);
        const double divisor=matrix[column][column];
        for(std::size_t c=column;c<9;++c) matrix[column][c]/=divisor;
        for(std::size_t row=0;row<8;++row) if(row!=column) {
            const double factor=matrix[row][column];
            for(std::size_t c=column;c<9;++c) matrix[row][c]-=factor*matrix[column][c];
        }
    }
    std::array<double,8> h{};
    for(std::size_t i=0;i<8;++i) {
        h[i]=matrix[i][8];
        if(!std::isfinite(h[i])) throw std::invalid_argument("Quad UV mapping exceeds numeric range");
    }
    std::array<double,4> result{}; double maximum{};
    for(std::size_t i=0;i<4;++i) {
        const double x=p[i][0],y=p[i][1];
        const double q=h[6]*x+h[7]*y+1;
        if(!std::isfinite(q) || q<=0) throw std::invalid_argument("Quad UV mapping has a pole in its domain");
        result[i]=q; maximum=std::max(maximum,q);
        for(std::size_t c=0;c<2;++c) {
            const double value=(h[c*3]*x+h[c*3+1]*y+h[c*3+2])/q;
            if(!std::isfinite(value) || std::abs(value-uv[i][c])>checkEpsilon)
                throw std::invalid_argument("Quad UV mapping is numerically unstable");
        }
    }
    for(auto& q:result) {
        q/=maximum;
        if(q<1e-6) throw std::invalid_argument("Quad UV denominator is too close to a pole for GPU interpolation");
    }
    return result;
}
}
