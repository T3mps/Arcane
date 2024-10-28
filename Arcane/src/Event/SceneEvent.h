#pragma once

#include "Event.h"
#include "Scene/Scene.h"

namespace ARC
{

   template <EventType Type, EventCategory Category>
   class SceneEventBase : public EventBase<Type, Category>
   {
   public:
      const std::shared_ptr<Scene>& GetScene() const { return m_scene; }

   protected:
      explicit SceneEventBase(const std::shared_ptr<Scene>& scene) : m_scene(scene) {}

      std::shared_ptr<Scene> m_scene;
   };

   class ScenePreStartEvent final : public SceneEventBase<EventType::ScenePreStart, EventCategory::Scene>
   {
   public:
      explicit ScenePreStartEvent(const std::shared_ptr<Scene>& scene) : SceneEventBase(scene) {}
   };

   class ScenePostStartEvent final : public SceneEventBase<EventType::ScenePostStart, EventCategory::Scene>
   {
   public:
      explicit ScenePostStartEvent(const std::shared_ptr<Scene>& scene) : SceneEventBase(scene) {}
   };

   class ScenePreStopEvent final : public SceneEventBase<EventType::ScenePreStop, EventCategory::Scene>
   {
   public:
      explicit ScenePreStopEvent(const std::shared_ptr<Scene>& scene) : SceneEventBase(scene) {}
   };

   class ScenePostStopEvent final : public SceneEventBase<EventType::ScenePostStop, EventCategory::Scene>
   {
   public:
      explicit ScenePostStopEvent(const std::shared_ptr<Scene>& scene) : SceneEventBase(scene) {}
   };

   class ScenePausedEvent final : public SceneEventBase<EventType::ScenePaused, EventCategory::Scene>
   {
   public:
      explicit ScenePausedEvent(const std::shared_ptr<Scene>& scene) : SceneEventBase(scene) {}
   };

   class SceneResumedEvent final : public SceneEventBase<EventType::SceneResumed, EventCategory::Scene>
   {
   public:
      explicit SceneResumedEvent(const std::shared_ptr<Scene>& scene) : SceneEventBase(scene) {}
   };

} // namespace ARC
