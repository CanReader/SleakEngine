#ifndef _HASH_TABLE_H_
#define _HASH_TABLE_H_

#include <cstdint>  // For uint64_t
#include <utility>  // For std::move (can be removed if unwanted)

namespace Sleak
{
    /**
 * @class HashTable
 * @brief Implements a hash table (or map) for efficient key-value storage and retrieval.
 *
 * The HashTable class provides fast access to values based on their associated keys.
 * It is suitable for scenarios where you need to quickly look up data using unique
 * identifiers. Use this class when you need to store and retrieve data based on keys,
 * such as symbol tables, dictionaries, or caching systems.
 *
 * Example Use Cases:
 * - Implementing a symbol table in a compiler.
 * - Creating a dictionary or vocabulary lookup system.
 * - Caching frequently accessed data.
 * - Storing user preferences or configuration settings.
 *
 * Implementation Details:
 * - Uses a hash function to map keys to indices in an internal array.
 * - Handles collisions using chaining or open addressing (specify which one you used).
 * - Provides methods for inserting, deleting, and retrieving key-value pairs.
 */
    template <typename Key, typename Value>
    class HashTable
    {
    private:
        struct Entry
        {
            Key key;
            Value value;
            bool occupied;
            bool deleted;  // To handle tombstones in deletion

            Entry() : occupied(false), deleted(false) {}
        };

        Entry* table;
        size_t capacity;
        size_t size;
        float loadFactor;

        static constexpr size_t DEFAULT_CAPACITY = 16;
        static constexpr float DEFAULT_LOAD_FACTOR = 0.7f;

        size_t hash(const Key& key) const
        {
            return reinterpret_cast<uintptr_t>(&key) % capacity;
        }

        void resize()
        {
            size_t newCapacity = capacity * 2;
            Entry* newTable = new Entry[newCapacity];

            for (size_t i = 0; i < capacity; ++i)
            {
                if (table[i].occupied && !table[i].deleted)
                {
                    size_t index = hash(table[i].key) % newCapacity;
                    while (newTable[index].occupied)
                    {
                        index = (index + 1) % newCapacity;
                    }
                    newTable[index] = std::move(table[i]);
                }
            }

            delete[] table;
            table = newTable;
            capacity = newCapacity;
        }

    public:
        HashTable(size_t initCapacity = DEFAULT_CAPACITY, float loadFactor = DEFAULT_LOAD_FACTOR)
            : capacity(initCapacity), size(0), loadFactor(loadFactor)
        {
            table = new Entry[capacity];
        }

        ~HashTable()
        {
            delete[] table;
        }

        void insert(const Key& key, const Value& value)
        {
            if (size >= capacity * loadFactor)
            {
                resize();
            }

            size_t index = hash(key) % capacity;
            while (table[index].occupied && !table[index].deleted && table[index].key != key)
            {
                index = (index + 1) % capacity;
            }

            if (!table[index].occupied || table[index].deleted)
            {
                table[index].key = key;
                table[index].value = value;
                table[index].occupied = true;
                table[index].deleted = false;
                ++size;
            }
            else
            {
                table[index].value = value;
            }
        }

        bool remove(const Key& key)
        {
            size_t index = hash(key) % capacity;
            while (table[index].occupied)
            {
                if (!table[index].deleted && table[index].key == key)
                {
                    table[index].deleted = true;
                    --size;
                    return true;
                }
                index = (index + 1) % capacity;
            }
            return false;
        }

        bool get(const Key& key, Value& outValue) const
        {
            size_t index = hash(key) % capacity;
            while (table[index].occupied)
            {
                if (!table[index].deleted && table[index].key == key)
                {
                    outValue = table[index].value;
                    return true;
                }
                index = (index + 1) % capacity;
            }
            return false;
        }

        bool contains(const Key& key) const
        {
            size_t index = hash(key) % capacity;
            while (table[index].occupied)
            {
                if (!table[index].deleted && table[index].key == key)
                {
                    return true;
                }
                index = (index + 1) % capacity;
            }
            return false;
        }

        void clear()
        {
            delete[] table;
            table = new Entry[capacity];
            size = 0;
        }

        size_t getSize() const { return size; }
        size_t getCapacity() const { return capacity; }
    };
}

#endif // _HASH_TABLE_H_
