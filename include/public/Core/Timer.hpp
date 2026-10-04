#ifndef _TIMER_HPP_
#define _TIMER_HPP_

#include <chrono>

namespace Sleak {
    /// Wall-clock stopwatch built on std::chrono. Application keeps one for
    /// frame delta time.
    /// @ingroup core
    class Timer {
        public:
            Timer();

            /// Restarts the clock at zero.
            void Reset();

            /// Seconds since construction or the last Reset().
            float Elapsed() const;

            /// Same as Elapsed() in double precision, for subtracting two
            /// readings that are hours into a session.
            double ElapsedSeconds() const;

        private:
            std::chrono::time_point<std::chrono::steady_clock> startPoint;
    };
};

#endif
