#pragma once

#include "Event.h"

namespace ARC
{
   template <EventType Type, EventCategory Category>
   class ApplicationEventBase : public EventBase<Type, Category>
   {
   protected:
      ApplicationEventBase() {}
   };

   template <EventType Type, EventCategory Category>
   class WindowEventBase : public ApplicationEventBase<Type, Category>
   {
   protected:
      WindowEventBase() {}
   };

   class WindowClosedEvent final : public WindowEventBase<EventType::WindowClosed, EventCategory::Application>
   {
   public:
      WindowClosedEvent() {}
   };

   class WindowMinimizedEvent final : public WindowEventBase<EventType::WindowMinimized, EventCategory::Application>
   {
   public:
      explicit WindowMinimizedEvent(bool minimized) : m_minimized(minimized) {}

      bool IsMinimized() const { return m_minimized; }

   private:
      bool m_minimized;
   };

   class WindowResizedEvent final : public WindowEventBase<EventType::WindowResized, EventCategory::Application>
   {
   public:
      WindowResizedEvent(uint32_t width, uint32_t height) : m_width(width), m_height(height) {}

      uint32_t GetWidth() const { return m_width; }
      uint32_t GetHeight() const { return m_height; }

   private:
      uint32_t m_width;
      uint32_t m_height;
   };

   class WindowFocusedEvent final : public WindowEventBase<EventType::WindowFocused, EventCategory::Application>
   {
   public:
      WindowFocusedEvent() {}
   };

   class WindowLostFocusEvent final : public WindowEventBase<EventType::WindowLostFocus, EventCategory::Application>
   {
   public:
      WindowLostFocusEvent() {}
   };

   class WindowMovedEvent final : public WindowEventBase<EventType::WindowMoved, EventCategory::Application>
   {
   public:
      WindowMovedEvent() {}
   };

   class WindowTitleBarHitTestEvent final : public WindowEventBase<EventType::WindowTitleBarClicked, EventCategory::Application>
   {
   public:
      WindowTitleBarHitTestEvent(int32_t x, int32_t y, int32_t& hit) : m_x(x), m_y(y), m_hit(hit) {}

      int32_t GetX() const { return m_x; }
      int32_t GetY() const { return m_y; }
      void SetHit(bool hit) { m_hit = static_cast<int32_t>(hit); }

   private:
      int32_t m_x;
      int32_t m_y;
      int32_t& m_hit;
   };

   class AppUpdatedEvent final : public ApplicationEventBase<EventType::AppUpdated, EventCategory::Application>
   {
   public:
      AppUpdatedEvent() {}
   };

   class AppRenderedEvent final : public ApplicationEventBase<EventType::AppRendered, EventCategory::Application>
   {
   public:
      AppRenderedEvent() {}
   };
} // namespace ARC
