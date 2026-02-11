#ifndef _PAIR_H_
#define _PAIR_H_

#include <utility> // For std::forward, std::swap (optional)

namespace Sleak {

    /**
     * @class Pair
     * @brief A simple container for storing two values as a pair.
     *
     * This class provides a way to store two values together as a single unit.
     * It is similar to `std::pair` and can be used in similar situations,
     * such as storing key-value pairs, coordinates, or any other data
     * that needs to be grouped together.
     *
     * Example Use Cases:
     * - Storing key-value pairs in a map or hash table.
     * - Representing coordinates (x, y) or (latitude, longitude).
     * - Returning multiple values from a function.
     *
     * Implementation Details:
     * - Stores two values of potentially different types.
     * - Provides access to the values through `first` and `second` members.
     * - Implements comparison operators for lexicographical ordering.
     * - Offers a `makePair` function for convenient creation of `Pair` objects.
     */
    template <typename T1, typename T2>
    class Pair {
    public:
        // Member variables to store the first and second values
        T1 first;
        T2 second;

        /**
         * @brief Default constructor.
         */
        Pair() = default;

        /**
         * @brief Constructor with initial values.
         *
         * @param first The initial value for the first member.
         * @param second The initial value for the second member.
         */
        Pair(const T1& first, const T2& second) : first(first), second(second) {}

        /**
         * @brief Copy constructor.
         *
         * @param other The `Pair` object to copy from.
         */
        Pair(const Pair& other) = default;

        /**
         * @brief Move constructor.
         *
         * @param other The `Pair` object to move from.
         */
        Pair(Pair&& other) noexcept
            : first(std::forward<T1>(other.first)),
              second(std::forward<T2>(other.second)) {}

        /**
         * @brief Copy assignment operator.
         *
         * @param other The `Pair` object to copy from.
         * @return A reference to the assigned `Pair` object.
         */
        Pair& operator=(const Pair& other) = default;

        /**
         * @brief Move assignment operator.
         *
         * @param other The `Pair` object to move from.
         * @return A reference to the assigned `Pair` object.
         */
        Pair& operator=(Pair&& other) noexcept {
            if (this != &other) {
                first = std::forward<T1>(other.first);
                second = std::forward<T2>(other.second);
            }
            return *this;
        }

        /**
         * @brief Swaps the contents of this `Pair` with another `Pair`.
         *
         * @param other The `Pair` object to swap with.
         */
        void swap(Pair& other) noexcept {
            using std::swap; // Bring std::swap into scope (optional)
            swap(first, other.first);
            swap(second, other.second);
        }
    };

    /**
     * @brief Creates a `Pair` object with the given values.
     *
     * This is a helper function for convenient creation of `Pair` objects.
     *
     * @param first The first value.
     * @param second The second value.
     * @return A `Pair` object containing the given values.
     */
    template <typename T1, typename T2>
    Pair<T1, T2> makePair(T1&& first, T2&& second) {
        return Pair<T1, T2>(std::forward<T1>(first), std::forward<T2>(second));
    }

    // Comparison operators (implementing lexicographical comparison)
    template <typename T1, typename T2>
    bool operator==(const Pair<T1, T2>& lhs, const Pair<T1, T2>& rhs) {
        return lhs.first == rhs.first && lhs.second == rhs.second;
    }

    template <typename T1, typename T2>
    bool operator!=(const Pair<T1, T2>& lhs, const Pair<T1, T2>& rhs) {
        return !(lhs == rhs);
    }

    template <typename T1, typename T2>
    bool operator<(const Pair<T1, T2>& lhs, const Pair<T1, T2>& rhs) {
        if (lhs.first < rhs.first) return true;
        if (rhs.first < lhs.first) return false;
        return lhs.second < rhs.second;
    }

    template <typename T1, typename T2>
    bool operator<=(const Pair<T1, T2>& lhs, const Pair<T1, T2>& rhs) {
        return !(rhs < lhs);
    }

    template <typename T1, typename T2>
    bool operator>(const Pair<T1, T2>& lhs, const Pair<T1, T2>& rhs) {
        return rhs < lhs;
    }

    template <typename T1, typename T2>
    bool operator>=(const Pair<T1, T2>& lhs, const Pair<T1, T2>& rhs) {
        return !(lhs < rhs);
    }
}

#endif // _PAIR_H_