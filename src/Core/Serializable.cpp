#include <yaml-cpp/yaml.h>

#include <FileSystem/Serializable.hpp>
#include <algorithm>
#include <bit>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <utility>

namespace Sleak {

namespace {

constexpr int kMaxDepth = 256;
constexpr char kBinaryMagic[4] = {'S', 'L', 'K', 'B'};
constexpr uint32_t kBinaryVersion = 1;

[[noreturn]] void Fail(const std::string& message) {
    throw std::runtime_error(message);
}

void CheckDepth(int depth) {
    if (depth > kMaxDepth)
        Fail("Serialized data is nested deeper than " +
             std::to_string(kMaxDepth) + " levels");
}

const char* FormatName(SerializationFormat format) {
    switch (format) {
        case SerializationFormat::Binary:
            return "Binary";
        case SerializationFormat::JSON:
            return "JSON";
        case SerializationFormat::YAML:
            return "YAML";
        case SerializationFormat::XML:
            return "XML";
        default:
            return "Unknown";
    }
}

void RequireSupported(SerializationFormat format) {
    if (format == SerializationFormat::XML)
        Fail("XML serialization is not supported, use .json, .yaml or .bin");
    if (format != SerializationFormat::Binary &&
        format != SerializationFormat::JSON &&
        format != SerializationFormat::YAML)
        Fail("Unknown serialization format, use .json, .yaml or .bin");
}

// JSON

using Json = nlohmann::ordered_json;

Json ToJson(const SerialValue& v, int depth) {
    CheckDepth(depth);
    switch (v.GetType()) {
        case SerialValue::Type::Null:
            return nullptr;
        case SerialValue::Type::Bool:
            return v.AsBool();
        case SerialValue::Type::Int:
            return v.AsInt();
        case SerialValue::Type::Float:
            return v.AsFloat();
        case SerialValue::Type::String:
            return v.AsString();
        case SerialValue::Type::Array: {
            Json out = Json::array();
            for (size_t i = 0; i < v.Size(); ++i)
                out.push_back(ToJson(v.At(i), depth + 1));
            return out;
        }
        case SerialValue::Type::Object: {
            Json out = Json::object();
            for (size_t i = 0; i < v.Size(); ++i)
                out[v.KeyAt(i)] = ToJson(v.At(i), depth + 1);
            return out;
        }
    }
    return nullptr;
}

SerialValue FromJson(const Json& j, int depth) {
    CheckDepth(depth);
    switch (j.type()) {
        case Json::value_t::boolean:
            return SerialValue::MakeBool(j.get<bool>());
        case Json::value_t::number_integer:
            return SerialValue::MakeInt(j.get<int64_t>());
        case Json::value_t::number_unsigned: {
            uint64_t u = j.get<uint64_t>();
            if (u > static_cast<uint64_t>(INT64_MAX))
                return SerialValue::MakeFloat(static_cast<double>(u));
            return SerialValue::MakeInt(static_cast<int64_t>(u));
        }
        case Json::value_t::number_float:
            return SerialValue::MakeFloat(j.get<double>());
        case Json::value_t::string:
            return SerialValue::MakeString(j.get<std::string>());
        case Json::value_t::array: {
            SerialValue out = SerialValue::MakeArray();
            for (const auto& item : j) out.Append(FromJson(item, depth + 1));
            return out;
        }
        case Json::value_t::object: {
            SerialValue out = SerialValue::MakeObject();
            for (auto it = j.begin(); it != j.end(); ++it)
                out.Set(it.key(), FromJson(it.value(), depth + 1));
            return out;
        }
        default:
            return SerialValue();
    }
}

// YAML

std::string FormatFloat(double d) {
    if (std::isnan(d)) return ".nan";
    if (std::isinf(d)) return d > 0 ? ".inf" : "-.inf";
    char buf[64];
    auto res = std::to_chars(buf, buf + sizeof(buf), d);
    std::string s(buf, res.ptr);
    if (s.find_first_of(".eE") == std::string::npos) s += ".0";
    return s;
}

bool IsScalarArray(const SerialValue& v) {
    for (size_t i = 0; i < v.Size(); ++i) {
        auto t = v.At(i).GetType();
        if (t == SerialValue::Type::Array || t == SerialValue::Type::Object)
            return false;
    }
    return true;
}

void EmitYaml(YAML::Emitter& out, const SerialValue& v, int depth) {
    CheckDepth(depth);
    switch (v.GetType()) {
        case SerialValue::Type::Null:
            out << YAML::Null;
            break;
        case SerialValue::Type::Bool:
            out << v.AsBool();
            break;
        case SerialValue::Type::Int:
            out << static_cast<long long>(v.AsInt());
            break;
        case SerialValue::Type::Float:
            out << FormatFloat(v.AsFloat());
            break;
        case SerialValue::Type::String:
            out << YAML::DoubleQuoted << v.AsString();
            break;
        case SerialValue::Type::Array:
            if (IsScalarArray(v)) out << YAML::Flow;
            out << YAML::BeginSeq;
            for (size_t i = 0; i < v.Size(); ++i)
                EmitYaml(out, v.At(i), depth + 1);
            out << YAML::EndSeq;
            break;
        case SerialValue::Type::Object:
            if (v.Size() == 0) out << YAML::Flow;
            out << YAML::BeginMap;
            for (size_t i = 0; i < v.Size(); ++i) {
                out << YAML::Key << v.KeyAt(i) << YAML::Value;
                EmitYaml(out, v.At(i), depth + 1);
            }
            out << YAML::EndMap;
            break;
    }
}

SerialValue ParseYamlScalar(const YAML::Node& node) {
    const std::string& s = node.Scalar();
    if (node.Tag() == "!") return SerialValue::MakeString(s);

    if (s == "true" || s == "True" || s == "TRUE")
        return SerialValue::MakeBool(true);
    if (s == "false" || s == "False" || s == "FALSE")
        return SerialValue::MakeBool(false);
    if (s.empty() || s == "~" || s == "null" || s == "Null" || s == "NULL")
        return SerialValue();
    if (s == ".nan" || s == ".NaN" || s == ".NAN")
        return SerialValue::MakeFloat(std::numeric_limits<double>::quiet_NaN());
    if (s == ".inf" || s == ".Inf" || s == ".INF" || s == "+.inf")
        return SerialValue::MakeFloat(std::numeric_limits<double>::infinity());
    if (s == "-.inf" || s == "-.Inf" || s == "-.INF")
        return SerialValue::MakeFloat(-std::numeric_limits<double>::infinity());

    const char* begin = s.data();
    const char* end = s.data() + s.size();
    if (*begin == '+') ++begin;

    int64_t i = 0;
    auto ir = std::from_chars(begin, end, i);
    if (ir.ec == std::errc() && ir.ptr == end) return SerialValue::MakeInt(i);

    double d = 0.0;
    auto fr = std::from_chars(begin, end, d);
    if (fr.ec == std::errc() && fr.ptr == end) return SerialValue::MakeFloat(d);

    return SerialValue::MakeString(s);
}

SerialValue FromYaml(const YAML::Node& node, int depth) {
    CheckDepth(depth);
    switch (node.Type()) {
        case YAML::NodeType::Scalar:
            return ParseYamlScalar(node);
        case YAML::NodeType::Sequence: {
            SerialValue out = SerialValue::MakeArray();
            for (const auto& item : node) out.Append(FromYaml(item, depth + 1));
            return out;
        }
        case YAML::NodeType::Map: {
            SerialValue out = SerialValue::MakeObject();
            for (const auto& kv : node) {
                if (!kv.first.IsScalar()) Fail("YAML map keys must be scalars");
                out.Set(kv.first.Scalar(), FromYaml(kv.second, depth + 1));
            }
            return out;
        }
        default:
            return SerialValue();
    }
}

// Binary: magic, u32 version, then one tagged value. Integers are little
// endian, strings and containers carry a u32 length prefix.

void PutU32(std::string& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>(v >> (i * 8)));
}

void PutU64(std::string& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>(v >> (i * 8)));
}

