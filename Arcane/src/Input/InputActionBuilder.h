#pragma once

#include "InputAction.h"
#include "InputActionMap.h"
#include "InputBinding.h"
#include "InputActionBase.h"

namespace ARC
{
   template<typename InputType>
   class InputActionBuilder
   {
   public:
      using ActionType = InputAction<InputType>;
      using ActionMapType = InputActionMap<InputType>;
      using BindingType = InputBinding<InputType>;
      using BasicCallback = std::function<void()>;
      using ActionCallback = std::function<void(const ActionType&)>;

      static InputActionBuilder CreateButtonAction(const std::string& name) { return InputActionBuilder(name, InputActionType::Button); }
      static InputActionBuilder CreateValueAction(const std::string& name) { return InputActionBuilder(name, InputActionType::Value); }
      static InputActionBuilder CreatePassThroughAction(const std::string& name) { return InputActionBuilder(name, InputActionType::PassThrough); }

      InputActionBuilder& WithBinding(BindingType binding)
      {
         m_bindings.push_back(std::move(binding));
         return *this;
      }

      InputActionBuilder& WithBindings(std::initializer_list<BindingType> bindings)
      {
         m_bindings.insert(m_bindings.end(), std::make_move_iterator(bindings.begin()), std::make_move_iterator(bindings.end()));
         return *this;
      }

      InputActionBuilder& OnStarted(const BasicCallback& callback)
      {
         m_startedCallback = [callback](const ActionType&) { callback(); };
         return *this;
      }

      InputActionBuilder& OnPerformed(const BasicCallback& callback)
      {
         m_performedCallback = [callback](const ActionType&) { callback(); };
         return *this;
      }

      InputActionBuilder& OnCanceled(const BasicCallback& callback)
      {
         m_canceledCallback = [callback](const ActionType&) { callback(); };
         return *this;
      }

      InputActionBuilder& OnStarted(const ActionCallback& callback)
      {
         m_startedCallback = callback;
         return *this;
      }

      InputActionBuilder& OnPerformed(const ActionCallback& callback)
      {
         m_performedCallback = callback;
         return *this;
      }

      InputActionBuilder& OnCanceled(const ActionCallback& callback)
      {
         m_canceledCallback = callback;
         return *this;
      }

      InputActionBuilder& WithPriority(ActionPriority priority)
      {
         m_priority = priority;
         return *this;
      }

      virtual std::shared_ptr<ActionType> Build(std::shared_ptr<typename ActionType::ContainerType> container)
      {
         size_t index = container->CreateAction(m_name, m_type);
         auto action = std::make_shared<ActionType>(container, index);

         for (auto& binding : m_bindings)
         {
            action->AddBinding(std::move(binding));
         }

         if (m_startedCallback)
         {
            action->SetStartedCallback([action, callback = m_startedCallback]()
               {
                  callback(*action);
               });
         }
         if (m_performedCallback)
         {
            action->SetPerformedCallback([action, callback = m_performedCallback]()
               {
                  callback(*action);
               });
         }
         if (m_canceledCallback)
         {
            action->SetCanceledCallback([action, callback = m_canceledCallback]()
               {
                  callback(*action);
               });
         }

         action->m_container->SetPriority(index, m_priority);
         return action;
      }

      virtual ActionType& BuildAndAddToMap(ActionMapType& actionMap)
      {
         auto action = Build(actionMap.GetContainer());
         auto& actionRef = *action;
         actionMap.AddAction(action);
         return actionRef;
      }

   protected:
      InputActionBuilder(const std::string& name, InputActionType type) : m_name(name), m_type(type) {}

      std::string m_name;
      InputActionType m_type;
      std::vector<BindingType> m_bindings;
      ActionCallback m_startedCallback;
      ActionCallback m_performedCallback;
      ActionCallback m_canceledCallback;
      ActionPriority m_priority = ActionPriority::Medium;
   };
} // namespace ARC