#ifndef _GAMEBASE_H_
#define _GAMEBASE_H_

#include <Core/OSDef.hpp>
#include <Utility/Container/List.hpp>
#include <Core/Scene.hpp>

namespace Sleak {

  class ENGINE_API GameBase {
  public:
    virtual ~GameBase() {
        ActiveScene = nullptr;
        for (size_t i = 0; i < Scenes.GetSize(); ++i) {
            if (Scenes[i]) {
                Scenes[i]->Unload();
                delete Scenes[i];
            }
        }
        Scenes.clear();
    }
    virtual bool Initialize() = 0;
    virtual void Begin() = 0;
    virtual void Loop(float DeltaTime) = 0;

    virtual bool GetIsGameRunning() = 0;

    // Scene management
    virtual void AddScene(SceneBase* scene) {
        if (scene) Scenes.add(scene);
    }

    virtual void RemoveScene(SceneBase* scene) {
        int index = Scenes.indexOf(scene);
        if (index != -1) {
            if (ActiveScene == scene) {
                ActiveScene->Deactivate();
                ActiveScene = nullptr;
            }
            Scenes[index]->Unload();
            delete Scenes[index];
            Scenes.erase(index);
        }
    }

    virtual void SetActiveScene(SceneBase* scene) {
        if (!scene || ActiveScene == scene) return;
        if (ActiveScene) ActiveScene->Deactivate();
        ActiveScene = scene;
        ActiveScene->Activate();
    }

    virtual void SetActiveScene(int index) {
      SetActiveScene(Scenes[index]);
    }

    virtual SceneBase* GetActiveScene() { return ActiveScene; }

  protected:
    Sleak::List<Sleak::SceneBase*> Scenes;
    SceneBase* ActiveScene = nullptr;
};

}

#endif
