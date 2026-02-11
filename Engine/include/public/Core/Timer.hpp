#ifndef _TIMER_HPP_
#define _TIMER_HPP_

#include <chrono>

namespace Sleak {
    class Timer {
        public:
            Timer();

            void Reset();

            float Elapsed() const;

        private:
            std::chrono::time_point<std::chrono::high_resolution_clock> startPoint;
    };
};

#endif