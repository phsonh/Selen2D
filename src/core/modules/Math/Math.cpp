#include "Math.h"
#include "core/runtime/Log/Log.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>

namespace core::modules::Math {
    namespace {
        constexpr double DegreesToRadians = std::numbers::pi / 180.0;
        constexpr double RadiansToDegrees = 180.0 / std::numbers::pi;
        constexpr double ExactIntegerLimit = 9007199254740991.0;
        constexpr std::uint64_t FullRange = std::uint64_t{1} << 32;
        bool initialized = false;

        struct RandomState {
            std::uint32_t x, y, z, w;
        };

        constexpr std::uint64_t SplitMix64Next(std::uint64_t& state) {
            auto z = (state += 0x9E3779B97F4A7C15ULL);
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
            return z ^ (z >> 31);
        }

        constexpr RandomState ExpandSeed(std::uint32_t seed) {
            std::uint64_t expansion = seed;
            RandomState result{};
            result.x = static_cast<std::uint32_t>(SplitMix64Next(expansion));
            result.y = static_cast<std::uint32_t>(SplitMix64Next(expansion));
            result.z = static_cast<std::uint32_t>(SplitMix64Next(expansion));
            result.w = static_cast<std::uint32_t>(SplitMix64Next(expansion));
            if ((result.x | result.y | result.z | result.w) == 0) result.w = 1;
            return result;
        }

        RandomState randomState = ExpandSeed(0);

        std::uint32_t NextU32() {
            const std::uint32_t t = randomState.x ^ (randomState.x << 11);
            randomState.x = randomState.y;
            randomState.y = randomState.z;
            randomState.z = randomState.w;
            randomState.w = randomState.w ^ (randomState.w >> 19) ^ t ^ (t >> 8);
            return randomState.w;
        }

        void RequireFinite(double value, const char* argument) {
            if (!std::isfinite(value))
                throw std::invalid_argument(std::string(argument) + " must be finite");
        }

        void RequireInteger(double value, double minimum, double maximum, const char* argument) {
            RequireFinite(value, argument);
            if (value < minimum || value > maximum || std::trunc(value) != value)
                throw std::invalid_argument(std::string(argument) + " must be an integer in the supported range");
        }

        void RequireUnit(double value, const char* argument) {
            RequireFinite(value, argument);
            if (value < -1 || value > 1)
                throw std::invalid_argument(std::string(argument) + " must be in [-1, 1]");
        }
    }

    bool Init() {
        if (initialized) return true;
        randomState = ExpandSeed(0);
        initialized = true;
        core::runtime::Log::Debug("Math module initialized");
        return true;
    }

    void Shutdown() {
        if (!initialized) return;
        initialized = false;
        core::runtime::Log::Debug("Math module shutdown");
    }

    double sin(double degrees) {
        RequireFinite(degrees, "Math.sin degrees");
        return std::sin(degrees * DegreesToRadians);
    }

    double cos(double degrees) {
        RequireFinite(degrees, "Math.cos degrees");
        return std::cos(degrees * DegreesToRadians);
    }

    double tan(double degrees) {
        RequireFinite(degrees, "Math.tan degrees");
        return std::tan(degrees * DegreesToRadians);
    }

    double asin(double value) {
        RequireUnit(value, "Math.asin value");
        return std::asin(value) * RadiansToDegrees;
    }

    double acos(double value) {
        RequireUnit(value, "Math.acos value");
        return std::acos(value) * RadiansToDegrees;
    }

    double atan2(double y, double x) {
        RequireFinite(y, "Math.atan2 y");
        RequireFinite(x, "Math.atan2 x");
        if (x == 0 && y == 0) return 0;
        return std::atan2(y, x) * RadiansToDegrees;
    }

    namespace Random {
        void Seed(double seed) {
            RequireInteger(seed, 0, 4294967295.0, "Math.Random.Seed seed");
            randomState = ExpandSeed(static_cast<std::uint32_t>(seed));
        }

        std::int64_t Int(double begin, double end) {
            RequireInteger(begin, -ExactIntegerLimit, ExactIntegerLimit, "Math.Random.Int begin");
            RequireInteger(end, -ExactIntegerLimit, ExactIntegerLimit, "Math.Random.Int end");
            if (begin > end) throw std::invalid_argument("Math.Random.Int requires begin <= end");
            const auto first = static_cast<std::int64_t>(begin);
            const auto last = static_cast<std::int64_t>(end);
            const auto range = static_cast<std::uint64_t>(last - first) + 1;
            if (range > FullRange) throw std::invalid_argument("Math.Random.Int range must not exceed 2^32");
            if (range == 1) return first; // Degenerate and invalid calls never advance the generator.
            if (range == FullRange) return first + static_cast<std::int64_t>(NextU32());

            const auto n = static_cast<std::uint32_t>(range);
            const std::uint32_t threshold = (std::uint32_t{0} - n) % n;
            std::uint32_t sample;
            do { sample = NextU32(); } while (sample < threshold);
            return first + static_cast<std::int64_t>(sample % n);
        }

        double Float(double begin, double end) {
            RequireFinite(begin, "Math.Random.Float begin");
            RequireFinite(end, "Math.Random.Float end");
            if (begin > end) throw std::invalid_argument("Math.Random.Float requires begin <= end");
            if (begin == end) return begin;

            const double t = static_cast<double>(NextU32()) / 4294967296.0;
            double result;
            // Opposite-sign, very large finite endpoints can have an infinite difference.
            // A half-width test avoids even evaluating that overflowing subtraction.
            const bool wide = begin < 0 && end > 0
                && end * 0.5 - begin * 0.5 > std::numeric_limits<double>::max() * 0.5;
            if (wide) {
                // Both products are finite, with opposite signs. No float narrowing.
                result = begin * (1.0 - t) + end * t;
            } else {
                const double width = end - begin;
                result = begin + width * t;
            }
            if (result >= end) return std::nextafter(end, begin);
            return std::max(begin, result);
        }

        int Sign() {
            return (NextU32() & 1U) == 0 ? -1 : 1;
        }
    }
}
