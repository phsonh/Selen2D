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

    // Each Lua generator owns this value; the binding selects one per Lua VM.
    namespace Random {
        struct State {
            std::uint32_t seed{}, x{}, y{}, z{}, w{};
            bool operator==(const State&) const = default;
        };
        class Generator {
        public:
            explicit Generator(double seed = 0);
            void Seed(double seed);
            std::uint32_t GetSeed() const noexcept;
            State GetState() const noexcept;
            void SetState(State state);
            std::int64_t Int(double begin, double end);
            double Float(double begin, double end);
            int Sign();
        private:
            State state_;
            std::uint32_t NextU32() noexcept;
        };
        // Native host compatibility; Lua entry points use their own Generator.
        void Seed(double seed);
        std::int64_t Int(double begin, double end);
        double Float(double begin, double end);
        int Sign();
    }
}
