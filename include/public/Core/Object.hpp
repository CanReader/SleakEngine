#ifndef _OBJECT_H_
#define _OBJECT_H_

#include <cstdint>
#include <string>
#include <atomic>

namespace Sleak {
    class Object {
    public:
        Object(const std::string& name = "Object")
         : m_uniqueID(s_nextUniqueID.fetch_add(1, std::memory_order_relaxed)),
           m_name(name)
        {
            SetName(m_name + "-" + std::to_string(m_uniqueID));
        }

        virtual ~Object() = default;

        uint64_t GetUniqueID() const { return m_uniqueID; }

        const std::string& GetName() const { return m_name; }

        inline void SetName(const std::string& value) { m_name = value; }

    private:
        std::string m_name;
        uint64_t m_uniqueID;
        static inline std::atomic<uint64_t> s_nextUniqueID = 0;
    };
}

#endif
