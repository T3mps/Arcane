#include "arcpch.h"
#include "SceneManager.h"

ARC::SceneManager::~SceneManager()
{
   m_scenes.clear();
   m_activeScenes.clear();
}

std::shared_ptr<ARC::Scene> ARC::SceneManager::CreateScene(const std::string& name)
{
   auto scene = std::make_shared<Scene>(name);
   m_scenes[scene->GetUUID()] = scene;

   // If this is the first scene, make it the current scene
   if (m_scenes.size() == 1)
   {
      m_currentScene = scene->GetUUID();
      m_activeScenes.push_back(m_currentScene);
      scene->Start();
   }

   return scene;
}

void ARC::SceneManager::DestroyScene(const UUID& sceneID)
{
   auto it = m_scenes.find(sceneID);
   if (it != m_scenes.end())
   {
      RemoveActiveScene(sceneID);
      m_scenes.erase(it);
   }
}

void ARC::SceneManager::SetActiveScene(const UUID& sceneID)
{
   if (auto currentScene = GetCurrentScene())
   {
      currentScene->Stop();
   }

   m_activeScenes.clear();
   m_currentScene = sceneID;

   if (auto newScene = GetScene(sceneID))
   {
      m_activeScenes.push_back(sceneID);
      newScene->Start();
   }
}

void ARC::SceneManager::AddActiveScene(const UUID& sceneID)
{
   if (std::find(m_activeScenes.begin(), m_activeScenes.end(), sceneID) == m_activeScenes.end())
   {
      if (auto scene = GetScene(sceneID))
      {
         m_activeScenes.push_back(sceneID);
         scene->Start();
      }
   }
}

void ARC::SceneManager::RemoveActiveScene(const UUID& sceneID)
{
   auto it = std::find(m_activeScenes.begin(), m_activeScenes.end(), sceneID);
   if (it != m_activeScenes.end())
   {
      if (auto scene = GetScene(sceneID))
         scene->Stop();
      m_activeScenes.erase(it);
   }
}

std::shared_ptr<ARC::Scene> ARC::SceneManager::GetScene(const UUID& sceneID) const
{
   auto it = m_scenes.find(sceneID);
   return it != m_scenes.end() ? it->second : nullptr;
}

std::shared_ptr<ARC::Scene> ARC::SceneManager::GetSceneByName(const std::string& name) const
{
   for (const auto& [id, scene] : m_scenes)
   {
      if (scene->GetName() == name)
         return scene;
   }
   return nullptr;
}

std::shared_ptr<ARC::Scene> ARC::SceneManager::GetCurrentScene() const
{
   return GetScene(m_currentScene);
}

void ARC::SceneManager::TransitionTo(const UUID& newSceneId, float32_t transitionTime)
{
   if (!m_activeScenes.empty())
      TransitionBetween(m_activeScenes[0], newSceneId, transitionTime);
   else
      SetActiveScene(newSceneId);
}

void ARC::SceneManager::TransitionBetween(const UUID& fromSceneId, const UUID& toSceneId, float32_t transitionTime)
{
   if (m_transition.inProgress)
   {
      ARC_CORE_WARN("Scene transition already in progress!");
      return;
   }

   auto fromScene = GetScene(fromSceneId);
   auto toScene = GetScene(toSceneId);

   if (!fromScene || !toScene)
   {
      ARC_CORE_ERROR("Invalid scene IDs for transition!");
      return;
   }

   m_transition.fromScene = fromSceneId;
   m_transition.toScene = toSceneId;
   m_transition.duration = transitionTime;
   m_transition.elapsed = 0.0f;
   m_transition.inProgress = true;

   fromScene->Pause();
   toScene->Start();
   toScene->Pause();
}

void ARC::SceneManager::UpdateTransition(float32_t deltaTime)
{
   if (!m_transition.inProgress)
      return;

   m_transition.elapsed += deltaTime;
   float32_t t = m_transition.elapsed / m_transition.duration;

   if (t >= 1.0f)
      CompleteTransition();
   else if (m_transition.transitionUpdate)
      m_transition.transitionUpdate(t);
}

void ARC::SceneManager::CompleteTransition()
{
   if (auto fromScene = GetScene(m_transition.fromScene))
      fromScene->Stop();

   if (auto toScene = GetScene(m_transition.toScene))
      toScene->Resume();

   SetActiveScene(m_transition.toScene);
   m_transition.inProgress = false;
}

void ARC::SceneManager::Update(float32_t deltaTime)
{
   UpdateTransition(deltaTime);

   for (const auto& sceneID : m_activeScenes)
   {
      if (auto scene = GetScene(sceneID))
         scene->Update(deltaTime);
   }
}

void ARC::SceneManager::FixedUpdate(float32_t timeStep)
{
   for (const auto& sceneID : m_activeScenes)
   {
      if (auto scene = GetScene(sceneID))
         scene->FixedUpdate(timeStep);
   }
}

void ARC::SceneManager::Render()
{
   for (const auto& sceneID : m_activeScenes)
   {
      if (auto scene = GetScene(sceneID))
         scene->Render();
   }
}
