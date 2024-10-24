#pragma once

#include "entt/entt.hpp"
#include "System.h"
#include "Util/UUID.h"

namespace ARC
{
   class Entity;

   class Scene : public std::enable_shared_from_this<Scene>
   {
   public:
      Scene();
      ~Scene();

      Entity CreateEntity(const std::string& name = "Entity");
      Entity CreateEntity(UUID uuid, const std::string& name = "Entity");
      void DestroyEntity(Entity entity);

      Entity FindEntity(const std::string& name);
      Entity GetEntity(UUID uuid);
		
      template <typename... Components>
		auto GetEntitiesWith() { return m_registry.view<Components...>(); }

      void OnStart();
      void OnStop();

      void Update(float32_t deltaTime);
      void FixedUpdate(float32_t timeStep);
      void Render();

      void AddUpdateSystem(const UpdateSystem& system) { m_updateSystems.Insert(system); }
      void AddRenderSystem(const RenderSystem& system) { m_renderSystems.Insert(system); }

      bool IsRunning() const { return m_running; }

   private:
      void SortEntities();

      template <typename T>
      void OnComponentAdded(Entity entity, T& component);

      entt::registry m_registry;
      std::unordered_map<UUID, entt::entity> m_entityMap;

      SortedSystemList<UpdateSystem> m_updateSystems;
      SortedSystemList<UpdateSystem> m_fixedUpdateSystems;
      SortedSystemList<RenderSystem> m_renderSystems;

      bool m_running;
      bool m_paused;

      friend class Entity;
   };
} // namespace ARC
