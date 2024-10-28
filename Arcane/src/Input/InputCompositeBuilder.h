#pragma once

#include "InputActionBuilder.h"
#include "InputCompositeAction.h"

namespace ARC
{
   template<typename ContainerInputType, typename... ActionInputTypes>
   class InputCompositeActionBuilder : public InputActionBuilder<ContainerInputType>
   {
   public:
      using ActionType = typename InputActionBuilder<ContainerInputType>::ActionType;
      using ActionMapType = typename InputActionBuilder<ContainerInputType>::ActionMapType;

      template<typename T>
      using SpecificAction = InputAction<T>;

      static InputCompositeActionBuilder CreateOneAfterAnother(const std::string& name, const std::shared_ptr<SpecificAction<ActionInputTypes>>&... actions)
      {
         std::vector<std::shared_ptr<InputActionBase>> baseActions;
         (baseActions.push_back(actions), ...);
         return InputCompositeActionBuilder(name, CompositeType::OneAfterAnother, std::move(baseActions));
      }

      static InputCompositeActionBuilder CreateSequence(const std::string& name, const std::shared_ptr<SpecificAction<ActionInputTypes>>&... actions)
      {
         std::vector<std::shared_ptr<InputActionBase>> baseActions;
         (baseActions.push_back(actions), ...);
         return InputCompositeActionBuilder(name, CompositeType::Sequence, std::move(baseActions));
      }

      static InputCompositeActionBuilder CreateConcurrent(const std::string& name, const std::shared_ptr<SpecificAction<ActionInputTypes>>&... actions)
      {
         std::vector<std::shared_ptr<InputActionBase>> baseActions;
         (baseActions.push_back(actions), ...);
         return InputCompositeActionBuilder(name, CompositeType::Concurrent, std::move(baseActions));
      }

      InputCompositeActionBuilder& WithTimeout(std::chrono::milliseconds timeout)
      {
         m_timeout = timeout;
         return *this;
      }

      std::shared_ptr<ActionType> Build(std::shared_ptr<typename ActionType::ContainerType> container) override
      {
         size_t index = container->CreateAction(this->m_name, InputActionType::Button);

         auto action = std::make_shared<InputCompositeAction<ContainerInputType>>(
            container, index, m_type, std::move(m_actions), m_timeout);

         if (this->m_startedCallback)
            action->SetStartedCallback(
               [action, callback = this->m_startedCallback]()
               {
                  callback(*action);
               });
         if (this->m_performedCallback)
            action->SetPerformedCallback(
               [action, callback = this->m_performedCallback]()
               {
                  callback(*action);
               });
         if (this->m_canceledCallback)
            action->SetCanceledCallback(
               [action, callback = this->m_canceledCallback]()
               {
                  callback(*action);
               });

         container->SetPriority(index, this->m_priority);
         container->RegisterCompositeAction(action.get());

         return action;
      }

      // Inherit OnStarted/OnPerformed/OnCanceled/WithPriority methods explicitly
      using InputActionBuilder<ContainerInputType>::OnStarted;
      using InputActionBuilder<ContainerInputType>::OnPerformed;
      using InputActionBuilder<ContainerInputType>::OnCanceled;
      using InputActionBuilder<ContainerInputType>::WithPriority;

   private:
      InputCompositeActionBuilder(const std::string& name, CompositeType type, std::vector<std::shared_ptr<InputActionBase>> actions) :
         InputActionBuilder<ContainerInputType>(name, InputActionType::Button),
         m_type(type),
         m_actions(std::move(actions))
      {}

      CompositeType m_type;
      std::vector<std::shared_ptr<InputActionBase>> m_actions;
      std::chrono::milliseconds m_timeout{250};
   };
} // namespace ARC