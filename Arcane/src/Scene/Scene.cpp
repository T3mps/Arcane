#include "arcpch.h"
#include "Scene.h"

#include "Components.h"
#include "Core/Application.h"
#include "Core/Console.h"
#include "Entity.h"
#include "Event/SceneEvent.h"
#include "Math/Geometry.h"

ARC::Scene::Scene(const std::string& name) :
   m_name(name),
   m_running(false),
   m_paused(false)
{
}

ARC::Scene::~Scene()
{
}

ARC::Entity ARC::Scene::CreateEntity(const std::string& name)
{
   return CreateEntity(UUID(), name);
}

ARC::Entity ARC::Scene::CreateEntity(UUID uuid, const std::string& name)
{
   Entity entity = { m_registry.create(), this };
   
   entity.AddComponent<Components::ID>(uuid);
   entity.AddComponent<Components::Tag>(name);
   entity.AddComponent<Components::Transform>();
   
   m_entityMap[uuid] = entity;

   SortEntities();

   return entity;
}

void ARC::Scene::DestroyEntity(Entity entity)
{
   m_entityMap.erase(entity.GetComponent<Components::ID>());
   m_registry.destroy(entity);
   SortEntities();
}

ARC::Entity ARC::Scene::FindEntity(const std::string& name)
{
   auto view = m_registry.view<Components::Tag>();
   for (auto entity : view)
   {
      const auto& tag = view.get<Components::Tag>(entity);
      if (tag == name)
         return Entity{ entity, this };
   }

   return Entity{};
}

ARC::Entity ARC::Scene::GetEntity(UUID uuid)
{
   auto entity = m_entityMap.find(uuid);
   if (entity != m_entityMap.end())
      return Entity{ entity->second, this };

   return Entity{};
}

void ARC::Scene::Clear()
{
   // Stop any running systems first
   if (m_running)
   {
      Stop();
   }

   // Clear all entities
   m_entityMap.clear();
   m_registry.clear();

   // Reset state
   m_running = false;
   m_paused = false;
}

void ARC::Scene::Start()
{
   Application::GetInstance().DispatchEvent<ScenePreStartEvent>(shared_from_this());
   m_running = true;
   Application::GetInstance().DispatchEvent<ScenePostStartEvent>(shared_from_this());
}

void ARC::Scene::Stop()
{
   Application::GetInstance().DispatchEvent<ScenePreStopEvent>(shared_from_this());
   m_running = false;
   Application::GetInstance().DispatchEvent<ScenePostStopEvent>(shared_from_this());
}

void ARC::Scene::Pause()
{
   if (m_running && !m_paused)
   {
      m_paused = true;
      Application::GetInstance().DispatchEvent<ScenePausedEvent>(shared_from_this());
   }
}

void ARC::Scene::Resume()
{
   if (m_running && m_paused)
   {
      m_paused = false;
      Application::GetInstance().DispatchEvent<SceneResumedEvent>(shared_from_this());
   }
}

void ARC::Scene::Update(float32_t deltaTime)
{
   if (m_running && !m_paused)
   {
      for (const auto& system : m_updateSystems)
      {
         system(shared_from_this(), deltaTime);
      }
   }
}

void ARC::Scene::FixedUpdate(float32_t timeStep)
{
   if (m_running && !m_paused)
   {
      for (const auto& system : m_fixedUpdateSystems)
      {
         system(shared_from_this(), timeStep);
      }
   }
}

void ARC::Scene::Render()
{
   if (m_running)
   {
      for (auto& system : m_renderSystems)
      {
         system(shared_from_this());
      }
   }
}

ARC::System::SystemID ARC::Scene::AddUpdateSystem(const UpdateSystem& system) 
{ 
   m_updateSystems.Insert(system); 
   return system.GetID();
}

ARC::System::SystemID ARC::Scene::AddFixedUpdateSystem(const UpdateSystem& system) 
{ 
   m_fixedUpdateSystems.Insert(system); 
   return system.GetID();
}

ARC::System::SystemID ARC::Scene::AddRenderSystem(const RenderSystem& system) 
{ 
   m_renderSystems.Insert(system); 
   return system.GetID();
}

const ARC::UpdateSystem* ARC::Scene::GetUpdateSystem(const System::SystemID& id) const { return m_updateSystems.GetByID(id); }
const ARC::UpdateSystem* ARC::Scene::GetFixedUpdateSystem(const System::SystemID& id) const { return m_fixedUpdateSystems.GetByID(id); }
const ARC::RenderSystem* ARC::Scene::GetRenderSystem(const System::SystemID& id) const { return m_renderSystems.GetByID(id); }

void ARC::Scene::RemoveUpdateSystem(const System::SystemID& id) { m_updateSystems.RemoveByID(id); }
void ARC::Scene::RemoveFixedUpdateSystem(const System::SystemID& id) { m_fixedUpdateSystems.RemoveByID(id); }
void ARC::Scene::RemoveRenderSystem(const System::SystemID& id) { m_renderSystems.RemoveByID(id); }

void ARC::Scene::SortEntities()
{
   m_registry.sort<Components::ID>([&](const auto lhs, const auto rhs)
   {
      auto lhsEntity = m_entityMap.find(lhs.value);
      auto rhsEntity = m_entityMap.find(rhs.value);
      return static_cast<uint32_t>(lhsEntity->second) < static_cast<uint32_t>(rhsEntity->second);
   });
}

template <typename T>
void ARC::Scene::OnComponentAdded(Entity entity, T& component)
{
   static_assert(sizeof(T) == 0);
}

template <>
void ARC::Scene::OnComponentAdded<ARC::Components::ID>(Entity entity, Components::ID& component)
{
}

template <>
void ARC::Scene::OnComponentAdded<ARC::Components::Tag>(Entity entity, Components::Tag& component)
{
}

template <>
void ARC::Scene::OnComponentAdded<ARC::Components::Transform>(Entity entity, Components::Transform& component)
{
}
