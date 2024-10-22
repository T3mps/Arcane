#pragma once

namespace ARC
{
	enum class EventType
	{
		None = 0,
		WindowClosed, WindowMinimized, WindowResized, WindowFocused, WindowLostFocus, WindowMoved, WindowTitleBarClicked,
		AppUpdated, AppRendered,
		KeyPressed, KeyReleased, KeyTyped,
		MouseButtonPressed, MouseButtonReleased, MouseButtonHeld, MouseMoved, MouseScrolled,
		ScenePreStart, ScenePostStart, ScenePreStop, ScenePostStop
	};

	enum EventCategory
	{
		None = 0,
		EventCategoryApplication	= (1U << 0U),
		EventCategoryInput         = (1U << 1U),
		EventCategoryKeyboard      = (1U << 2U),
		EventCategoryMouse         = (1U << 3U),
		EventCategoryMouseButton   = (1U << 4U),
		EventCategoryScene			= (1U << 5U)
	};

#define ARC_EVENT_CLASS_TYPE(type) \
	static EventType GetStaticType() { return EventType::type; } \
	virtual EventType GetEventType() const override { return GetStaticType(); } \
	virtual const char* GetName() const override { return #type; }

#define ARC_EVENT_CLASS_CATEGORY(category) \
	virtual int32_t GetCategoryFlags() const override { return category; }

	struct Event
	{
		using Callback = std::function<void(Event&)>;

		bool handled = false;
		bool synced = false; // Queued events are only processed if this is true.  It is set true when asset thread syncs with main thread.

		virtual ~Event() {}
		virtual EventType GetEventType() const = 0;
		virtual const char* GetName() const = 0;
		virtual int32_t GetCategoryFlags() const = 0;
		virtual std::string ToString() const { return GetName(); }

		inline bool IsInCategory(EventCategory category) { return GetCategoryFlags() & category; }
	};

	class EventDispatcher
	{
		template<typename T>
		using EventFn = std::function<bool(T&)>;
	public:
		EventDispatcher(Event& event) : m_event(event) {}

		template<typename T>
		bool Dispatch(EventFn<T> func)
		{
			if (m_event.GetEventType() == T::GetStaticType() && !m_event.handled)
			{
				m_event.handled = func(*(T*)&m_event);
				return true;
			}
			return false;
		}
	private:
		Event& m_event;
	};

	inline std::ostream& operator<<(std::ostream& os, const Event& e)
	{
		return os << e.ToString();
	}
}

#define ARC_BIND_EVENT_CALLBACK(fn) std::bind(&Application::##fn, this, std::placeholders::_1)
