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
      Scene(const std::string& name = "UntitledScene");
      ~Scene();

      Entity CreateEntity(const std::string& name = "Entity");
      Entity CreateEntity(UUID uuid, const std::string& name = "Entity");
      void DestroyEntity(Entity entity);

      Entity FindEntity(const std::string& name);
      Entity GetEntity(UUID uuid);
		
      template <typename... Components>
		auto GetEntitiesWith() { return m_registry.view<Components...>(); }

      void Clear();

      bool Save(const std::string& filepath);
      bool Load(const std::string& filepath);

      void Start();
      void Stop();
      void Pause();
      void Resume();

      void Update(float32_t deltaTime);
      void FixedUpdate(float32_t timeStep);
      void Render();

      const std::string& GetName() const { return m_name; }
      void SetName(const std::string& name) { m_name = name; }

      const UUID& GetUUID() const { return m_uuid; }
      
      System::SystemID AddUpdateSystem(const UpdateSystem& system);
      System::SystemID AddFixedUpdateSystem(const UpdateSystem& system);
      System::SystemID AddRenderSystem(const RenderSystem& system);

      const UpdateSystem* GetUpdateSystem(const System::SystemID& id) const;
      const UpdateSystem* GetFixedUpdateSystem(const System::SystemID& id) const;
      const RenderSystem* GetRenderSystem(const System::SystemID& id) const;

      void RemoveUpdateSystem(const System::SystemID& system);
      void RemoveFixedUpdateSystem(const System::SystemID& system);
      void RemoveRenderSystem(const System::SystemID& system);

      bool IsRunning() const { return m_running; }
      bool IsPaused() const { return m_paused; }

   private:
      void SortEntities();

      template <typename T>
      void OnComponentAdded(Entity entity, T& component);

      std::string m_name;
      UUID m_uuid;

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