void PutString(std::string& out, const std::string& s) {
    if (s.size() > UINT32_MAX) Fail("String too long for binary format");
    PutU32(out, static_cast<uint32_t>(s.size()));
    out.append(s);
}

void EncodeBinary(std::string& out, const SerialValue& v, int depth) {
    CheckDepth(depth);
    out.push_back(static_cast<char>(v.GetType()));
    switch (v.GetType()) {
        case SerialValue::Type::Null:
            break;
        case SerialValue::Type::Bool:
            out.push_back(v.AsBool() ? 1 : 0);
            break;
        case SerialValue::Type::Int:
            PutU64(out, static_cast<uint64_t>(v.AsInt()));
            break;
        case SerialValue::Type::Float:
            PutU64(out, std::bit_cast<uint64_t>(v.AsFloat()));
            break;
        case SerialValue::Type::String:
            PutString(out, v.AsString());
            break;
        case SerialValue::Type::Array:
            PutU32(out, static_cast<uint32_t>(v.Size()));
            for (size_t i = 0; i < v.Size(); ++i)
                EncodeBinary(out, v.At(i), depth + 1);
            break;
        case SerialValue::Type::Object:
            PutU32(out, static_cast<uint32_t>(v.Size()));
            for (size_t i = 0; i < v.Size(); ++i) {
                PutString(out, v.KeyAt(i));
                EncodeBinary(out, v.At(i), depth + 1);
            }
            break;
    }
}

