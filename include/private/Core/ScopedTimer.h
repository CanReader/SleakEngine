#ifndef _SCOPEDTIMER_HPP_
#define _SCOPEDTIMER_HPP_

#include "Logger.hpp"

namespace Sleak {
    class ScopedTimer {
        public:
            ScopedTimer(const std::string& name)
                : m_Name(name), m_Start(std::chrono::high_resolution_clock::now()) {}
        
            ~ScopedTimer() {
                auto end = std::chrono::high_resolution_clock::now();
                auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - m_Start);
                SLEAK_INFO("{0} took {1} ms",m_Name,duration.count())
            }
            
        private:
            std::string m_Name;
            std::chrono::time_point<std::chrono::high_resolution_clock> m_Start;
        };
}

#endif