#include "arcpch.h"
#include "SceneManager.h"

#include "Scene.h"

ARC::SceneManager::SceneManager() :
   m_initialized(false),
   m_currentScene(nullptr)
{}

void ARC::SceneManager::Initialize()
{
   auto& instance = GetInstance();
   auto newScene = std::make_shared<Scene>();
   instance.m_initialized = true;
   instance.ChangeScene(std::move(newScene));
}

bool ARC::SceneManager::ChangeScene(std::shared_ptr<Scene> scene)
{
   auto& instance = GetInstance();

   if (!instance.m_initialized)
   {
      ARC_CORE_ERROR("Initialize Scene Manager before accessing.");
      return false;
   }
   if (!scene)
   {
      ARC_CORE_ERROR("New scene is invalid.");
      return false;
   }
   if (instance.m_currentScene)
   {
      instance.m_currentScene->OnStop();
   }

   instance.m_currentScene = scene;
   instance.m_currentScene->OnStart();

   return true;
}
