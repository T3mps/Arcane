#pragma once

namespace ARC
{
   enum class EventType
   {
      None = 0,

      // Window
      WindowClosed,
      WindowMinimized,
      WindowResized,
      WindowFocused,
      WindowLostFocus,
      WindowMoved,
      WindowTitleBarClicked,

      // Application
      AppUpdated,
      AppRendered,

      // Keyboard
      KeyPressed,
      KeyReleased,
      KeyTyped,

      // Mouse
      MouseButtonPressed,
      MouseButtonReleased,
      MouseButtonHeld,
      MouseMoved,
      MouseScrolled,

      // Scene
      ScenePreStart,
      ScenePostStart,
      ScenePreStop,
      ScenePostStop
   };

   enum class EventCategory : uint32_t
   {
      None         = 0,
      Application  = (1U << 0U),
      Input        = (1U << 1U),
      Keyboard     = (1U << 2U),
      Mouse        = (1U << 3U),
      MouseButton  = (1U << 4U),
      Scene        = (1U << 5U)
   };

   inline constexpr EventCategory operator|(EventCategory lhs, EventCategory rhs)
   {
      return static_cast<EventCategory>(static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
   }

   inline EventCategory& operator|=(EventCategory& lhs, EventCategory rhs)
   {
      lhs = lhs | rhs;
      return lhs;
   }

   inline constexpr EventCategory operator&(EventCategory lhs, EventCategory rhs)
   {
      return static_cast<EventCategory>(static_cast<uint32_t>(lhs) & static_cast<uint32_t>(rhs));
   }

   inline const char* EventTypeToString(EventType type)
   {
      switch (type)
      {
         case EventType::None: return "None";
         case EventType::WindowClosed: return "WindowClosed";
         case EventType::WindowMinimized: return "WindowMinimized";
         case EventType::WindowResized: return "WindowResized";
         case EventType::WindowFocused: return "WindowFocused";
         case EventType::WindowLostFocus: return "WindowLostFocus";
         case EventType::WindowMoved: return "WindowMoved";
         case EventType::WindowTitleBarClicked: return "WindowTitleBarClicked";
         case EventType::AppUpdated: return "AppUpdated";
         case EventType::AppRendered: return "AppRendered";
         case EventType::KeyPressed: return "KeyPressed";
         case EventType::KeyReleased: return "KeyReleased";
         case EventType::KeyTyped: return "KeyTyped";
         case EventType::MouseButtonPressed: return "MouseButtonPressed";
         case EventType::MouseButtonReleased: return "MouseButtonReleased";
         case EventType::MouseButtonHeld: return "MouseButtonHeld";
         case EventType::MouseMoved: return "MouseMoved";
         case EventType::MouseScrolled: return "MouseScrolled";
         case EventType::ScenePreStart: return "ScenePreStart";
         case EventType::ScenePostStart: return "ScenePostStart";
         case EventType::ScenePreStop: return "ScenePreStop";
         case EventType::ScenePostStop: return "ScenePostStop";
         default: return "Unknown";
      }
   }

   template<typename T>
   struct EventTypeID
   {
      static const std::size_t value;
   };

   template<typename T>
   const std::size_t EventTypeID<T>::value = reinterpret_cast<std::size_t>(&EventTypeID<T>::value);

   class Event
   {
   public:
      using Callback = std::function<void(Event&)>;

      bool handled = false;

      virtual ~Event() {}
      virtual EventType GetEventType() const = 0;
      virtual const char* GetName() const = 0;
      virtual EventCategory GetCategoryFlags() const = 0;
      virtual std::string ToString() const { return GetName(); }

      inline bool IsInCategory(EventCategory category) const
      {
         return static_cast<uint32_t>(GetCategoryFlags()) & static_cast<uint32_t>(category);
      }
   };

   // Defined events should inherit from this class
   template <EventType Type, EventCategory Category>
   class EventBase : public Event
   {
   public:
      static EventType GetStaticType() { return Type; }
      virtual EventType GetEventType() const override { return GetStaticType(); }
      virtual EventCategory GetCategoryFlags() const override { return Category; }
      virtual const char* GetName() const override { return EventTypeToString(Type); }
   };

   inline std::ostream& operator<<(std::ostream& os, const Event& e)
   {
      return os << e.ToString();
   }
}
