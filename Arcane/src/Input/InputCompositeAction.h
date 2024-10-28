#pragma once

#include "InputAction.h"

namespace ARC
{
   enum class CompositeType
   {
      OneAfterAnother = 0, // Second action must start while first is held
      Sequence,            // Actions must be triggered in sequence within timeframe
      Concurrent,          // All actions must be triggered simultaneously
      AndThen              // TODO: First action must be held for a certain period of time, and then the second button is pressed
   };

   template<typename... InputTypes>
   using InputActionVariant = std::variant<std::shared_ptr<InputAction<InputTypes>>...>;

   template<typename InputType>
   class InputCompositeAction : public InputAction<InputType>
   {
   public:
      using TimePoint = std::chrono::steady_clock::time_point;
      using Duration = std::chrono::milliseconds;

      static std::shared_ptr<InputCompositeAction> Create(
         std::shared_ptr<typename InputAction<InputType>::ContainerType> container, 
         size_t index, 
         CompositeType type, 
         std::vector<std::shared_ptr<InputActionBase>> actions, 
         Duration timeout = Duration(100))
      {
         return std::make_shared<InputCompositeAction>(container, index, type, std::move(actions), timeout);
      }

      InputCompositeAction(std::shared_ptr<typename InputAction<InputType>::ContainerType> container, size_t index, CompositeType type, std::vector<std::shared_ptr<InputActionBase>> actions, Duration timeout) :
         InputAction<InputType>(container, index),
         m_type(type),
         m_actions(std::move(actions)),
         m_timeout(timeout)
      {
         if (this->m_container)
            this->m_container->RegisterCompositeAction(this);
      }

      ~InputCompositeAction()
      {
         if (this->m_container)
            this->m_container->UnregisterCompositeAction(this);
      }

      void Reset()
      {
         m_currentAction = 0;
         m_lastActionTime = TimePoint{};
         m_firstActionTriggered = false;
         this->m_container->m_isTriggered[this->m_index] = false;
      }

      void Update()
      {
         if (!this->m_container || !this->m_container->IsEnabled())
         {
            Reset();
            return;
         }

         auto now = std::chrono::steady_clock::now();

         switch (m_type)
         {
            case CompositeType::Sequence:
               UpdateSequence(now);
               break;
            case CompositeType::Concurrent:
               UpdateConcurrent();
               break;
            case CompositeType::OneAfterAnother:
               UpdateOneAfterAnother(now);
               break;
         }
      }

   private:
      void UpdateSequence(const TimePoint& now)
      {
         bool wasTriggered = this->m_container->m_isTriggered[this->m_index];
         auto& data = this->m_container->m_actionData[this->m_index];

         if (m_currentAction == 0 && m_actions[0]->IsStarted())
         {
            m_lastActionTime = now;
            m_currentAction = 1;
            this->m_container->SetSequenceInProgress(true);
            return;
         }

         if (m_currentAction > 0 && (now - m_lastActionTime) > m_timeout)
         {
            if (wasTriggered && data.canceledCallback)
               data.canceledCallback();
            Reset();
            this->m_container->SetSequenceInProgress(false);
            return;
         }

         if (m_currentAction > 0 && m_currentAction < m_actions.size())
         {
            if (m_actions[m_currentAction]->IsStarted())
            {
               m_lastActionTime = now;
               ++m_currentAction;

               if (m_currentAction == m_actions.size())
               {
                  if (data.startedCallback)
                     data.startedCallback();
                  if (data.performedCallback)
                     data.performedCallback();
                  if (data.canceledCallback)
                     data.canceledCallback();

                  this->m_container->m_isTriggered[this->m_index] = true;
                  Reset();
                  this->m_container->SetSequenceInProgress(false);
                  return;
               }
            }
         }

         this->m_container->m_isTriggered[this->m_index] = false;
      }

      void UpdateConcurrent()
      {
         bool allTriggered = true;
         for (const auto& action : m_actions)
         {
            if (!action->IsTriggered())
            {
               allTriggered = false;
               break;
            }
         }
         this->m_container->m_isTriggered[this->m_index] = allTriggered;
      }

      void UpdateOneAfterAnother(const TimePoint& now)
      {
         if (this->m_container->IsSequenceInProgress())
            return;

         if (m_actions.size() < 2)
            return;

         bool wasTriggered = this->m_container->m_isTriggered[this->m_index];

         if (!m_firstActionTriggered && m_actions[0]->IsStarted())
         {
            m_lastActionTime = now;
            m_firstActionTriggered = true;
         }

         bool timeoutExceeded = m_firstActionTriggered && (now - m_lastActionTime) > m_timeout;
         if (timeoutExceeded)
         {
            auto& data = this->m_container->m_actionData[this->m_index];
            if (wasTriggered && data.canceledCallback)
               data.canceledCallback();
            Reset();
            return;
         }

         bool firstActionHeld = m_firstActionTriggered && m_actions[0]->IsTriggered();
         if (!firstActionHeld)
         {
            auto& data = this->m_container->m_actionData[this->m_index];
            if (wasTriggered && data.canceledCallback)
               data.canceledCallback();
            Reset();
            return;
         }

         bool comboSucceeded = m_firstActionTriggered       && 
                               m_actions[0]->IsTriggered()  && 
                               m_actions[1]->IsStarted()    &&
                               (now - m_lastActionTime) <= m_timeout;

         auto& data = this->m_container->m_actionData[this->m_index];

         if (comboSucceeded && !wasTriggered)
         {
            if (data.startedCallback)
               data.startedCallback();
            if (data.performedCallback)
               data.performedCallback();
            if (data.canceledCallback)
               data.canceledCallback();
            Reset();
            return;
         }

         this->m_container->m_isTriggered[this->m_index] = comboSucceeded;
      }

      CompositeType m_type;
      std::vector<std::shared_ptr<InputActionBase>> m_actions;
      Duration m_timeout;
      TimePoint m_lastActionTime;
      size_t m_currentAction = 0;
      bool m_firstActionTriggered = false;
   };
} // namespace ARC