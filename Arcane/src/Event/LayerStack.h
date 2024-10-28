#pragma once

#include "Layer.h"

namespace ARC
{
   class LayerStack
   {
   public:
      using iterator = std::vector<std::unique_ptr<Layer>>::iterator;
      using const_iterator = std::vector<std::unique_ptr<Layer>>::const_iterator;
      using reverse_iterator = std::vector<std::unique_ptr<Layer>>::reverse_iterator;
      using const_reverse_iterator = std::vector<std::unique_ptr<Layer>>::const_reverse_iterator;

      LayerStack();
      ~LayerStack();

      template <typename Layer>
      void Push(std::unique_ptr<Layer> layer)
      {
         std::scoped_lock lock(m_mutex);
         m_layers.emplace(m_layers.begin() + m_layerInsertIndex, std::move(layer));
         ++m_layerInsertIndex;
      }
      
      template <typename Layer>
      void PushFront(std::unique_ptr<Layer> overlay)
      {
         std::scoped_lock lock(m_mutex);
         m_layers.emplace_back(std::move(overlay));
      }

      template <typename Layer>
      std::unique_ptr<Layer> Pop(Layer* layer)
      {
         std::scoped_lock lock(m_mutex);
         auto it = std::find_if(m_layers.begin(), m_layers.begin() + m_layerInsertIndex,
            [layer](const std::unique_ptr<Layer>& l) { return l.get() == layer; });
         if (it != m_layers.begin() + m_layerInsertIndex)
         {
            std::unique_ptr<Layer> removedLayer = std::move(*it);
            m_layers.erase(it);
            --m_layerInsertIndex;
            return removedLayer;
         }
         return nullptr;
      }
      
      template <typename Layer>
      std::unique_ptr<Layer> PopFront(Layer* overlay)
      {
         std::scoped_lock lock(m_mutex);
         auto it = std::find_if(m_layers.begin() + m_layerInsertIndex, m_layers.end(),
            [overlay](const std::unique_ptr<Layer>& l) { return l.get() == overlay; });
         if (it != m_layers.end())
         {
            std::unique_ptr<Layer> removedOverlay = std::move(*it);
            m_layers.erase(it);
            return removedOverlay;
         }
         return nullptr;
      }

      void Clear();

      size_t Size() const { return m_layers.size(); }

      iterator begin() { return m_layers.begin(); }
      iterator end() { return m_layers.end(); }
      const_iterator begin() const { return m_layers.begin(); }
      const_iterator end() const { return m_layers.end(); }
      reverse_iterator rbegin() { return m_layers.rbegin(); }
      reverse_iterator rend() { return m_layers.rend(); }
      const_reverse_iterator rbegin() const { return m_layers.rbegin(); }
      const_reverse_iterator rend() const { return m_layers.rend(); }

   private:
      std::vector<std::unique_ptr<Layer>> m_layers;
      uint32_t m_layerInsertIndex = 0;
      std::mutex m_mutex;
   };
} // namespace ARC
