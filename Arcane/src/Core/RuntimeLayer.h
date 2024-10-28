#pragma once

#include "Event/Layer.h"
#include "Scene/Scene.h"

namespace ARC
{
   class RuntimeLayer final : public Layer
   {
   public:
      RuntimeLayer();
      virtual ~RuntimeLayer() = default;

      void OnAttach() override;
      void OnDetach() override;
      void OnUpdate(float deltaTime) override;
      void OnFixedUpdate(float timeStep) override;
      void OnRender() override;
   };
} // namespace ARC
