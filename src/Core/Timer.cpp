#include <Core/Timer.hpp>

namespace Sleak {
    Timer::Timer() {
        Reset();
    }

    void Timer::Reset() {
        startPoint = std::chrono::steady_clock::now();
    }

    float Timer::Elapsed() const {
        return static_cast<float>(ElapsedSeconds());
    }

    double Timer::ElapsedSeconds() const {
        std::chrono::duration<double> elapsed =
            std::chrono::steady_clock::now() - startPoint;
        return elapsed.count();
    }
};
