#pragma once

#include "InputBinding.h"
#include "InputActionBase.h"

namespace ARC
{
   template<typename InputType>
   class ActionContainer;

   template<typename InputType>
   class InputActionBuilder;

   template<typename ContainerInputType, typename... ActionInputTypes>
   class InputCompositeActionBuilder;

   template<typename InputType>
   class InputCompositeAction;

   template<typename InputType>
   class InputAction : public InputActionBase
   {
   public:
      using Callback = std::function<void()>;
      using BindingType = InputBinding<InputType>;
      using ContainerType = ActionContainer<InputType>;

      InputAction(std::shared_ptr<ContainerType> container, size_t index) : m_container(container), m_index(index) {}
      virtual ~InputAction() = default;

      static std::unique_ptr<InputAction> Create(std::shared_ptr<ContainerType> container, size_t index)
      {
         return std::unique_ptr<InputAction>(new InputAction(container, index));
      }

      void AddBinding(BindingType binding) { m_container->AddBinding(m_index, std::move(binding)); }

      void SetStartedCallback(const Callback& callback) { m_container->SetStartedCallback(m_index, callback); }
      void SetPerformedCallback(const Callback& callback) { m_container->SetPerformedCallback(m_index, callback); }
      void SetCanceledCallback(const Callback& callback) { m_container->SetCanceledCallback(m_index, callback); }

      bool IsTriggered() const override { return m_container->IsTriggered(m_index); }
      bool IsStarted() const override { return m_container->IsTriggered(m_index) && !m_container->WasTriggered(m_index); }
      bool IsCanceled() const override { return !m_container->IsTriggered(m_index) && m_container->WasTriggered(m_index); }

      const std::string& GetName() const { return m_container->GetName(m_index); }
      float GetValue() const override { return m_container->GetValue(m_index); }

      const std::vector<BindingType>& GetBindings() const { return m_container->GetBindings(m_index); }

      std::span<BindingType> GetAxisBindings() const
      {
         return FilterBindings([](const BindingType& binding) { return binding.IsAxis(); });
      }

      std::span<BindingType> GetButtonBindings() const
      {
         return FilterBindings([](const BindingType& binding) { return binding.IsButton(); });
      }

      std::shared_ptr<ContainerType> GetContainer() const { return m_container; }
      size_t GetIndex() const { return m_index; }

   protected:
      template <typename Predicate>
      std::span<BindingType> FilterBindings(Predicate predicate) const
      {
         static std::vector<BindingType> filteredBindings;
         filteredBindings.clear();

         const auto& bindings = m_container->GetBindings(m_index);
         for (const auto& binding : bindings)
         {
            if (predicate(binding))
               filteredBindings.push_back(binding);
         }

         return std::span<BindingType>(filteredBindings);
      }

      std::shared_ptr<ContainerType> m_container;
      size_t m_index;

      template<typename T>
      friend class InputActionBuilder;

      template<typename CT, typename... AT>
      friend class InputCompositeActionBuilder;

      template<typename T>
      friend class InputCompositeAction;
   };
} // namespace ARC
