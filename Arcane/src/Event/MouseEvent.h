#pragma once

#include "Event.h"
#include "Input/KeyCode.h"

namespace ARC
{
   template <EventType Type, EventCategory Category>
   class MouseEventBase : public EventBase<Type, Category>
   {
   protected:
      MouseEventBase() {}
   };

   template <EventType Type, EventCategory Category>
   class MouseButtonEventBase : public MouseEventBase<Type, Category>
   {
   public:
      MouseButton GetMouseButton() const { return m_button; }

   protected:
      explicit MouseButtonEventBase(MouseButton button) : m_button(button) {}

      MouseButton m_button;
   };

   class MouseButtonPressedEvent final : public MouseButtonEventBase<EventType::MouseButtonPressed, EventCategory::MouseButton | EventCategory::Mouse | EventCategory::Input>
   {
   public:
      explicit MouseButtonPressedEvent(MouseButton button) : MouseButtonEventBase(button) {}
   };

   class MouseButtonReleasedEvent final : public MouseButtonEventBase<EventType::MouseButtonReleased, EventCategory::MouseButton | EventCategory::Mouse | EventCategory::Input>
   {
   public:
      explicit MouseButtonReleasedEvent(MouseButton button) : MouseButtonEventBase(button) {}
   };

   class MouseMovedEvent final : public MouseEventBase<EventType::MouseMoved, EventCategory::Mouse | EventCategory::Input>
   {
   public:
      MouseMovedEvent(float x, float y) : m_mouseX(x), m_mouseY(y) {}

      float GetX() const { return m_mouseX; }
      float GetY() const { return m_mouseY; }

   private:
      float m_mouseX;
      float m_mouseY;
   };

   class MouseScrolledEvent final : public MouseEventBase<EventType::MouseScrolled, EventCategory::Mouse | EventCategory::Input>
   {
   public:
      MouseScrolledEvent(float xOffset, float yOffset) : m_xOffset(xOffset), m_yOffset(yOffset) {}

      float GetXOffset() const { return m_xOffset; }
      float GetYOffset() const { return m_yOffset; }

   private:
      float m_xOffset;
      float m_yOffset;
   };
} // namespace ARC
