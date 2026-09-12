#ifndef _SERIALIZABLE_H_
#define _SERIALIZABLE_H_

#include <iostream>
#include <string>
#include <memory>
#include <unordered_map>
#include <any>
#include <stdexcept>
#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>  

namespace Sleak {

    // Forward declarations
    class ISerializationContext;

    /// File format a Serializable can be saved to or loaded from.
    /// @ingroup filesystem
    enum class SerializationFormat {
        Binary,
        JSON,
        YAML,
        XML,
        Unknown
    };

    /**
     * @interface ISerializationContext
     * @brief Interface for serialization contexts.
     * @ingroup filesystem
     */
    class ISerializationContext {
    public:
        virtual ~ISerializationContext() = default;

        // Serialization methods
        virtual void Write(const std::string& key, const std::any& value) = 0;
        virtual std::any Read(const std::string& key) const = 0;

        // Stream-based methods
        virtual void WriteToStream(std::ostream& stream) const = 0;
        virtual void ReadFromStream(std::istream& stream) = 0;
    };

    /**
     * @class Serializable
     * @brief Base class for objects that can be serialized and deserialized.
     * @ingroup filesystem
     */
    class Serializable {
    public:
        virtual ~Serializable() = default;

        /**
         * @brief Serializes the object using the specified context.
         * @param context The serialization context.
         */
        virtual void Serialize(ISerializationContext& context) const = 0;

        /**
         * @brief Deserializes the object using the specified context.
         * @param context The deserialization context.
         */
        virtual void Deserialize(const ISerializationContext& context) = 0;

        /**
         * @brief Serializes the object to a file.
         * @param filePath The path to the file where the object will be saved.
         */
        void SerializeToFile(const std::string& filePath) const;

        /**
         * @brief Deserializes the object from a file.
         * @param filePath The path to the file from which the object will be loaded.
         */
        void DeserializeFromFile(const std::string& filePath);
    };

    /**
     * @class SerializationFactory
     * @brief Detects file format and creates the appropriate serialization context.
     * @ingroup filesystem
     */
    class SerializationFactory {
    public:
        /// Picks a format from filePath's extension (.bin/.json/.yaml/.yml).
        static SerializationFormat DetectFormat(const std::string& filePath);
        /// Builds the context implementation matching format.
        static std::unique_ptr<ISerializationContext> CreateContext(SerializationFormat format);
    };

} // namespace Sleak

#endif // _SERIALIZABLE_H_