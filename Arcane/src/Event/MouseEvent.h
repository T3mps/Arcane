#pragma once

#include "Core/KeyCode.h"
#include "Event.h"

namespace ARC
{
	class MouseEvent : public Event
	{
	public:
		ARC_EVENT_CLASS_CATEGORY(EventCategoryMouse | EventCategoryInput)
	
	protected:
		MouseEvent() {}
	};

	class MouseButtonEvent : public MouseEvent
	{
	public:
		inline MouseButton GetMouseButton() const { return m_button; }

	protected:
		MouseButtonEvent(MouseButton button) : m_button(button) {}

		MouseButton m_button;
	};

	class MouseButtonPressedEvent : public MouseButtonEvent
	{
	public:
		MouseButtonPressedEvent(MouseButton button) : MouseButtonEvent(button) {}

		ARC_EVENT_CLASS_TYPE(MouseButtonPressed)
	};

	class MouseButtonReleasedEvent : public MouseButtonEvent
	{
	public:
		MouseButtonReleasedEvent(MouseButton button) : MouseButtonEvent(button) {}

		ARC_EVENT_CLASS_TYPE(MouseButtonReleased)
	};

	class MouseButtonDownEvent : public MouseButtonEvent
	{
	public:
		MouseButtonDownEvent(MouseButton button) : MouseButtonEvent(button) {}

		ARC_EVENT_CLASS_TYPE(MouseButtonHeld)
	};

	class MouseMovedEvent : public MouseEvent
	{
	public:
		MouseMovedEvent(float x, float y) : m_mouseX(x), m_mouseY(y) {}

		inline float GetX() const { return m_mouseX; }
		inline float GetY() const { return m_mouseY; }

		ARC_EVENT_CLASS_TYPE(MouseMoved)

	private:
		float m_mouseX;
		float m_mouseY;
	};

	class MouseScrolledEvent : public MouseEvent
	{
	public:
		MouseScrolledEvent(float xOffset, float yOffset) : m_xOffset(xOffset), m_yOffset(yOffset) {}

		inline float GetXOffset() const { return m_xOffset; }
		inline float GetYOffset() const { return m_yOffset; }

		ARC_EVENT_CLASS_TYPE(MouseScrolled)
	
	private:
		float m_xOffset;
		float m_yOffset;
	};
} // namespace ARC
