#pragma once

#include "Input.h"
#include "InputActionMap.h"
#include "Util/Singleton.h"

namespace ARC
{
   class InputActionMapProxyBase
   {
   public:
      virtual ~InputActionMapProxyBase() = default;
      virtual void Update() = 0;
   };

   template<typename InputType>
   class InputActionMapProxy : public InputActionMapProxyBase
   {
   public:
      void AddActionMap(const std::shared_ptr<InputActionMap<InputType>>& actionMap)
      {
         m_actionMaps.push_back(actionMap);
      }

      void RemoveActionMap(const std::shared_ptr<InputActionMap<InputType>>& actionMap)
      {
         m_actionMaps.erase(std::remove(m_actionMaps.begin(), m_actionMaps.end(), actionMap), m_actionMaps.end());
      }

      void Update() override
      {
         for (auto& actionMap : m_actionMaps)
         {
            actionMap->Update();
         }
      }

   private:
      std::vector<std::shared_ptr<InputActionMap<InputType>>> m_actionMaps;
   };

   class InputManager : public Singleton<InputManager>
   {
   public:
      template<typename InputType>
      void AddActionMap(const std::shared_ptr<InputActionMap<InputType>>& actionMap)
      {
         auto& typedManager = GetOrCreateManager<InputType>();
         typedManager.AddActionMap(actionMap);
      }

      template<typename InputType>
      void RemoveActionMap(const std::shared_ptr<InputActionMap<InputType>>& actionMap)
      {
         if (auto it = m_managers.find(typeid(InputType)); it != m_managers.end())
         {
            if (auto manager = dynamic_cast<InputActionMapProxy<InputType>*>(it->second.get()))
            {
               manager->RemoveActionMap(actionMap);
            }
         }
      }

      void Update()
      {
         for (auto& [type, manager] : m_managers)
         {
            manager->Update();
         }
      }

   private:
      InputManager() = default;
      virtual ~InputManager() = default;

      template<typename InputType>
      InputActionMapProxy<InputType>& GetOrCreateManager()
      {
         auto& manager = m_managers[typeid(InputType)];
         if (!manager)
            manager = std::make_unique<InputActionMapProxy<InputType>>();
         return *static_cast<InputActionMapProxy<InputType>*>(manager.get());
      }

      std::unordered_map<std::type_index, std::unique_ptr<InputActionMapProxyBase>> m_managers;

      friend class Singleton<InputManager>;
   };
} // namespace ARC
