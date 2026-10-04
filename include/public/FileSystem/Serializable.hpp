#ifndef _SERIALIZABLE_H_
#define _SERIALIZABLE_H_

#include <Core/OSDef.hpp>
#include <Math/Color.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector.hpp>
#include <any>
#include <array>
#include <charconv>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace Sleak {

class ISerializationContext;
class Serializable;

/// File format a Serializable can be saved to or loaded from.
/// XML is reserved and not supported, CreateContext throws for it.
/// @ingroup filesystem
enum class SerializationFormat { Binary, JSON, YAML, XML, Unknown };

/// Format-neutral value tree that every serialization context reads and
/// writes. Objects keep insertion order so output is deterministic.
/// @ingroup filesystem
class ENGINE_API SerialValue {
   public:
    /// Kind of value held.
    enum class Type : uint8_t { Null, Bool, Int, Float, String, Array, Object };

    SerialValue() = default;

    /// Builds a bool value.
    static SerialValue MakeBool(bool value);
    /// Builds a signed integer value.
    static SerialValue MakeInt(int64_t value);
    /// Builds a floating point value.
    static SerialValue MakeFloat(double value);
    /// Builds a string value.
    static SerialValue MakeString(std::string value);
    /// Builds an empty array.
    static SerialValue MakeArray();
    /// Builds an empty object.
    static SerialValue MakeObject();

    Type GetType() const { return m_type; }
    bool IsNull() const { return m_type == Type::Null; }
    bool IsBool() const { return m_type == Type::Bool; }
    bool IsInt() const { return m_type == Type::Int; }
    bool IsFloat() const { return m_type == Type::Float; }
    /// True for both Int and Float.
    bool IsNumber() const { return IsInt() || IsFloat(); }
    bool IsString() const { return m_type == Type::String; }
    bool IsArray() const { return m_type == Type::Array; }
    bool IsObject() const { return m_type == Type::Object; }

    bool AsBool() const { return m_bool; }
    int64_t AsInt() const { return m_int; }
    /// Numeric value as double, also valid for Int.
    double AsFloat() const {
        return m_type == Type::Int ? static_cast<double>(m_int) : m_float;
    }
    const std::string& AsString() const { return m_string; }

    /// Element or member count for arrays and objects, 0 otherwise.
    size_t Size() const { return m_items.size(); }
    /// Element (array) or member value (object) at index.
    const SerialValue& At(size_t index) const { return m_items.at(index); }
    SerialValue& At(size_t index) { return m_items.at(index); }
    /// Member name at index, objects only.
    const std::string& KeyAt(size_t index) const { return m_keys.at(index); }

    /// Appends to an array, turning a null value into an empty array first.
    SerialValue& Append(SerialValue value);
    /// Sets an object member, replacing an existing one with the same key.
    SerialValue& Set(const std::string& key, SerialValue value);
    /// Member lookup, nullptr when missing or when this is not an object.
    const SerialValue* Find(const std::string& key) const;
    SerialValue* Find(const std::string& key);

    bool operator==(const SerialValue& other) const;
    bool operator!=(const SerialValue& other) const {
        return !(*this == other);
    }

    /// Human readable type name for error messages.
    static const char* TypeName(Type type);

   private:
    Type m_type = Type::Null;
    bool m_bool = false;
    int64_t m_int = 0;
    double m_float = 0.0;
    std::string m_string;
    std::vector<SerialValue> m_items;
    std::vector<std::string> m_keys;
};

/// Converts T to and from a SerialValue. Specialize it to make a custom type
/// work with ISerializationContext::Set/Get.
/// @ingroup filesystem
template <typename T, typename Enable = void>
struct SerialTraits;

template <>
struct SerialTraits<SerialValue> {
    static SerialValue ToValue(const SerialValue& v) { return v; }
    static bool FromValue(const SerialValue& v, SerialValue& out) {
        out = v;
        return true;
    }
};

template <>
struct SerialTraits<bool> {
    static SerialValue ToValue(bool v) { return SerialValue::MakeBool(v); }
    static bool FromValue(const SerialValue& v, bool& out) {
        if (!v.IsBool()) return false;
        out = v.AsBool();
        return true;
    }
};

