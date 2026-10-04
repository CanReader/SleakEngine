#ifndef _COMPONENT_REGISTRY_HPP_
#define _COMPONENT_REGISTRY_HPP_

#include <Core/GameObject.hpp>
#include <Core/OSDef.hpp>
#include <ECS/Component.hpp>
#include <FileSystem/Serializable.hpp>
#include <functional>
#include <string>
#include <typeindex>
#include <typeinfo>
#include <vector>

namespace Sleak {

/// Maps component type names to factories so SceneSerializer can save and
/// rebuild components it knows nothing about at compile time.
///
/// Engine components are registered on first use. Game components opt in by
/// overriding Component::Serialize/Deserialize and calling Register<T>() once
/// at startup, or by passing their own functions when the constructor needs
/// more than the owner.
///
/// @code{.cpp}
/// Sleak::ComponentRegistry::Register<SpinComponent>("Spin");
/// @endcode
/// @ingroup scene
class ENGINE_API ComponentRegistry {
   public:
    /// Adds the component to owner from saved data and returns it.
    using CreateFn = std::function<Component*(GameObject& owner,
                                              const ISerializationContext&)>;
    /// Finds the existing component of this type on owner, or nullptr.
    using FindFn = std::function<Component*(GameObject& owner)>;
    /// Writes the component's data. Returning false skips the component.
    using SaveFn =
        std::function<bool(const Component&, ISerializationContext&)>;
    /// Applies saved data to a component that already exists.
    using LoadFn =
        std::function<void(Component&, const ISerializationContext&)>;

    /// One registered component type.
    struct Entry {
        std::string name;
        std::type_index type;
        CreateFn create;
        FindFn find;
        SaveFn save;
        LoadFn load;
    };

    /// Registers a type with explicit functions. A second registration of the
    /// same name or type replaces the first.
    static void Register(const std::string& name, std::type_index type,
                         CreateFn create, FindFn find, SaveFn save,
                         LoadFn load);

    /// Registers T using its (GameObject*) constructor and its
    /// Serialize/Deserialize overrides.
    template <typename T>
    static void Register(const std::string& name) {
        static_assert(std::is_base_of_v<Component, T>,
                      "T must derive from Component!");
        Register(
            name, typeid(T),
            [](GameObject& owner, const ISerializationContext& data) {
                owner.AddComponent<T>();
                Component* c = owner.GetComponent<T>();
                if (c) c->Deserialize(data);
                return c;
            },
            [](GameObject& owner) -> Component* {
                return owner.GetComponent<T>();
            },
            [](const Component& c, ISerializationContext& data) {
                c.Serialize(data);
                return true;
            },
            [](Component& c, const ISerializationContext& data) {
                c.Deserialize(data);
            });
    }

    /// Entry for a saved type name, nullptr when unknown.
    static const Entry* Find(const std::string& name);
    /// Entry for the dynamic type of component, nullptr when unregistered.
    static const Entry* Find(const Component& component);

   private:
    static std::vector<Entry>& Entries();
};

}  // namespace Sleak

#endif  // _COMPONENT_REGISTRY_HPP_
