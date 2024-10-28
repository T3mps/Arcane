#pragma once

namespace ARC
{
   class InputActionBase
   {
   public:
      virtual ~InputActionBase() = default;
      virtual bool IsTriggered() const = 0;
      virtual bool IsStarted() const = 0;
      virtual bool IsCanceled() const = 0;
      virtual float GetValue() const = 0;
   };
}
