#pragma once

#include "entt/entt.hpp"
#include "Scene.h"

namespace ARC
{
   class Entity
   {
   public:
      Entity() = default;
      Entity(entt::entity handle, Scene* scene);

      ~Entity() = default;
      
      operator bool()         const { return m_handle != entt::null; }
      operator entt::entity() const { return m_handle; }
      operator uint32_t()     const { return (uint32_t)m_handle; }

      bool operator==(const Entity& other) const { return m_handle == other.m_handle && m_scene == other.m_scene; }
      bool operator!=(const Entity& other) const { return !(*this == other); }

      template <typename Component, typename... Args>
      Component& AddComponent(Args&&... args)
      {
         ARC_ASSERT(!HasComponent<Component>(), "Entity already has component.");
         Component& component = m_scene->m_registry.emplace<Component>(m_handle, std::forward<Args>(args)...);
         m_scene->OnComponentAdded<Component>(*this, component);
         return component;
      }

      template <typename Component, typename... Args>
      Component& AddOrReplaceComponent(Args&&... args)
      {
         Component& component = m_scene->m_registry.emplace_or_replace<Component>(m_handle, std::forward<Args>(args)...);
         m_scene->OnComponentAdded<Component>(*this, component);
         return component;
      }

      template <typename Component>
      Component& GetComponent()
      {
         ARC_ASSERT(HasComponent<Component>(), "Entity doesn't have the specified component.");
         return m_scene->m_registry.get<Component>(m_handle);
      }

      template <typename Component>
      const Component& GetComponent() const 
      {
         ARC_ASSERT(HasComponent<Component>(), "Entity doesn't have the specified component.");
         return m_scene->m_registry.get<Component>(m_handle);
      }

      template <typename Component>
      bool HasComponent() { return m_scene->m_registry.try_get<Component>(m_handle) != nullptr; }

      template <typename Component>
      bool HasComponent() const { return m_scene->m_registry.try_get<Component>(m_handle) != nullptr; }

      template <typename Component>
      void RemoveComponent() { m_scene->m_registry.remove<Component>(m_handle); }
      
   private:
      entt::entity m_handle;
      Scene* m_scene;

      friend class Scene;
   };
} // namespace ARC
