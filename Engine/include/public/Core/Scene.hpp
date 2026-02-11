#ifndef _SCENE_HPP_
#define _SCENE_HPP_

#include "SceneBase.hpp"

namespace Sleak {
    class ENGINE_API Scene : public SceneBase {
    public:
        explicit Scene(const std::string& name)
            : SceneBase(name), bEnableFixedUpdate(true) {}
        ~Scene() override = default;

        // Resource lifecycle
        void OnLoad() override;
        void OnUnload() override;

        // Activation lifecycle
        void OnActivate() override;
        void OnDeactivate() override;

        // Initialization
        bool Initialize() override;
        void Begin() override;

        // Update loops
        void Update(float deltaTime) override;
        void FixedUpdate(float fixedDeltaTime) override;
        void LateUpdate(float deltaTime) override;

        void SetFixedUpdateEnabled(bool enabled) { bEnableFixedUpdate = enabled; }
        bool IsFixedUpdateEnabled() const { return bEnableFixedUpdate; }

    private:
        bool bEnableFixedUpdate;
    };
}

#endif // _SCENE_HPP_
