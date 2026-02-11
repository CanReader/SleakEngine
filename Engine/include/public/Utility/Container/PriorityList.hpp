#ifndef _PRIORITYLIST_H_
#define _PRIORITYLIST_H_

#include <Utility/List.hpp>
#include <Utility/Pair.hpp>

namespace Sleak {

    /**
     * @class PriorityList
     * @brief A list where elements are stored with associated priorities.
     *
     * This class provides a way to store elements with priorities and
     * efficiently retrieve or remove the element with the highest priority.
     * It is suitable for scenarios where you need to manage a collection of
     * items with different priorities, such as task scheduling, event handling,
     * or AI decision-making.
     *
     * Example Use Cases:
     * - Task scheduling based on priority levels.
     * - Event handling where events have different priorities.
     * - AI decision-making where actions have different priorities.
     *
     * Implementation Details:
     * - Uses a `List` to store pairs of (element, priority).
     * - Elements are kept sorted by priority for efficient retrieval.
     * - Provides methods for inserting elements with priorities,
     * retrieving the highest priority element, and removing it.
     */
    template <typename T>
    class PriorityList {
    public:
        /**
         * @brief Inserts an element with the given priority.
         *
         * @param element The element to insert.
         * @param priority The priority of the element.
         */
        void insert(const T& element, int priority) {
            // Find the correct position to insert based on priority (keep the list sorted)
            size_t index = 0;
            while (index < data.GetSize() && data[index].second > priority) {
                ++index;
            }
            data.insert(index, std::make_pair(element, priority));
        }

        /**
         * @brief Returns the element with the highest priority.
         *
         * @return The element with the highest priority.
         * @throw std::runtime_error if the list is empty.
         */
        T getHighestPriorityElement() const {
            if (data.GetSize() == 0) {
                throw std::runtime_error("Priority list is empty");
            }
            return data[0].first;
        }

        /**
         * @brief Removes the element with the highest priority.
         *
         * @throw std::runtime_error if the list is empty.
         */
        void removeHighestPriorityElement() {
            if (data.GetSize() == 0) {
                throw std::runtime_error("Priority list is empty");
            }
            data.insert(0, List<Pair<T, int>>().begin(), List<Pair<T, int>>().end());
        }

    private:
        List<Pair<T, int>> data; // Use Pair to store element and priority
    };
}

#endif // _PRIORITYLIST_H_