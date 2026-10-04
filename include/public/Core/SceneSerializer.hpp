#ifndef _SCENE_SERIALIZER_HPP_
#define _SCENE_SERIALIZER_HPP_

#include <Core/OSDef.hpp>
#include <FileSystem/Serializable.hpp>
#include <string>

namespace Sleak {

class SceneBase;
class GameObject;

/// Saves a scene's GameObject hierarchy to JSON, YAML or binary and loads it
/// back into another scene.
///
/// Each object stores its name, tag, active flag, Camera or Light settings,
/// and the components ComponentRegistry knows about. Meshes are stored as
/// the primitive that built them, and objects created by ModelLoader as the
/// model path, so a model's generated children are rebuilt by reloading it.
/// Anything that cannot be stored (a hand-built mesh, an unregistered
/// component) is skipped with a warning naming the object.
///
/// @code{.cpp}
/// Sleak::SceneSerializer(*this).Save("assets/scenes/level1.json");
///
/// Sleak::SceneSerializer loader(*otherScene);
/// if (!loader.Load("assets/scenes/level1.json")) { /* logged */ }
/// @endcode
/// @see ComponentRegistry, Serializable
/// @ingroup scene
class ENGINE_API SceneSerializer {
   public:
    /// Version written to every scene file.
    static constexpr int kVersion = 1;

    explicit SceneSerializer(SceneBase& scene) : m_scene(scene) {}

    /// Writes the scene to path, format picked from the extension. Logs and
    /// returns false on failure.
    bool Save(const std::string& path) const;
    /// Adds the objects stored in path to the scene, keeping existing ones.
    /// Logs and returns false on failure, leaving the scene untouched.
    bool Load(const std::string& path);

    /// Builds the scene document in memory.
    SerialValue SaveToValue() const;
    /// Loads from an in-memory scene document.
    bool LoadFromValue(const SerialValue& document);

    /// Serializes one object and its children.
    static SerialValue SerializeObject(const GameObject& object);
    /// Builds an object tree from SerializeObject output. The caller owns it
    /// until it is added to a scene. Returns nullptr on malformed data.
    static GameObject* DeserializeObject(const SerialValue& data);

   private:
    SceneBase& m_scene;
};

}  // namespace Sleak

#endif  // _SCENE_SERIALIZER_HPP_
