#pragma once

#include "InputActionBuilder.h"
#include "InputActionMap.h"
#include "InputCompositeBuilder.h"
#include "InputManager.h"
#include "KeyCode.h"
#include "Util/Singleton.h"

namespace ARC
{
   template<typename T> class InputAction;
   template<typename T> class InputActionMap;
   template<typename T> class InputBinding;

   class InputActionSystem : public Singleton<InputActionSystem>
   {
   public:
      template<typename T>
      class ActionMapBuilder
      {
      public:
         class ActionBuilder
         {
         public:
            ActionBuilder& Key(T key)
            {
               m_action.WithBinding(InputBinding<T>::CreateButton(key));
               return *this;
            }

            ActionBuilder& Axis(T positive, T negative, float scale = 1.0f)
            {
               m_action.WithBinding(InputBinding<T>::CreateAxis(positive, negative, scale));
               return *this;
            }

            ActionBuilder& Key(T key, ModifierKey modifier)
            {
               m_action.WithBinding(InputBinding<T>::CreateButton(key, modifier));
               return *this;
            }

            ActionBuilder& Axis(T positive, T negative, float scale, ModifierKey modifier)
            {
               m_action.WithBinding(InputBinding<T>::CreateAxis(positive, negative, scale, modifier, modifier));
               return *this;
            }

            ActionBuilder& OnPress(std::function<void()> callback)
            {
               m_action.OnStarted(callback);
               return *this;
            }

            ActionBuilder& OnRelease(std::function<void()> callback)
            {
               m_action.OnCanceled(callback);
               return *this;
            }

            ActionBuilder& WhileHeld(std::function<void()> callback)
            {
               m_action.OnPerformed(callback);
               return *this;
            }

            ActionBuilder& OnChange(std::function<void(float)> callback)
            {
               m_action.OnPerformed([callback](const InputAction<T>& action)
               {
                  callback(action.GetValue());
               });
               return *this;
            }

            ActionBuilder& OnUpdate(std::function<void(const InputAction<T>&)> callback)
            {
               m_action.OnPerformed(callback);
               return *this;
            }

            std::shared_ptr<InputAction<T>> Build()
            {
               auto action = m_action.Build(m_map->GetContainer());
               m_map->AddAction(action);
               return action;
            }

            operator std::shared_ptr<InputAction<T>>()
            {
               return Build();
            }

         protected:
            friend class ActionMapBuilder;
            ActionBuilder(InputActionBuilder<T>&& builder, std::shared_ptr<InputActionMap<T>>& mapRef)  :
               m_action(std::move(builder)),
               m_map(mapRef)
            {}

            InputActionBuilder<T> m_action;
            std::shared_ptr<InputActionMap<T>>& m_map;
         };

         template<typename... ActionTypes>
         auto Sequence(const std::string& name, const std::shared_ptr<InputAction<ActionTypes>>&... actions)
         {
            return InputCompositeActionBuilder<T, ActionTypes...>::CreateSequence(name, actions...);
         }

         template<typename... ActionTypes>
         auto Concurrent(const std::string& name, const std::shared_ptr<InputAction<ActionTypes>>&... actions)
         {
            return InputCompositeActionBuilder<T, ActionTypes...>::CreateConcurrent(name, actions...);
         }

         template<typename... ActionTypes>
         auto OneAfterAnother(const std::string& name, const std::shared_ptr<InputAction<ActionTypes>>&... actions)
         {
            return InputCompositeActionBuilder<T, ActionTypes...>::CreateOneAfterAnother(name, actions...);
         }

         ActionBuilder Button(const std::string& name)
         {
            return ActionBuilder(InputActionBuilder<T>::CreateButtonAction(name), m_map);
         }

         ActionBuilder Value(const std::string& name)
         {
            return ActionBuilder(InputActionBuilder<T>::CreateValueAction(name), m_map);
         }

         operator std::shared_ptr<InputActionMap<T>>() { return m_map; }
         std::shared_ptr<InputActionMap<T>> Data() { return m_map; }

      private:
         friend class InputActionSystem;
         ActionMapBuilder(const std::string& name) : m_map(std::make_shared<InputActionMap<T>>(name))
         {
            m_map->Enable();
            InputManager::GetInstance().AddActionMap(m_map);
         }

         std::shared_ptr<InputActionMap<T>> m_map;
      };

      template<typename T>
      ActionMapBuilder<T> CreateMap(const std::string& name)
      {
         return ActionMapBuilder<T>(name);
      }

   private:
      InputActionSystem() = default;
      virtual ~InputActionSystem() = default;
      friend class Singleton<InputActionSystem>;
   };
} // namespace ARC