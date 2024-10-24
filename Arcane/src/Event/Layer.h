#pragma once

#include "Event.h"

namespace ARC
{
   class Layer
   {
   public:
      Layer(const std::string& name = "Layer") : m_debugName(name) {}
      virtual ~Layer() {}

      virtual void OnAttach() {}
      virtual void OnDetach() {}
      virtual void OnUpdate(float deltaTime) {}
      virtual void OnFixedUpdate(float timeStep) {}
      virtual void OnImGuiRender() {}
      virtual void OnEvent(Event& event) {}

      inline const std::string& GetName() const { return m_debugName; }

   protected:
      std::string m_debugName;
   };
} // namespace ARC
