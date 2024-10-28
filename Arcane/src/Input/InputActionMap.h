#pragma once

#include "InputActionContainer.h"
#include "InputAction.h"

namespace ARC
{
   template<typename InputType>
   class InputActionMap
   {
   public:
      using ActionType = InputAction<InputType>;
      using ContainerType = ActionContainer<InputType>;

      InputActionMap() : m_container(std::make_shared<ContainerType>()) {}
      InputActionMap(const std::string& name) : m_name(name), m_container(std::make_shared<ContainerType>()) {}

      void AddAction(std::shared_ptr<ActionType> action) { m_actions.push_back(action); }

      std::shared_ptr<ContainerType> GetContainer() const { return m_container; }

      ActionType& AddAction(const std::string& name, InputActionType type)
      {
         size_t index = m_container->CreateAction(name, type);
         auto action = std::make_shared<ActionType>(m_container, index);
         auto& actionRef = *action;
         m_actions.push_back(action);
         return actionRef;
      }

      void RemoveAction(const ActionType* action)
      {
         m_actions.erase(std::remove_if(m_actions.begin(), m_actions.end(),
            [action](const auto& ptr) { return ptr.get() == action; }),
            m_actions.end());
      }

      void Enable() { m_enabled = true; m_container->Enable(); }
      void Disable() { m_enabled = false; m_container->Disable(); }
      bool IsEnabled() const { return m_enabled; }

      void Update()
      {
         if (!m_enabled)
            return;
         m_container->Update();
      }

   private:
      std::string m_name;
      std::vector<std::shared_ptr<ActionType>> m_actions;
      std::shared_ptr<ContainerType> m_container;
      bool m_enabled = false;
   };
} // namespace ARC