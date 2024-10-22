#pragma once

#include "Event.h"
#include "Scene/Scene.h"

namespace ARC
{
	class SceneEvent : public Event
	{
	public:
		const std::shared_ptr<Scene>& GetScene() const { return m_scene; }
		std::shared_ptr<Scene> GetScene() { return m_scene; }

		ARC_EVENT_CLASS_CATEGORY(EventCategoryApplication | EventCategoryScene)
	
	protected:
		SceneEvent(const std::shared_ptr<Scene>& scene) : m_scene(scene) {}

		std::shared_ptr<Scene> m_scene;
	};

	class ScenePreStartEvent : public SceneEvent
	{
	public:
		ScenePreStartEvent(const std::shared_ptr<Scene>& scene) : SceneEvent(scene) {}

		ARC_EVENT_CLASS_TYPE(ScenePreStart)
	};

	class ScenePostStartEvent : public SceneEvent
	{
	public:
		ScenePostStartEvent(const std::shared_ptr<Scene>& scene) : SceneEvent(scene) {}

		ARC_EVENT_CLASS_TYPE(ScenePostStart)
	};

	class ScenePreStopEvent : public SceneEvent
	{
	public:
		ScenePreStopEvent(const std::shared_ptr<Scene>& scene) : SceneEvent(scene) {}

		ARC_EVENT_CLASS_TYPE(ScenePreStop)
	};

	class ScenePostStopEvent : public SceneEvent
	{
	public:
		ScenePostStopEvent(const std::shared_ptr<Scene>& scene) : SceneEvent(scene) {}

		ARC_EVENT_CLASS_TYPE(ScenePostStop)
	};
} // namespace ARC
