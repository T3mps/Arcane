#pragma once

#include "Scene/Components.h"
#include "Scene/Scene.h"
#include "Util/Singleton.h"

namespace ARC
{
   class Scene;

   class SceneManager : public Singleton<SceneManager>
   {
   public:
      static void Initialize();
      
      static bool ChangeScene(std::shared_ptr<Scene> scene);
         
      [[nodiscard]] static std::shared_ptr<Scene> GetCurrentScene() { return GetInstance().m_currentScene; }

   private:
      SceneManager();
      ~SceneManager() = default;

      bool m_initialized;
      std::shared_ptr<Scene> m_currentScene;

      friend class Singleton<SceneManager>;
   };
} // namespace ARC