class BinaryReader {
   public:
    explicit BinaryReader(const std::string& data) : m_data(data) {}

    void Need(size_t n) const {
        if (m_data.size() - m_pos < n)
            Fail("Binary data is truncated at offset " + std::to_string(m_pos));
    }

    uint8_t U8() {
        Need(1);
        return static_cast<uint8_t>(m_data[m_pos++]);
    }

    uint32_t U32() {
        Need(4);
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i)
            v |= static_cast<uint32_t>(static_cast<uint8_t>(m_data[m_pos++]))
                 << (i * 8);
        return v;
    }

    uint64_t U64() {
        Need(8);
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
            v |= static_cast<uint64_t>(static_cast<uint8_t>(m_data[m_pos++]))
                 << (i * 8);
        return v;
    }

    std::string String() {
        uint32_t len = U32();
        Need(len);
        std::string s = m_data.substr(m_pos, len);
        m_pos += len;
        return s;
    }

    SerialValue Value(int depth) {
        CheckDepth(depth);
        size_t at = m_pos;
        uint8_t tag = U8();
        switch (static_cast<SerialValue::Type>(tag)) {
            case SerialValue::Type::Null:
                return SerialValue();
            case SerialValue::Type::Bool:
                return SerialValue::MakeBool(U8() != 0);
            case SerialValue::Type::Int:
                return SerialValue::MakeInt(static_cast<int64_t>(U64()));
            case SerialValue::Type::Float:
                return SerialValue::MakeFloat(std::bit_cast<double>(U64()));
            case SerialValue::Type::String:
                return SerialValue::MakeString(String());
            case SerialValue::Type::Array: {
                uint32_t count = U32();
                Need(count);
                SerialValue out = SerialValue::MakeArray();
                for (uint32_t i = 0; i < count; ++i)
                    out.Append(Value(depth + 1));
                return out;
            }
            case SerialValue::Type::Object: {
                uint32_t count = U32();
                Need(static_cast<size_t>(count) * 5);
                SerialValue out = SerialValue::MakeObject();
                for (uint32_t i = 0; i < count; ++i) {
                    std::string key = String();
                    out.Set(key, Value(depth + 1));
                }
                return out;
            }
        }
        Fail("Unknown value tag " + std::to_string(tag) + " at offset " +
             std::to_string(at));
    }

    bool AtEnd() const { return m_pos == m_data.size(); }

   private:
    const std::string& m_data;
    size_t m_pos = 0;
};

