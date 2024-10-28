#pragma once

#include "Scene.h"
#include "Util/Singleton.h"

namespace ARC
{
   class SceneManager : public Singleton<SceneManager>
   {
   public:
      using SceneMap = std::unordered_map<UUID, std::shared_ptr<Scene>>;

      std::shared_ptr<Scene> CreateScene(const std::string& name = "UntitledScene");
      void DestroyScene(const UUID& sceneID);

      bool LoadScene(const std::string& filepath) {} // TODO: Implement
      bool SaveScene(const UUID& sceneID, const std::string& filepath) {} // TODO: Implement

      void SetActiveScene(const UUID& sceneID);
      void AddActiveScene(const UUID& sceneID);
      void RemoveActiveScene(const UUID& sceneID);

      std::shared_ptr<Scene> GetScene(const UUID& sceneID) const;
      std::shared_ptr<Scene> GetSceneByName(const std::string& name) const;
      std::shared_ptr<Scene> GetCurrentScene() const;
      const SceneMap& GetAllScenes() const { return m_scenes; }
      const std::vector<UUID>& GetActiveScenes() const { return m_activeScenes; }

      void TransitionTo(const UUID& newSceneId, float32_t transitionTime = 0.0f);
      void TransitionBetween(const UUID& fromSceneId, const UUID& toSceneId, float32_t transitionTime = 0.0f);

      void Update(float32_t deltaTime);
      void FixedUpdate(float32_t timeStep);
      void Render();

   private:
      SceneManager() = default;
      virtual ~SceneManager();

      void UpdateTransition(float32_t deltaTime);
      void CompleteTransition();

      SceneMap m_scenes;
      std::vector<UUID> m_activeScenes;
      UUID m_currentScene;

      struct TransitionState
      {
         UUID fromScene{};
         UUID toScene{};
         float32_t duration = 0.0f;
         float32_t elapsed = 0.0f;
         std::function<void(float32_t)> transitionUpdate = nullptr;
         bool inProgress = false;
      } m_transition;

      friend class Singleton<SceneManager>;
   };
}
