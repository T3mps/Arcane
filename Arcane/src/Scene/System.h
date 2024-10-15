#pragma once

namespace ARC
{
   class Scene;

   class System
   {
   public:
      int32_t priority = 0;

      bool operator<(const System& other) const { return priority < other.priority; }
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
         auto it = std::lower_bound(m_systems.begin(), m_systems.end(), system,
            [](const SystemType& a, const SystemType& b) { return a.priority < b.priority; });
         m_systems.insert(it, system);
      }

      typename std::vector<SystemType>::const_iterator begin() const { return m_systems.begin(); }
      typename std::vector<SystemType>::const_iterator end() const { return m_systems.end(); }

   private:
      std::vector<SystemType> m_systems;
   };
} // namespace ARC