std::string ReadAll(std::istream& stream) {
    std::ostringstream ss;
    ss << stream.rdbuf();
    return ss.str();
}

template <typename T>
bool TryAny(const std::any& value, SerialValue& out) {
    if (value.type() != typeid(T)) return false;
    out = SerialTraits<T>::ToValue(std::any_cast<const T&>(value));
    return true;
}

template <typename... Ts>
bool AnyToValue(const std::any& value, SerialValue& out) {
    return (TryAny<Ts>(value, out) || ...);
}

}  // namespace

// SerialValue

SerialValue SerialValue::MakeBool(bool value) {
    SerialValue v;
    v.m_type = Type::Bool;
    v.m_bool = value;
    return v;
}

SerialValue SerialValue::MakeInt(int64_t value) {
    SerialValue v;
    v.m_type = Type::Int;
    v.m_int = value;
    return v;
}

SerialValue SerialValue::MakeFloat(double value) {
    SerialValue v;
    v.m_type = Type::Float;
    v.m_float = value;
    return v;
}

SerialValue SerialValue::MakeString(std::string value) {
    SerialValue v;
    v.m_type = Type::String;
    v.m_string = std::move(value);
    return v;
}

SerialValue SerialValue::MakeArray() {
    SerialValue v;
    v.m_type = Type::Array;
    return v;
}

SerialValue SerialValue::MakeObject() {
    SerialValue v;
    v.m_type = Type::Object;
    return v;
}

SerialValue& SerialValue::Append(SerialValue value) {
    if (m_type == Type::Null) m_type = Type::Array;
    if (m_type != Type::Array)
        Fail(std::string("Append on a ") + TypeName(m_type) + " value");
    m_items.push_back(std::move(value));
    return m_items.back();
}

SerialValue& SerialValue::Set(const std::string& key, SerialValue value) {
    if (m_type == Type::Null) m_type = Type::Object;
    if (m_type != Type::Object)
        Fail("Set('" + key + "') on a " + TypeName(m_type) + " value");
    if (SerialValue* existing = Find(key)) {
        *existing = std::move(value);
        return *existing;
    }
    m_keys.push_back(key);
    m_items.push_back(std::move(value));
    return m_items.back();
}

const SerialValue* SerialValue::Find(const std::string& key) const {
    if (m_type != Type::Object) return nullptr;
    for (size_t i = 0; i < m_keys.size(); ++i)
        if (m_keys[i] == key) return &m_items[i];
    return nullptr;
}

SerialValue* SerialValue::Find(const std::string& key) {
    return const_cast<SerialValue*>(std::as_const(*this).Find(key));
}

bool SerialValue::operator==(const SerialValue& other) const {
    if (m_type != other.m_type) return false;
    switch (m_type) {
        case Type::Null:
            return true;
        case Type::Bool:
            return m_bool == other.m_bool;
        case Type::Int:
            return m_int == other.m_int;
        case Type::Float:
            return m_float == other.m_float ||
                   (std::isnan(m_float) && std::isnan(other.m_float));
        case Type::String:
            return m_string == other.m_string;
        case Type::Array:
            return m_items == other.m_items;
        case Type::Object:
            return m_keys == other.m_keys && m_items == other.m_items;
    }
    return false;
}

const char* SerialValue::TypeName(Type type) {
    switch (type) {
        case Type::Null:
            return "null";
        case Type::Bool:
            return "bool";
        case Type::Int:
            return "int";
        case Type::Float:
            return "float";
        case Type::String:
            return "string";
        case Type::Array:
            return "array";
        case Type::Object:
            return "object";
    }
    return "unknown";
}

