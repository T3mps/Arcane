#include "arcpch.h"
#include "Scene.h"

#include "Components.h"
#include "Core/Console.h"
#include "Entity.h"
#include "Math/Geometry.h"

ARC::Scene::Scene() :
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

void ARC::Scene::OnStart()
{
   m_running = true;
}

void ARC::Scene::OnStop()
{
   m_running = false;
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
