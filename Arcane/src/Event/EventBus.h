#pragma once

#include "Event.h"
#include "Util/Singleton.h"

namespace ARC
{
   class EventBus : public Singleton<EventBus>
   {
   public:
      using ListenerID = size_t;

      template<typename EventType>
      ListenerID Subscribe(std::function<void(const EventType&)> listener)
      {
         std::scoped_lock<std::mutex> lock(m_mutex);

         auto typeIndex = std::type_index(typeid(EventType));
         auto& listeners = m_listeners[typeIndex];
         ListenerID id = ++m_lastListenerID;

         listeners.emplace_back(id, [listener](const Event& event)
            { listener(static_cast<const EventType&>(event)); });

         return id;
      }

      template<typename EventType>
      void Unsubscribe(ListenerID listenerID)
      {
         std::scoped_lock<std::mutex> lock(m_mutex);

         auto typeIndex = std::type_index(typeid(EventType));
         auto& listeners = m_listeners[typeIndex];

         listeners.erase(std::remove_if(listeners.begin(), listeners.end(),
            [listenerID](const Listener& listener) { return listener.id == listenerID; }),
               listeners.end());
      }

      void Publish(Event& event);

   private:
      struct Listener
      {
         ListenerID id;
         std::function<void(Event&)> callback;
      };

      using ListenerMap = std::unordered_map<std::type_index, std::vector<Listener>>;

      EventBus();
      virtual ~EventBus() = default;

      std::unordered_map<std::type_index, std::vector<Listener>> m_listeners;
      ListenerID m_lastListenerID;
      std::mutex m_mutex;

      friend class Singleton<EventBus>;
   };
} // namespace ARC
