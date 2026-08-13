#ifndef _TIMER_HPP_
#define _TIMER_HPP_

#include <chrono>

namespace Sleak {
    /// Wall-clock stopwatch built on std::chrono. Application keeps one for
    /// frame delta time.
    class Timer {
        public:
            Timer();

            /// Restarts the clock at zero.
            void Reset();

            /// Seconds since construction or the last Reset().
            float Elapsed() const;

        private:
            std::chrono::time_point<std::chrono::high_resolution_clock> startPoint;
    };
};

#endif