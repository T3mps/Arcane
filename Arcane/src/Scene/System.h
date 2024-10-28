#pragma once

#include "Util/UUID.h"

namespace ARC
{
   class Scene;

   class System
   {
   public:
      using SystemID = UUID;

      System() : m_id(SystemID()), m_priority(0) {}
      virtual ~System() = default;

      SystemID GetID() const { return m_id; }
      int32_t GetPriority() const { return m_priority; }

      bool operator<(const System& other) const { return m_priority < other.m_priority; }

   protected:
      SystemID m_id;
      int32_t m_priority;
   };

   struct UpdateSystem : public System
   {
      std::function<void(std::shared_ptr<Scene>, float32_t)> system;

      void operator()(std::shared_ptr<Scene> scene, float32_t deltaTime) { system(scene, deltaTime); }
      void operator()(std::shared_ptr<Scene> scene, float32_t deltaTime) const { system(scene, deltaTime); }
   };

   struct RenderSystem : public System
   {
      std::function<void(std::shared_ptr<Scene>)> system;

      void operator()(std::shared_ptr<Scene> scene) { system(scene); }
      void operator()(std::shared_ptr<Scene> scene) const { system(scene); }
   };

   template <typename SystemType>
   class SortedSystemList
   {
   public:
      SortedSystemList(size_t capacity = 32)
      {
         m_systems.reserve(capacity);
      }

      ~SortedSystemList() = default;

      void Insert(const SystemType& system)
      {
         RemoveByID(system.GetID());

         auto it = std::lower_bound(m_systems.begin(), m_systems.end(), system, [](const SystemType& a, const SystemType& b)
            { return a.GetPriority() < b.GetPriority(); });
         m_systems.insert(it, system);
      }

      void RemoveByID(const typename System::SystemID& id)
      {
         m_systems.erase(std::remove_if(m_systems.begin(), m_systems.end(),
            [&id](const SystemType& system) { return system.GetID() == id; }),
               m_systems.end());
      }

      const SystemType* GetByID(const typename System::SystemID& id) const
      {
         auto it = std::find_if(m_systems.begin(), m_systems.end(),
            [&id](const SystemType& system) { return system.GetID() == id; });
         return it != m_systems.end() ? &(*it) : nullptr;
      }

      typename std::vector<SystemType>::const_iterator begin() const { return m_systems.begin(); }
      typename std::vector<SystemType>::const_iterator end() const { return m_systems.end(); }

   private:
      std::vector<SystemType> m_systems;
   };
} // namespace ARC