// ISerializationContext

void ISerializationContext::WriteValue(const std::string& key, SerialValue) {
    Fail("This serialization context does not support WriteValue ('" + key +
         "')");
}

const SerialValue* ISerializationContext::FindValue(const std::string&) const {
    return nullptr;
}

void ISerializationContext::WriteObject(const std::string& key,
                                        const Serializable& object) {
    SerializationContext nested;
    object.Serialize(nested);
    WriteValue(key, std::move(nested.GetRoot()));
}

bool ISerializationContext::ReadObject(const std::string& key,
                                       Serializable& object) const {
    const SerialValue* value = FindValue(key);
    if (!value || !value->IsObject()) return false;
    SerializationContext nested(SerializationFormat::JSON, *value);
    object.Deserialize(nested);
    return true;
}

// SerializationContext

SerializationContext::SerializationContext(SerializationFormat format)
    : m_format(format), m_root(SerialValue::MakeObject()) {
    RequireSupported(format);
}

SerializationContext::SerializationContext(SerializationFormat format,
                                           SerialValue root)
    : m_format(format), m_root(std::move(root)) {
    RequireSupported(format);
    if (!m_root.IsObject())
        Fail("A serialization context root must be an object");
}

void SerializationContext::Write(const std::string& key,
                                 const std::any& value) {
    SerialValue converted;
    bool ok =
        AnyToValue<SerialValue, bool, int, unsigned int, long, unsigned long,
                   long long, unsigned long long, short, unsigned short,
                   signed char, unsigned char, float, double, std::string,
                   const char*, char*, Math::Vector2D, Math::Vector3D,
                   Math::Vector4D, Math::Quaternion, Math::Color,
                   std::vector<bool>, std::vector<int>, std::vector<float>,
                   std::vector<double>, std::vector<std::string>,
                   std::vector<Math::Vector3D>>(value, converted);
    if (!ok)
        Fail("Cannot serialize key '" + key + "': unsupported type " +
             value.type().name());
    WriteValue(key, std::move(converted));
}

std::any SerializationContext::Read(const std::string& key) const {
    const SerialValue* v = FindValue(key);
    if (!v) Fail("Key not found: " + key);
    switch (v->GetType()) {
        case SerialValue::Type::Null:
            return {};
        case SerialValue::Type::Bool:
            return v->AsBool();
        case SerialValue::Type::Int:
            if (v->AsInt() >= std::numeric_limits<int>::min() &&
                v->AsInt() <= std::numeric_limits<int>::max())
                return static_cast<int>(v->AsInt());
            return v->AsInt();
        case SerialValue::Type::Float:
            return v->AsFloat();
        case SerialValue::Type::String:
            return v->AsString();
        default:
            return *v;
    }
}

void SerializationContext::WriteValue(const std::string& key,
                                      SerialValue value) {
    m_root.Set(key, std::move(value));
}

const SerialValue* SerializationContext::FindValue(
    const std::string& key) const {
    return m_root.Find(key);
}

void SerializationContext::WriteToStream(std::ostream& stream) const {
    switch (m_format) {
        case SerializationFormat::JSON:
            stream << ToJson(m_root, 0).dump(4) << '\n';
            break;
        case SerializationFormat::YAML: {
            YAML::Emitter out;
            EmitYaml(out, m_root, 0);
            if (!out.good())
                Fail(std::string("YAML emit failed: ") + out.GetLastError());
            stream << out.c_str() << '\n';
            break;
        }
        case SerializationFormat::Binary: {
            std::string data(kBinaryMagic, sizeof(kBinaryMagic));
            PutU32(data, kBinaryVersion);
            EncodeBinary(data, m_root, 0);
            stream.write(data.data(),
                         static_cast<std::streamsize>(data.size()));
            break;
        }
        default:
            RequireSupported(m_format);
    }
    if (!stream) Fail("Failed to write serialized data to stream");
}

