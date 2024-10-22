#pragma once

#include "Core/KeyCode.h"
#include "Event.h"

namespace ARC
{
	class KeyEvent : public Event
	{
	public:
		inline KeyCode GetKeyCode() const { return m_KeyCode; }

		ARC_EVENT_CLASS_CATEGORY(EventCategoryKeyboard | EventCategoryInput)

	protected:
		KeyEvent(KeyCode keycode) : m_KeyCode(keycode) {}

		KeyCode m_KeyCode;
	};

	class KeyPressedEvent : public KeyEvent
	{
	public:
		KeyPressedEvent(KeyCode keycode, int32_t repeatCount) : KeyEvent(keycode), m_RepeatCount(repeatCount) {}

		inline int32_t GetRepeatCount() const { return m_RepeatCount; }

		ARC_EVENT_CLASS_TYPE(KeyPressed)

	private:
		int32_t m_RepeatCount;
	};

	class KeyReleasedEvent : public KeyEvent
	{
	public:
		KeyReleasedEvent(KeyCode keycode) : KeyEvent(keycode) {}

		ARC_EVENT_CLASS_TYPE(KeyReleased)
	};

	class KeyTypedEvent : public KeyEvent
	{
	public:
		KeyTypedEvent(KeyCode keycode) : KeyEvent(keycode) {}

		ARC_EVENT_CLASS_TYPE(KeyTyped)
	};
} // namespace ARC
