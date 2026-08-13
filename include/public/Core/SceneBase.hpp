#ifndef _SCENE_BASE_HPP_
#define _SCENE_BASE_HPP_

#include <string>
#include <Memory/RefPtr.hpp>
#include <Core/OSDef.hpp>
#include <Utility/Container/List.hpp>


namespace Sleak {

    /// Lifecycle state a SceneBase moves through via Load/Unload/Activate/Deactivate.
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
    class ENGINE_API SceneBase {
    public:
        explicit SceneBase(const std::string& name)
            : name(name), state(SceneState::Unloaded),
              bInitialized(false), bActive(false) {}

        virtual ~SceneBase();

        // Resource lifecycle hooks
        virtual void OnLoad() {}
        virtual void OnUnload() {}

        // Activation lifecycle hooks
        virtual void OnActivate() {}
        virtual void OnDeactivate() {}

        // Initialization
        /// Runs once before the scene's first Begin()/Update().
        virtual bool Initialize();
        virtual void Begin() = 0;

        // Update loops
        virtual void Update(float deltaTime) = 0;
        virtual void FixedUpdate(float fixedDeltaTime);
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
        void Pause();
        void Resume();

        // Object management — scene takes ownership of added objects
        virtual void AddObject(GameObject* object);
        virtual void RemoveObject(GameObject* object);
        /// Queues an object for destruction; actually freed on the next ProcessPendingDestroy().
        void DestroyObject(GameObject* object);
        const List<GameObject*>& GetObjects() const { return Objects; }

        // Object queries
        GameObject* FindObjectByName(const std::string& name);
        GameObject* FindObjectByID(uint64_t id);
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


