#ifndef _GAMEBASE_H_
#define _GAMEBASE_H_

#include <Core/OSDef.hpp>
#include <Utility/Container/List.hpp>
#include <Core/Scene.hpp>

namespace Sleak {

  /// Base class the game's top-level object derives from. Owns the scene
  /// list and the currently active scene; Application drives it each frame.
  ///
  /// This is the root of your game. Subclass it, register your scenes in
  /// Initialize(), and hand an instance to Application::Run(). Three
  /// methods are pure virtual and must be implemented: Initialize() for
  /// one-time setup (return false to abort the run), Begin() for work that
  /// needs the first scene already active, and Loop() for per-frame logic
  /// that is not tied to any single scene.
  ///
  /// GameBase owns every scene passed to AddScene() and unloads and
  /// deletes all of them in its destructor. Never delete a registered
  /// scene yourself; call RemoveScene() instead. When swapping scenes at
  /// runtime, call Application::WaitGPUIdle() first so the GPU is not
  /// still reading resources you are about to free.
  ///
  /// @code{.cpp}
  /// class SLEAK_API Game : public Sleak::GameBase {
  /// public:
  ///     bool Initialize() override {
  ///         Sleak::Application::GetInstance()->SetVSync(true);
  ///
  ///         auto* menu  = new MenuScene();
  ///         auto* world = new WorldScene();
  ///         AddScene(menu);
  ///         AddScene(world);
  ///         SetActiveScene(menu);
  ///         return true;
  ///     }
  ///
  ///     void Begin() override {}
  ///     void Loop(float deltaTime) override {}
  ///     bool GetIsGameRunning() override { return bIsGameRunning; }
  ///
  /// private:
  ///     bool bIsGameRunning = true;
  /// };
  /// @endcode
  ///
  /// @see Application, Scene, SceneBase
  /// @ingroup core
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
    /// One-time setup before the loop starts. Return false to abort the run.
    virtual bool Initialize() = 0;
    /// Called once after Initialize(), before the first Loop().
    virtual void Begin() = 0;
    /// Per-frame update, called after the active scene's own update.
    virtual void Loop(float DeltaTime) = 0;

    virtual bool GetIsGameRunning() = 0;

    // Scene management
    /// Registers a scene; the game takes ownership.
    virtual void AddScene(SceneBase* scene) {
        if (scene) Scenes.add(scene);
    }

    /// Unloads, destroys, and drops a scene from the registry.
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

    /// Deactivates the current scene and activates the given one.
    virtual void SetActiveScene(SceneBase* scene) {
        if (!scene || ActiveScene == scene) return;
        if (ActiveScene) ActiveScene->Deactivate();
        ActiveScene = scene;
        ActiveScene->Activate();
    }

    /// Activates the scene at the given index in the registry.
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
