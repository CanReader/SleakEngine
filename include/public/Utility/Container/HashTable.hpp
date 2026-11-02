#ifndef _HASH_TABLE_H_
#define _HASH_TABLE_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>

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
 * - Handles collisions with linear-probing open addressing and tombstones.
 * - Provides methods for inserting, deleting, and retrieving key-value pairs.
 * @ingroup utility
 */
    template <typename Key, typename Value>
    class HashTable
    {
    private:
        /// One slot: key/value plus occupied and tombstone-deleted flags.
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
        size_t tombstones;
        float loadFactor;

        static constexpr size_t DEFAULT_CAPACITY = 16;
        static constexpr float DEFAULT_LOAD_FACTOR = 0.7f;
        static constexpr float MAX_LOAD_FACTOR = 1.0f;
        static constexpr size_t MIN_CAPACITY = 1;
        static constexpr size_t GROWTH_FACTOR = 2;

        /// Home slot of key in a table with tableCapacity slots.
        static size_t slotFor(const Key& key, size_t tableCapacity)
        {
            return std::hash<Key>{}(key) % tableCapacity;
        }

        /// Home slot of key in the current table.
        size_t hash(const Key& key) const
        {
            return slotFor(key, capacity);
        }

        /// Slot holding a live entry for key, or capacity if there is none.
        size_t findSlot(const Key& key) const
        {
            size_t index = hash(key);
            for (size_t probes = 0; probes < capacity && table[index].occupied;
                 ++probes)
            {
                if (!table[index].deleted && table[index].key == key)
                {
                    return index;
                }
                index = (index + 1) % capacity;
            }
            return capacity;
        }

        /// Moves live entries into a newCapacity table, dropping tombstones.
        void rehash(size_t newCapacity)
        {
            Entry* newTable = new Entry[newCapacity];

            for (size_t i = 0; i < capacity; ++i)
            {
                if (table[i].occupied && !table[i].deleted)
                {
                    size_t index = slotFor(table[i].key, newCapacity);
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
            tombstones = 0;
        }

        /// Grows if live entries fill the table, else purges tombstones only.
        void resize()
        {
            const bool mostlyLive =
                size * GROWTH_FACTOR >= capacity * loadFactor;
            rehash(mostlyLive ? capacity * GROWTH_FACTOR : capacity);
        }

    public:
        HashTable(size_t initCapacity = DEFAULT_CAPACITY, float loadFactor = DEFAULT_LOAD_FACTOR)
            : capacity(initCapacity > MIN_CAPACITY ? initCapacity
                                                   : MIN_CAPACITY),
              size(0),
              tombstones(0),
              loadFactor(loadFactor > 0.0f && loadFactor <= MAX_LOAD_FACTOR
                             ? loadFactor
                             : DEFAULT_LOAD_FACTOR)
        {
            table = new Entry[capacity];
        }

        /// Deep-copies other's slots, including its capacity and load factor.
        HashTable(const HashTable& other)
            : table(new Entry[other.capacity]),
              capacity(other.capacity),
              size(other.size),
              tombstones(other.tombstones),
              loadFactor(other.loadFactor)
        {
            for (size_t i = 0; i < capacity; ++i)
            {
                table[i] = other.table[i];
            }
        }

        /// Replaces this table with a deep copy of other.
        HashTable& operator=(const HashTable& other)
        {
            if (this != &other)
            {
                HashTable copy(other);
                std::swap(table, copy.table);
                std::swap(capacity, copy.capacity);
                std::swap(size, copy.size);
                std::swap(tombstones, copy.tombstones);
                std::swap(loadFactor, copy.loadFactor);
            }
            return *this;
        }

        ~HashTable()
        {
            delete[] table;
        }

        /// Inserts or updates the value for key, resizing first if over the load factor.
        void insert(const Key& key, const Value& value)
        {
            const size_t existing = findSlot(key);
            if (existing != capacity)
            {
                table[existing].value = value;
                return;
            }

            if (size + tombstones >= capacity * loadFactor)
            {
                resize();
            }

            size_t index = hash(key);
            while (table[index].occupied && !table[index].deleted)
            {
                index = (index + 1) % capacity;
            }

            if (table[index].deleted)
            {
                --tombstones;
            }
            table[index].key = key;
            table[index].value = value;
            table[index].occupied = true;
            table[index].deleted = false;
            ++size;
        }

        /// Tombstones the entry for key; returns false if it wasn't present.
        bool remove(const Key& key)
        {
            const size_t index = findSlot(key);
            if (index == capacity)
            {
                return false;
            }
            table[index].key = Key();
            table[index].value = Value();
            table[index].deleted = true;
            --size;
            ++tombstones;
            return true;
        }

        /// Looks up key and writes its value into outValue; returns false if not found.
        bool get(const Key& key, Value& outValue) const
        {
            const size_t index = findSlot(key);
            if (index == capacity)
            {
                return false;
            }
            outValue = table[index].value;
            return true;
        }

        /// True if key currently has a live entry.
        bool contains(const Key& key) const
        {
            return findSlot(key) != capacity;
        }

        /// Drops every entry and reallocates the table at its current capacity.
        void clear()
        {
            Entry* emptyTable = new Entry[capacity];
            delete[] table;
            table = emptyTable;
            size = 0;
            tombstones = 0;
        }

        size_t getSize() const { return size; }
        size_t getCapacity() const { return capacity; }
    };
}

#endif // _HASH_TABLE_H_