template <typename T>
struct SerialTraits<
    T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>>> {
    static SerialValue ToValue(T v) {
        return SerialValue::MakeInt(static_cast<int64_t>(v));
    }
    static bool FromValue(const SerialValue& v, T& out) {
        int64_t raw = 0;
        if (v.IsInt()) {
            raw = v.AsInt();
        } else if (v.IsFloat()) {
            double d = v.AsFloat();
            if (!(d >= -9.2e18 && d <= 9.2e18) ||
                d != static_cast<double>(static_cast<int64_t>(d)))
                return false;
            raw = static_cast<int64_t>(d);
        } else {
            return false;
        }
        if constexpr (std::is_signed_v<T>) {
            if (raw < static_cast<int64_t>(std::numeric_limits<T>::min()) ||
                raw > static_cast<int64_t>(std::numeric_limits<T>::max()))
                return false;
        } else {
            if (raw < 0) return false;
            if constexpr (sizeof(T) < sizeof(int64_t)) {
                if (raw > static_cast<int64_t>(std::numeric_limits<T>::max()))
                    return false;
            }
        }
        out = static_cast<T>(raw);
        return true;
    }
};

namespace Detail {
/// Widens a float through its shortest decimal form, so 0.1f is stored as 0.1
/// and still reads back as the same float.
inline double WidenFloat(float f) {
    char buf[32];
    auto res = std::to_chars(buf, buf + sizeof(buf), f);
    double d = 0.0;
    if (res.ec != std::errc() ||
        std::from_chars(buf, res.ptr, d).ec != std::errc())
        return static_cast<double>(f);
    return d;
}
}  // namespace Detail

template <typename T>
struct SerialTraits<T, std::enable_if_t<std::is_floating_point_v<T>>> {
    static SerialValue ToValue(T v) {
        if constexpr (std::is_same_v<T, float>)
            return SerialValue::MakeFloat(Detail::WidenFloat(v));
        else
            return SerialValue::MakeFloat(static_cast<double>(v));
    }
    static bool FromValue(const SerialValue& v, T& out) {
        if (!v.IsNumber()) return false;
        out = static_cast<T>(v.AsFloat());
        return true;
    }
};

template <typename T>
struct SerialTraits<T, std::enable_if_t<std::is_enum_v<T>>> {
    using U = std::underlying_type_t<T>;
    static SerialValue ToValue(T v) {
        return SerialTraits<U>::ToValue(static_cast<U>(v));
    }
    static bool FromValue(const SerialValue& v, T& out) {
        U raw{};
        if (!SerialTraits<U>::FromValue(v, raw)) return false;
        out = static_cast<T>(raw);
        return true;
    }
};

template <>
struct SerialTraits<std::string> {
    static SerialValue ToValue(const std::string& v) {
        return SerialValue::MakeString(v);
    }
    static bool FromValue(const SerialValue& v, std::string& out) {
        if (!v.IsString()) return false;
        out = v.AsString();
        return true;
    }
};

template <>
struct SerialTraits<const char*> {
    static SerialValue ToValue(const char* v) {
        return SerialValue::MakeString(v ? v : "");
    }
};

template <>
struct SerialTraits<char*> : SerialTraits<const char*> {};

namespace Detail {
template <size_t N>
inline SerialValue FloatsToValue(const std::array<float, N>& v) {
    SerialValue out = SerialValue::MakeArray();
    for (float f : v) out.Append(SerialValue::MakeFloat(WidenFloat(f)));
    return out;
}

template <size_t N>
inline bool ValueToFloats(const SerialValue& v, std::array<float, N>& out) {
    if (!v.IsArray() || v.Size() != N) return false;
    for (size_t i = 0; i < N; ++i) {
        if (!v.At(i).IsNumber()) return false;
        out[i] = static_cast<float>(v.At(i).AsFloat());
    }
    return true;
}
}  // namespace Detail

template <>
struct SerialTraits<Math::Vector2D> {
    static SerialValue ToValue(const Math::Vector2D& v) {
        return Detail::FloatsToValue<2>({v.GetX(), v.GetY()});
    }
    static bool FromValue(const SerialValue& v, Math::Vector2D& out) {
        std::array<float, 2> f{};
        if (!Detail::ValueToFloats(v, f)) return false;
        out = Math::Vector2D(f[0], f[1]);
        return true;
    }
};

