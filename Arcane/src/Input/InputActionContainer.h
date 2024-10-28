#pragma once

#include "InputAction.h"
#include "InputBinding.h"
#include "InputCompositeAction.h"

namespace ARC
{
   template<typename InputType>
   class InputActionBuilder;

   template<typename ContainerInputType, typename... ActionInputTypes>
   class InputCompositeActionBuilder;

   enum class InputActionType
   {
      Button = 0,
      Value,
      PassThrough
   };

   enum class ActionPriority
   {
      High = 0,
      Medium,
      Low
   };

   struct ActionData;

   struct ActionComparator
   {
      bool operator()(const std::tuple<ActionPriority, size_t>& lhs, const std::tuple<ActionPriority, size_t>& rhs) const
      {
         return static_cast<int>(std::get<0>(lhs)) > static_cast<int>(std::get<0>(rhs));
      }
   };

   template<typename InputType>
   class ActionContainer
   {
   public:
      using Callback = std::function<void()>;
      using BindingType = InputBinding<InputType>;

      struct ActionData
      {
         std::string name;
         InputActionType type;
         std::vector<BindingType> bindings;
         Callback startedCallback;
         Callback performedCallback;
         Callback canceledCallback;
         ActionPriority priority;
      };

      size_t CreateAction(const std::string& name, InputActionType type)
      {
         size_t index = m_actionData.size();
         m_actionData.push_back({name, type, {}, nullptr, nullptr, nullptr, ActionPriority::Medium});
         m_isTriggered.push_back(false);
         m_wasTriggered.push_back(false);

         m_sortedIndices.push_back({ActionPriority::Medium, index});
         m_priorityDirty = true;

         return index;
      }

      void Update()
      {
         m_wasTriggered = m_isTriggered;
         std::fill(m_isTriggered.begin(), m_isTriggered.end(), false);

         if (m_priorityDirty)
         {
            std::sort(m_sortedIndices.begin(), m_sortedIndices.end(), ActionComparator{});
            m_priorityDirty = false;
         }

         for (auto* composite : m_compositeActions)
         {
            composite->Update();
         }

         for (const auto& [priority, index] : m_sortedIndices)
         {
            switch (m_actionData[index].type)
            {
               case InputActionType::Button:
                  UpdateButtonAction(index);
                  break;
               case InputActionType::Value: ARC_FALLTHROUGH;
               case InputActionType::PassThrough:
                  UpdateValueAction(index);
                  break;
            }
         }
      }

      void SetPriority(size_t index, ActionPriority priority)
      {
         if (index >= m_actionData.size())
            return;

         if (m_actionData[index].priority != priority)
         {
            m_actionData[index].priority = priority;

            auto it = std::find_if(m_sortedIndices.begin(), m_sortedIndices.end(),
               [index](const auto& pair) { return std::get<1>(pair) == index; });

            if (it != m_sortedIndices.end())
            {
               std::get<0>(*it) = priority;
               m_priorityDirty = true;
            }
         }
      }

      void AddBinding(size_t index, BindingType binding)
      { 
         if (index >= m_actionData.size())
            return;
         m_actionData[index].bindings.push_back(std::move(binding)); 
      }

      void SetStartedCallback(size_t index, const Callback& callback)
      { 
         if (index >= m_actionData.size())
            return;
         m_actionData[index].startedCallback = callback; 
      }

      void SetPerformedCallback(size_t index, const Callback& callback)
      { 
         if (index >= m_actionData.size())
            return;
         m_actionData[index].performedCallback = callback; 
      }

      void SetCanceledCallback(size_t index, const Callback& callback)
      { 
         if (index >= m_actionData.size())
            return;
         m_actionData[index].canceledCallback = callback; 
      }

      void RegisterCompositeAction(InputCompositeAction<InputType>* action)
      {
         m_compositeActions.push_back(action);
      }

      void UnregisterCompositeAction(InputCompositeAction<InputType>* action)
      {
         m_compositeActions.erase(std::remove(m_compositeActions.begin(), m_compositeActions.end(), action), m_compositeActions.end());
      }

      bool IsTriggered(size_t index) const { return index < m_isTriggered.size() ? m_isTriggered[index] : false; }

      bool WasTriggered(size_t index) const { return index < m_wasTriggered.size() ? m_wasTriggered[index] : false; }

      const std::string& GetName(size_t index) const
      {
         static const std::string empty;
         return index < m_actionData.size() ? m_actionData[index].name : empty; 
      }

      const std::vector<BindingType>& GetBindings(size_t index) const
      {
         static const std::vector<BindingType> empty;
         return index < m_actionData.size() ? m_actionData[index].bindings : empty;
      }

      float GetValue(size_t index) const
      {
         if (index >= m_actionData.size())
            return 0.0f;

         float totalValue = 0.0f;
         for (const auto& binding : m_actionData[index].bindings)
         {
            totalValue += binding.GetValue();
         }
         return totalValue;
      }

      bool IsSequenceInProgress() const { return m_sequenceInProgress; }
      void SetSequenceInProgress(bool inProgress) { m_sequenceInProgress = inProgress; }

      bool IsEnabled() const { return m_enabled; }

      void Enable() { m_enabled = true; }
      void Disable() { m_enabled = false; }

   private:
      void UpdateButtonAction(size_t index)
      {
         for (const auto& binding : m_actionData[index].bindings)
         {
            if (binding.IsPressed() || binding.IsHeld())
            {
               m_isTriggered[index] = true;
               break;
            }
         }

         auto& data = m_actionData[index];
         if (m_isTriggered[index] && !m_wasTriggered[index] && data.startedCallback)
            data.startedCallback();
         if (m_isTriggered[index] && data.performedCallback)
            data.performedCallback();
         if (!m_isTriggered[index] && m_wasTriggered[index] && data.canceledCallback)
            data.canceledCallback();
      }

      void UpdateValueAction(size_t index)
      {
         float value = GetValue(index);
         m_isTriggered[index] = (value != 0.0f);

         if (m_isTriggered[index] && m_actionData[index].performedCallback)
            m_actionData[index].performedCallback();
      }

      std::vector<ActionData> m_actionData;
      std::vector<bool> m_isTriggered;
      std::vector<bool> m_wasTriggered;
      std::vector<std::tuple<ActionPriority, size_t>> m_sortedIndices;
      std::vector<InputCompositeAction<InputType>*> m_compositeActions;
      bool m_priorityDirty = false;
      bool m_enabled = false;
      bool m_sequenceInProgress = false;

      friend class InputActionBuilder<InputType>;
      template<typename InputType, typename... ActionInputTypes>
      friend class InputCompositeActionBuilder;
      friend class InputCompositeAction<InputType>;
   };
}
