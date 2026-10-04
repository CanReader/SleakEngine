#ifndef _MODEL_SOURCE_COMPONENT_HPP_
#define _MODEL_SOURCE_COMPONENT_HPP_

#include <ECS/Component.hpp>
#include <Runtime/ModelLoader.hpp>
#include <string>

namespace Sleak {

/// Marks the root object ModelLoader built, remembering the file and options
/// so SceneSerializer can store a reference instead of the generated meshes.
/// @ingroup scene
class ModelSourceComponent : public Component {
   public:
    ModelSourceComponent(GameObject* owner, const std::string& path,
                         const ModelLoadOptions& options = {})
        : Component(owner), m_path(path), m_options(options) {}

    bool Initialize() override { return true; }
    void Update(float) override {}

    const std::string& GetPath() const { return m_path; }
    const ModelLoadOptions& GetOptions() const { return m_options; }

   private:
    std::string m_path;
    ModelLoadOptions m_options;
};

}  // namespace Sleak

#endif  // _MODEL_SOURCE_COMPONENT_HPP_