template <>
struct SerialTraits<Math::Vector3D> {
    static SerialValue ToValue(const Math::Vector3D& v) {
        return Detail::FloatsToValue<3>({v.GetX(), v.GetY(), v.GetZ()});
    }
    static bool FromValue(const SerialValue& v, Math::Vector3D& out) {
        std::array<float, 3> f{};
        if (!Detail::ValueToFloats(v, f)) return false;
        out = Math::Vector3D(f[0], f[1], f[2]);
        return true;
    }
};

template <>
struct SerialTraits<Math::Vector4D> {
    static SerialValue ToValue(const Math::Vector4D& v) {
        return Detail::FloatsToValue<4>(
            {v.GetX(), v.GetY(), v.GetZ(), v.GetW()});
    }
    static bool FromValue(const SerialValue& v, Math::Vector4D& out) {
        std::array<float, 4> f{};
        if (!Detail::ValueToFloats(v, f)) return false;
        out = Math::Vector4D(f[0], f[1], f[2], f[3]);
        return true;
    }
};

/// Stored as [x, y, z, w].
template <>
struct SerialTraits<Math::Quaternion> {
    static SerialValue ToValue(const Math::Quaternion& q) {
        return Detail::FloatsToValue<4>({q.x, q.y, q.z, q.w});
    }
    static bool FromValue(const SerialValue& v, Math::Quaternion& out) {
        std::array<float, 4> f{};
        if (!Detail::ValueToFloats(v, f)) return false;
        out = Math::Quaternion(f[3], f[0], f[1], f[2]);
        return true;
    }
};

/// Stored as [r, g, b, a] integers in 0..255.
template <>
struct SerialTraits<Math::Color> {
    static SerialValue ToValue(const Math::Color& c) {
        SerialValue out = SerialValue::MakeArray();
        out.Append(SerialValue::MakeInt(c.GetR()));
        out.Append(SerialValue::MakeInt(c.GetG()));
        out.Append(SerialValue::MakeInt(c.GetB()));
        out.Append(SerialValue::MakeInt(c.GetA()));
        return out;
    }
    static bool FromValue(const SerialValue& v, Math::Color& out) {
        if (!v.IsArray() || v.Size() != 4) return false;
        std::array<uint8_t, 4> ch{};
        for (size_t i = 0; i < 4; ++i) {
            if (!SerialTraits<uint8_t>::FromValue(v.At(i), ch[i])) return false;
        }
        out = Math::Color(ch[0], ch[1], ch[2], ch[3]);
        return true;
    }
};

template <typename T>
struct SerialTraits<std::vector<T>> {
    static SerialValue ToValue(const std::vector<T>& v) {
        SerialValue out = SerialValue::MakeArray();
        for (const auto& item : v)
            out.Append(SerialTraits<T>::ToValue(static_cast<T>(item)));
        return out;
    }
    static bool FromValue(const SerialValue& v, std::vector<T>& out) {
        if (!v.IsArray()) return false;
        std::vector<T> result;
        result.reserve(v.Size());
        for (size_t i = 0; i < v.Size(); ++i) {
            T item{};
            if (!SerialTraits<T>::FromValue(v.At(i), item)) return false;
            result.push_back(std::move(item));
        }
        out = std::move(result);
        return true;
    }
};

/**
 * @interface ISerializationContext
 * @brief Keyed store a Serializable writes into and reads back from.
 * @ingroup filesystem
 *
 * Set/Get are the typed API and behave the same in every format. Read returns
 * bool, int (int64_t past int range), double, std::string, or a SerialValue
 * for arrays and objects.
 */
class ENGINE_API ISerializationContext {
   public:
    virtual ~ISerializationContext() = default;

    /// Stores a std::any holding any type SerialTraits knows about.
    virtual void Write(const std::string& key, const std::any& value) = 0;
    /// Reads a key back as a std::any. Throws if the key is missing.
    virtual std::any Read(const std::string& key) const = 0;

    virtual void WriteToStream(std::ostream& stream) const = 0;
    virtual void ReadFromStream(std::istream& stream) = 0;