void SerializationContext::ReadFromStream(std::istream& stream) {
    SerialValue root;
    switch (m_format) {
        case SerializationFormat::JSON: {
            Json j = Json::parse(stream, nullptr, false);
            if (j.is_discarded()) Fail("Malformed JSON");
            root = FromJson(j, 0);
            break;
        }
        case SerializationFormat::YAML: {
            YAML::Node node;
            try {
                node = YAML::Load(stream);
            } catch (const YAML::Exception& e) {
                Fail(std::string("Malformed YAML: ") + e.what());
            }
            root = FromYaml(node, 0);
            break;
        }
        case SerializationFormat::Binary: {
            std::string data = ReadAll(stream);
            if (data.size() < 8 ||
                std::memcmp(data.data(), kBinaryMagic, 4) != 0)
                Fail("Not a Sleak binary serialization file");
            BinaryReader reader(data);
            for (int i = 0; i < 4; ++i) reader.U8();
            uint32_t version = reader.U32();
            if (version != kBinaryVersion)
                Fail("Unsupported binary serialization version " +
                     std::to_string(version));
            root = reader.Value(0);
            if (!reader.AtEnd()) Fail("Trailing bytes after binary data");
            break;
        }
        default:
            RequireSupported(m_format);
    }
    if (!root.IsObject())
        Fail(std::string("Expected an object at the root of the ") +
             FormatName(m_format) + " data, got " +
             SerialValue::TypeName(root.GetType()));
    m_root = std::move(root);
}

void SerializationContext::SaveToFile(const SerialValue& root,
                                      const std::string& path) {
    SerializationContext context(SerializationFactory::DetectFormat(path),
                                 root);
    std::ostringstream buffer(std::ios::binary);
    context.WriteToStream(buffer);
    const std::string data = buffer.str();

    const std::string tmpPath = path + ".tmp";
    {
        std::ofstream file(tmpPath, std::ios::binary | std::ios::trunc);
        if (!file.is_open())
            Fail("Failed to open file for writing: " + tmpPath);
        file.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!file) Fail("Failed to write file: " + tmpPath);
    }
    std::error_code ec;
    std::filesystem::rename(tmpPath, path, ec);
    if (ec) {
        std::error_code ignored;
        std::filesystem::remove(tmpPath, ignored);
        Fail("Failed to replace " + path + ": " + ec.message());
    }
}

SerialValue SerializationContext::LoadFromFile(const std::string& path) {
    SerializationContext context(SerializationFactory::DetectFormat(path));
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) Fail("Failed to open file for reading: " + path);
    try {
        context.ReadFromStream(file);
    } catch (const std::exception& e) {
        Fail(path + ": " + e.what());
    }
    return std::move(context.GetRoot());
}

// Serializable

void Serializable::SerializeToFile(const std::string& filePath) const {
    SerializationContext context(SerializationFactory::DetectFormat(filePath));
    Serialize(context);
    SerializationContext::SaveToFile(context.GetRoot(), filePath);
}

void Serializable::DeserializeFromFile(const std::string& filePath) {
    SerializationContext context(SerializationFactory::DetectFormat(filePath),
                                 SerializationContext::LoadFromFile(filePath));
    Deserialize(context);
}

// SerializationFactory

SerializationFormat SerializationFactory::DetectFormat(
    const std::string& filePath) {
    std::string ext = std::filesystem::path(filePath).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (ext == ".bin") return SerializationFormat::Binary;
    if (ext == ".json") return SerializationFormat::JSON;
    if (ext == ".yaml" || ext == ".yml") return SerializationFormat::YAML;
    if (ext == ".xml") return SerializationFormat::XML;
    return SerializationFormat::Unknown;
}

std::unique_ptr<ISerializationContext> SerializationFactory::CreateContext(
    SerializationFormat format) {
    return std::make_unique<SerializationContext>(format);
}

}  // namespace Sleak
