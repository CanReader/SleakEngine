#ifndef _SCENE_BASE_HPP_
#define _SCENE_BASE_HPP_

#include <string>
#include <Memory/RefPtr.h>
#include <Core/OSDef.hpp>
#include <Utility/Container/List.hpp>
#include <Memory/ObjectPtr.h>

namespace Sleak {

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
        void Load();
        void Unload();
        void Activate();
        void Deactivate();
        void Pause();
        void Resume();

        // Object management — scene takes ownership of added objects
        virtual void AddObject(GameObject* object);
        virtual void RemoveObject(GameObject* object);
        void DestroyObject(GameObject* object);
        const List<GameObject*>& GetObjects() const { return Objects; }

        // Object queries
        GameObject* FindObjectByName(const std::string& name);
        GameObject* FindObjectByID(uint64_t id);
        List<GameObject*> FindObjectsByTag(const std::string& tag);
        size_t GetObjectCount() const { return Objects.GetSize(); }

        Camera* GetDebugCamera() const {
            return DebugCamera.IsValid() ? DebugCamera.get()
                                         : nullptr;
        }

        LightManager* GetLightManager() const {
            return m_lightManager;
        }

    protected:
        std::string name;
        SceneState state;
        bool bInitialized;
        bool bActive;

        List<GameObject*> Objects;
        List<GameObject*> m_pendingDestroy;

        ObjectPtr<Camera> DebugCamera;

        LightManager* m_lightManager = nullptr;

        void ProcessPendingDestroy();
        void DestroyAllObjects();

    private:
        void InitializeDebugCamera();
    };

} // namespace Sleak

#endif // _SCENE_BASE_HPP_


