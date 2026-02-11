#include <Core/Timer.hpp>

namespace Sleak {
    Timer::Timer() {
        Reset();
    }

    void Timer::Reset() {
        startPoint = std::chrono::high_resolution_clock::now();
    }

    // Outputs elaosed time as seconds
    float Timer::Elapsed() const {
        auto now = std::chrono::high_resolution_clock::now();
        std::chrono::duration<float> elapsed = now - startPoint;
        return elapsed.count();
    }
};