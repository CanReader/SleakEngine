#include <FileSystem/Serializable.hpp>
#include <Utility/Exception.hpp>
#include <fstream>
#include <sstream>

namespace YAML {
    template <>
    struct convert<std::any> {
        static Node encode(const std::any& rhs) {
            Node node;
            if (rhs.type() == typeid(int)) {
                node = std::any_cast<int>(rhs);
            } else if (rhs.type() == typeid(float)) {
                node = std::any_cast<float>(rhs);
            } else if (rhs.type() == typeid(std::string)) {
                node = std::any_cast<std::string>(rhs);
            } else {
                throw std::runtime_error("Unsupported type for YAML serialization");
            }
            return node;
        }

        static bool decode(const Node& node, std::any& rhs) {
            try {
                if (node.IsScalar()) {
                    std::string value = node.as<std::string>();
                    rhs = value;  // Store as std::any(std::string)
                    return true;
                }
            } catch (...) {
                return false;
            }
            return false;
        }
    };
}

namespace Sleak {

    // Binary serialization context
    class BinarySerializationContext : public ISerializationContext {
    public:
        std::unordered_map<std::string, std::any> Data;

        void Write(const std::string& key, const std::any& value) override {
            Data[key] = value;
        }

        std::any Read(const std::string& key) const override {
            auto it = Data.find(key);
            if (it == Data.end()) {
                throw std::runtime_error("Key not found: " + key);
            }
            return it->second;
        }

        void WriteToStream(std::ostream& stream) const override {
            for (const auto& [key, value] : Data) {
                // (Implementation depends on the data types)
            }
            throw Sleak::NotImplementedException("This method is not completed yet");
        }

        void ReadFromStream(std::istream& stream) override {
            // (Implementation depends on the data types)
            throw Sleak::NotImplementedException("This method is not completed yet");
        }
    };

    // JSON serialization context
    class JsonSerializationContext : public ISerializationContext {
    public:
        nlohmann::json JsonData;

        void Write(const std::string& key, const std::any& value) override {
            if (value.type() == typeid(int)) {
                JsonData[key] = std::any_cast<int>(value);
            } else if (value.type() == typeid(float)) {
                JsonData[key] = std::any_cast<float>(value);
            } else if (value.type() == typeid(std::string)) {
                JsonData[key] = std::any_cast<std::string>(value);
            }
            // Add more type conversions as needed
        }

        std::any Read(const std::string& key) const override {
            if (!JsonData.contains(key)) {
                throw std::runtime_error("Key not found: " + key);
            }
            return JsonData[key];
        }

        void WriteToStream(std::ostream& stream) const override {
            stream << JsonData.dump(4); // Pretty-print JSON
        }

        void ReadFromStream(std::istream& stream) override {
            stream >> JsonData;
        }
    };

    // YAML serialization context
    class YamlSerializationContext : public ISerializationContext {
    public:
        YAML::Node YamlData;

        void Write(const std::string& key, const std::any& value) override {
            if (value.type() == typeid(int)) {
                YamlData[key] = std::any_cast<int>(value);
            } else if (value.type() == typeid(float)) {
                YamlData[key] = std::any_cast<float>(value);
            } else if (value.type() == typeid(std::string)) {
                YamlData[key] = std::any_cast<std::string>(value);
            }
            // Add more type conversions as needed
        }

        std::any Read(const std::string& key) const override {
            if (!YamlData[key]) {
                throw std::runtime_error("Key not found: " + key);
            }
            return YamlData[key].as<std::any>();
        }

        void WriteToStream(std::ostream& stream) const override {
            stream << YamlData;
        }

        void ReadFromStream(std::istream& stream) override {
            YamlData = YAML::Load(stream);
        }
    };

    // Serializable methods
    void Serializable::SerializeToFile(const std::string& filePath) const {
        auto format = SerializationFactory::DetectFormat(filePath);
        auto context = SerializationFactory::CreateContext(format);

        std::ofstream file(filePath);
        if (!file.is_open()) {
            throw std::runtime_error("Failed to open file for writing: " + filePath);
        }

        Serialize(std::move(context));
        context->WriteToStream(file);
    }

    void Serializable::DeserializeFromFile(const std::string& filePath) {
        auto format = SerializationFactory::DetectFormat(filePath);
        auto context = SerializationFactory::CreateContext(format);

        std::ifstream file(filePath);
        if (!file.is_open()) {
            throw std::runtime_error("Failed to open file for reading: " + filePath);
        }

        context->ReadFromStream(file);
        Deserialize(std::move(context));
    }

    // SerializationFactory methods
    SerializationFormat SerializationFactory::DetectFormat(const std::string& filePath) {
        if (filePath.ends_with(".bin")) {
            return SerializationFormat::Binary;
        } else if (filePath.ends_with(".json")) {
            return SerializationFormat::JSON;
        } else if (filePath.ends_with(".yaml") || filePath.ends_with(".yml")) {
            return SerializationFormat::YAML;
        }
        return SerializationFormat::Unknown;
    }

    std::unique_ptr<ISerializationContext> SerializationFactory::CreateContext(SerializationFormat format) {
        switch (format) {
            case SerializationFormat::Binary:
                return std::make_unique<BinarySerializationContext>();
            case SerializationFormat::JSON:
                return std::make_unique<JsonSerializationContext>();
            case SerializationFormat::YAML:
                return std::make_unique<YamlSerializationContext>();
            default:
                throw std::runtime_error("Unsupported serialization format.");
        }
    }

} // namespace Sleak