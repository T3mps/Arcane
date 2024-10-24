#include "arcpch.h"
#include "EventBus.h"

ARC::EventBus::EventBus() : m_lastListenerID(0) {}

void ARC::EventBus::Publish(Event& event)
{
   std::vector<Listener> listenersCopy;
   {
      std::scoped_lock<std::mutex> lock(m_mutex);

      auto typeID = EventTypeID<EventType>::value;
      auto it = m_listeners.find(typeID);

      if (it != m_listeners.end())
         listenersCopy = it->second; // Copy listeners
   }

   // Call callbacks without holding the mutex
   for (const auto& listener : listenersCopy)
   {
      listener.callback(event);
   }
}

