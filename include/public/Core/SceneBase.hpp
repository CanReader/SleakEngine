#ifndef _SCENE_BASE_HPP_
#define _SCENE_BASE_HPP_

#include <string>
#include <Memory/RefPtr.hpp>
#include <Core/OSDef.hpp>
#include <Utility/Container/List.hpp>


namespace Sleak {

    /// Lifecycle state a SceneBase moves through via Load/Unload/Activate/Deactivate.
    /// @ingroup core
    enum class SceneState {
        Unloaded,
        Loading,
        Active,
        Paused,
        Unloading
    };

    class GameObject;
    class Camera;
    class Light;
    class LightManager;
    class Skybox;
    class ColliderComponent;

    namespace Physics { class PhysicsWorld; }

    /// Non-templated scene interface: object ownership, state machine, and
    /// per-frame update hooks. Game scenes derive from Scene, not this directly.
    ///
    /// SceneBase defines what every scene can do regardless of subclass:
    /// hold objects, move through the SceneState machine, and answer
    /// queries. GameBase stores scenes by `SceneBase*`, so this is the type
    /// you see in the scene registry API.
    ///
    /// Ownership is strict. AddObject() transfers ownership to the scene.
    /// RemoveObject() unregisters and deletes immediately, which is unsafe
    /// from inside that object's own update. DestroyObject() queues the
    /// object (and its children) for deletion at the end of the frame,
    /// which is the safe choice while iterating.
    ///
    /// Objects added to the scene are registered with the LightManager if
    /// they report IsLight(), and their colliders are registered with the
    /// PhysicsWorld, hierarchy included.
    ///
    /// @code{.cpp}
    /// // Destroy safely from inside an update
    /// if (auto* target = FindObjectByName("Crate")) {
    ///     DestroyObject(target);
    /// }
    ///
    /// // Act on a group
    /// Sleak::List<Sleak::GameObject*> enemies = FindObjectsByTag("Enemy");
    /// for (size_t i = 0; i < enemies.GetSize(); ++i) {
    ///     enemies[i]->SetActive(false);
    /// }
    ///
    /// // Scene-wide services
    /// if (auto* lm = GetLightManager()) lm->SetAmbientIntensity(0.6f);
    /// if (auto* pw = GetPhysicsWorld()) {
    ///     auto hit = pw->Raycast(origin, direction, 100.0f);
    /// }
    /// @endcode
    ///
    /// @see Scene, GameBase, GameObject, SceneState, Physics::PhysicsWorld
    /// @ingroup core
    class ENGINE_API SceneBase {
    public:
        explicit SceneBase(const std::string& name)
            : name(name), state(SceneState::Unloaded),
              bInitialized(false), bActive(false) {}

        virtual ~SceneBase();

        // Resource lifecycle hooks
        /// Override to load scene-specific assets; called once from Load().
        virtual void OnLoad() {}
        /// Override to release scene-specific assets; called from Unload().
        virtual void OnUnload() {}

        // Activation lifecycle hooks
        /// Override for logic that should run when the scene becomes active.
        virtual void OnActivate() {}
        /// Override for logic that should run when the scene stops being active.
        virtual void OnDeactivate() {}

        // Initialization
        /// Runs once before the scene's first Begin()/Update().
        virtual bool Initialize();
        /// Activates every owned object; runs once after Initialize().
        virtual void Begin() = 0;

        // Update loops
        /// Advances all active, root-level objects by deltaTime, then steps lighting and physics.
        virtual void Update(float deltaTime) = 0;
        /// Advances all active, root-level objects on the fixed timestep.
        virtual void FixedUpdate(float fixedDeltaTime);
        /// Advances all active, root-level objects after the main Update pass.
        virtual void LateUpdate(float deltaTime);

        // State
        const std::string& GetName() const { return name; }
        SceneState GetState() const { return state; }
        bool IsActive() const { return bActive; }
        bool IsLoaded() const { return state != SceneState::Unloaded; }

        // Scene state transitions
        /// Moves Unloaded -> Loading -> Active, calling OnLoad() and Initialize().
        void Load();
        /// Deactivates if active, calls OnUnload(), and destroys all owned objects.
        void Unload();
        /// Marks the scene active and calls OnActivate().
        void Activate();
        /// Marks the scene inactive and calls OnDeactivate().
        void Deactivate();
        /// Freezes the scene without tearing it down; leaves it in the Paused state.
        void Pause();
        /// Reactivates a paused scene without re-running OnActivate().
        void Resume();

        // Object management — scene takes ownership of added objects
        /// Takes ownership of object, registering it with lighting and physics as needed.
        virtual void AddObject(GameObject* object);
        /// Unregisters and deletes object immediately.
        virtual void RemoveObject(GameObject* object);
        /// Queues an object for destruction; actually freed on the next ProcessPendingDestroy().
        void DestroyObject(GameObject* object);
        const List<GameObject*>& GetObjects() const { return Objects; }

        // Object queries
        /// Linear search for the first object with a matching name.
        GameObject* FindObjectByName(const std::string& name);
        /// Linear search for the object with a matching unique ID.
        GameObject* FindObjectByID(uint64_t id);
        /// Collects every object whose tag matches.
        List<GameObject*> FindObjectsByTag(const std::string& tag);
        size_t GetObjectCount() const { return Objects.GetSize(); }

        Camera* GetActiveCamera() const { return m_activeCamera; }
        void SetActiveCamera(Camera* cam) { m_activeCamera = cam; }

        LightManager* GetLightManager() const {
            return m_lightManager;
        }

        Physics::PhysicsWorld* GetPhysicsWorld() const {
            return m_physicsWorld;
        }

        /// Replaces the scene's skybox, deleting the previous one.
        void SetSkybox(Skybox* skybox);
        Skybox* GetSkybox() const { return m_skybox; }

    protected:
        std::string name;
        SceneState state;
        bool bInitialized;
        bool bActive;

        List<GameObject*> Objects;
        List<GameObject*> m_pendingDestroy;

        Camera* m_activeCamera = nullptr;

        LightManager* m_lightManager = nullptr;
        Physics::PhysicsWorld* m_physicsWorld = nullptr;
        Skybox* m_skybox = nullptr;

        /// Actually deletes objects queued by DestroyObject().
        void ProcessPendingDestroy();
        /// Destroys every object still owned by the scene, e.g. during Unload().
        void DestroyAllObjects();
    };

} // namespace Sleak

#endif // _SCENE_BASE_HPP_


