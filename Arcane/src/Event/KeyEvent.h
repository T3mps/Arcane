#pragma once

#include "Event.h"
#include "Input/KeyCode.h"

namespace ARC
{
   template <EventType Type, EventCategory Category>
   class KeyEventBase : public EventBase<Type, Category>
   {
   public:
      KeyCode GetKeyCode() const { return m_keyCode; }

   protected:
      explicit KeyEventBase(KeyCode keycode) : m_keyCode(keycode) {}

      KeyCode m_keyCode;
   };

   class KeyPressedEvent final : public KeyEventBase<EventType::KeyPressed, EventCategory::Keyboard | EventCategory::Input>
   {
   public:
      KeyPressedEvent(KeyCode keycode, int32_t repeatCount) : KeyEventBase(keycode), m_repeatCount(repeatCount) {}

      int32_t GetRepeatCount() const { return m_repeatCount; }

   private:
      int32_t m_repeatCount;
   };

   class KeyReleasedEvent final : public KeyEventBase<EventType::KeyReleased, EventCategory::Keyboard | EventCategory::Input>
   {
   public:
      explicit KeyReleasedEvent(KeyCode keycode) : KeyEventBase(keycode) {}
   };

   class KeyTypedEvent final : public KeyEventBase<EventType::KeyTyped, EventCategory::Keyboard | EventCategory::Input>
   {
   public:
      explicit KeyTypedEvent(KeyCode keycode) : KeyEventBase(keycode) {}
   };
} // namespace ARC
