#pragma once

#include "Event.h"

namespace ARC
{
	class WindowClosedEvent : public Event
	{
	public:
		WindowClosedEvent() {}

		ARC_EVENT_CLASS_TYPE(WindowClosed)
		ARC_EVENT_CLASS_CATEGORY(EventCategoryApplication)
	};

	class WindowMinimizedEvent : public Event
	{
	public:
		WindowMinimizedEvent(bool minimized) : m_minimized(minimized) {}

		bool IsMinimized() const { return m_minimized; }

		ARC_EVENT_CLASS_TYPE(WindowMinimized)
		ARC_EVENT_CLASS_CATEGORY(EventCategoryApplication)
	
	private:
		bool m_minimized = false;
	};

	class WindowResizedEvent : public Event
	{
	public:
		WindowResizedEvent(uint32_t width, uint32_t height) : m_width(width), m_height(height) {}

		inline uint32_t GetWidth() const { return m_width; }
		inline uint32_t GetHeight() const { return m_height; }

		ARC_EVENT_CLASS_TYPE(WindowResized)
		ARC_EVENT_CLASS_CATEGORY(EventCategoryApplication)

	private:
		uint32_t m_width;
		uint32_t m_height;
	};

	class WindowFocusedEvent : public Event
	{
	public:
		WindowFocusedEvent() {}

		ARC_EVENT_CLASS_TYPE(WindowFocused)
		ARC_EVENT_CLASS_CATEGORY(EventCategoryApplication)
	};

	class WindowLostFocusEvent : public Event
	{
	public:
		WindowLostFocusEvent() {}

		ARC_EVENT_CLASS_TYPE(WindowLostFocus)
		ARC_EVENT_CLASS_CATEGORY(EventCategoryApplication)
	};

	class WindowMovedEvent : public Event
	{
	public:
		WindowMovedEvent() {}

		ARC_EVENT_CLASS_TYPE(WindowMoved)
		ARC_EVENT_CLASS_CATEGORY(EventCategoryApplication)
	};

	class WindowTitleBarHitTestEvent : public Event
	{
	public:
		WindowTitleBarHitTestEvent(int32_t x, int32_t y, int32_t& hit) : m_x(x), m_y(y), m_hit(hit) {}

		inline int32_t GetX() const { return m_x; }
		inline int32_t GetY() const { return m_y; }
		inline void SetHit(bool hit) { m_hit = (int32_t)hit; }

		ARC_EVENT_CLASS_TYPE(WindowTitleBarClicked)
		ARC_EVENT_CLASS_CATEGORY(EventCategoryApplication)

	private:
		int32_t m_x;
		int32_t m_y;
		int32_t& m_hit;
	};

	class AppUpdatedEvent : public Event
	{
	public:
		AppUpdatedEvent() {}

		ARC_EVENT_CLASS_TYPE(AppUpdated)
		ARC_EVENT_CLASS_CATEGORY(EventCategoryApplication)
	};

	class AppRenderedEvent : public Event
	{
	public:
		AppRenderedEvent() {}

		ARC_EVENT_CLASS_TYPE(AppRendered)
		ARC_EVENT_CLASS_CATEGORY(EventCategoryApplication)
	};
} // namespace ARC
