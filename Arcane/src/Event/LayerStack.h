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

      void Push(std::unique_ptr<Layer> layer);
      void PushFront(std::unique_ptr<Layer> overlay);
      std::unique_ptr<Layer> Pop(Layer* layer);
      std::unique_ptr<Layer> PopFront(Layer* overlay);

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
