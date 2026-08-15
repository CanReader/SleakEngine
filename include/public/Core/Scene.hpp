#ifndef _SCENE_HPP_
#define _SCENE_HPP_

#include "SceneBase.hpp"

namespace Sleak {
    /// Concrete scene type games subclass. Adds an opt-out switch for the
    /// fixed-timestep update on top of SceneBase's lifecycle.
    ///
    /// This is the class your levels, menus, and screens derive from.
    /// Override the hooks you need and call the base implementation from
    /// each one: OnLoad() and OnUnload() for assets, OnActivate() and
    /// OnDeactivate() for entering and leaving, Begin() to build the object
    /// graph, and Update() / FixedUpdate() / LateUpdate() for per-frame
    /// work.
    ///
    /// The scene owns every object handed to AddObject(), including lights
    /// and cameras, and destroys all of them on unload. It also owns a
    /// LightManager and a Physics::PhysicsWorld, reachable through
    /// GetLightManager() and GetPhysicsWorld(), and registers added objects
    /// with both automatically.
    ///
    /// Call SetFixedUpdateEnabled(false) on scenes that do not simulate
    /// anything, such as menus, to skip the fixed-timestep pass entirely.
    ///
    /// @code{.cpp}
    /// class WorldScene : public Sleak::Scene {
    /// public:
    ///     WorldScene() : Sleak::Scene("WorldScene") {}
    ///
    ///     void Begin() override {
    ///         auto* cube = Sleak::GameObject::CreateCube(
    ///             Sleak::Math::Vector3D(0.0f, 0.0f, 0.0f));
    ///         AddObject(cube);
    ///
    ///         auto* sun = new Sleak::DirectionalLight("Sun");
    ///         sun->SetDirection(Sleak::Math::Vector3D(-0.5f, -0.8f, -0.3f));
    ///         sun->SetIntensity(4.0f);
    ///         AddObject(sun);
    ///
    ///         auto* cam = new Sleak::Camera("MainCamera");
    ///         AddObject(cam);
    ///         SetActiveCamera(cam);
    ///
    ///         Sleak::Scene::Begin();   // activates everything added above
    ///     }
    ///
    ///     void Update(float deltaTime) override {
    ///         Sleak::Scene::Update(deltaTime);
    ///     }
    /// };
    /// @endcode
    ///
    /// @see SceneBase, GameBase, GameObject, LightManager
    /// @ingroup core
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
