#pragma once

#include <cstdint>

namespace core::modules::Math {
    bool Init();
    void Shutdown();

    // Public angles are degrees. Invalid numeric domains throw invalid_argument.
    double sin(double degrees);
    double cos(double degrees);
    double tan(double degrees);
    double asin(double value);
    double acos(double value);
    double atan2(double y, double x);

    // One engine-wide deterministic sequence, accessed on the Lua/host thread.
    // Double arguments retain validation before any narrowing integer conversion.
    namespace Random {
        void Seed(double seed);
        std::int64_t Int(double begin, double end);
        double Float(double begin, double end);
        int Sign();
    }
}