    /// Stores a raw value under key.
    virtual void WriteValue(const std::string& key, SerialValue value);
    /// Raw value for key, nullptr when missing.
    virtual const SerialValue* FindValue(const std::string& key) const;

    /// True when key exists.
    bool Has(const std::string& key) const { return FindValue(key) != nullptr; }

    /// Typed write.
    template <typename T>
    void Set(const std::string& key, const T& value) {
        WriteValue(key, SerialTraits<std::decay_t<T>>::ToValue(value));
    }

    /// Typed read, false when the key is missing or the stored value does not
    /// convert to T. out is left untouched on failure.
    template <typename T>
    bool TryGet(const std::string& key, T& out) const {
        const SerialValue* value = FindValue(key);
        return value && SerialTraits<T>::FromValue(*value, out);
    }

    /// Typed read, throws std::runtime_error on a missing or mismatched key.
    template <typename T>
    T Get(const std::string& key) const {
        const SerialValue* value = FindValue(key);
        if (!value) throw std::runtime_error("Key not found: " + key);
        T out{};
        if (!SerialTraits<T>::FromValue(*value, out)) {
            throw std::runtime_error(
                "Key '" + key + "' holds " +
                SerialValue::TypeName(value->GetType()) +
                ", which does not convert to the requested type");
        }
        return out;
    }

    /// Typed read with a fallback for missing or mismatched keys.
    template <typename T>
    T Get(const std::string& key, const T& fallback) const {
        T out = fallback;
        return TryGet(key, out) ? out : fallback;
    }

    /// Serializes a nested object under key.
    void WriteObject(const std::string& key, const Serializable& object);
    /// Deserializes a nested object, false when key is missing or not an
    /// object.
    bool ReadObject(const std::string& key, Serializable& object) const;
};

/// ISerializationContext over an in-memory SerialValue object. One class
/// serves every supported format, only the stream encoding differs.
/// @ingroup filesystem
class ENGINE_API SerializationContext : public ISerializationContext {
   public:
    explicit SerializationContext(
        SerializationFormat format = SerializationFormat::JSON);
    /// Wraps an existing object value, used for nested objects.
    SerializationContext(SerializationFormat format, SerialValue root);

    void Write(const std::string& key, const std::any& value) override;
    std::any Read(const std::string& key) const override;

    void WriteToStream(std::ostream& stream) const override;
    /// Throws std::runtime_error on malformed input.
    void ReadFromStream(std::istream& stream) override;

    void WriteValue(const std::string& key, SerialValue value) override;
    const SerialValue* FindValue(const std::string& key) const override;

    SerializationFormat GetFormat() const { return m_format; }
    SerialValue& GetRoot() { return m_root; }
    const SerialValue& GetRoot() const { return m_root; }

    /// Writes a whole tree to a file, format picked from the extension.
    static void SaveToFile(const SerialValue& root, const std::string& path);
    /// Reads a whole tree from a file, format picked from the extension.
    static SerialValue LoadFromFile(const std::string& path);

   private:
    SerializationFormat m_format;
    SerialValue m_root;
};

/**
 * @class Serializable
 * @brief Base class for objects that can be serialized and deserialized.
 * @ingroup filesystem
 */
class ENGINE_API Serializable {
   public:
    virtual ~Serializable() = default;

    /// Writes this object's state into context.
    virtual void Serialize(ISerializationContext& context) const = 0;

    /// Restores this object's state from context.
    virtual void Deserialize(const ISerializationContext& context) = 0;

    /// Saves to filePath, format picked from the extension. Throws on error.
    void SerializeToFile(const std::string& filePath) const;

    /// Loads from filePath, format picked from the extension. Throws on error.
    void DeserializeFromFile(const std::string& filePath);
};

/**
 * @class SerializationFactory
 * @brief Detects file format and creates the appropriate serialization context.
 * @ingroup filesystem
 */
class ENGINE_API SerializationFactory {
   public:
    /// Picks a format from filePath's extension (.bin/.json/.yaml/.yml/.xml).
    static SerializationFormat DetectFormat(const std::string& filePath);
    /// Builds the context for format. Throws for XML and Unknown.
    static std::unique_ptr<ISerializationContext> CreateContext(
        SerializationFormat format);
};

}  // namespace Sleak

#endif  // _SERIALIZABLE_H_
