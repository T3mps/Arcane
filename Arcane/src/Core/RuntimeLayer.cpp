#include "arcpch.h"
#include "RuntimeLayer.h"

#include "Scene/SceneManager.h"

ARC::RuntimeLayer::RuntimeLayer() : Layer("Runtime")
{}

void ARC::RuntimeLayer::OnAttach()
{
   ARC_CORE_INFO("RuntimeLayer attached");
   auto initialScene = SceneManager::GetInstance().CreateScene();
}

void ARC::RuntimeLayer::OnDetach()
{
   ARC_CORE_INFO("RuntimeLayer detached");
   auto& sceneManager = SceneManager::GetInstance();
   auto currentScene = sceneManager.GetCurrentScene();
   UUID sceneUUID = currentScene->GetUUID();
   sceneManager.DestroyScene(sceneUUID);
}

void ARC::RuntimeLayer::OnUpdate(float deltaTime)
{
   auto currentScene = SceneManager::GetInstance().GetCurrentScene();
   ARC_ASSERT(currentScene != nullptr, "Attempted to update a null scene.");
   currentScene->Update(deltaTime);
}

void ARC::RuntimeLayer::OnFixedUpdate(float timeStep)
{
   auto currentScene = SceneManager::GetInstance().GetCurrentScene();
   ARC_ASSERT(currentScene != nullptr, "Attempted to fixed update a null scene.");
   currentScene->FixedUpdate(timeStep);
}

void ARC::RuntimeLayer::OnRender()
{
   auto currentScene = SceneManager::GetInstance().GetCurrentScene();
   ARC_ASSERT(currentScene != nullptr, "Attempted to render a null scene.");
   currentScene->Render();
}
