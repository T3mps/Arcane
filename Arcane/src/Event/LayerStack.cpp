#include "arcpch.h"
#include "LayerStack.h"

ARC::LayerStack::LayerStack() : m_layerInsertIndex(0U)
{
}

ARC::LayerStack::~LayerStack()
{
	Clear();
}

void ARC::LayerStack::Push(std::unique_ptr<Layer> layer)
{
   std::scoped_lock lock(m_mutex);
   m_layers.emplace(m_layers.begin() + m_layerInsertIndex, std::move(layer));
   ++m_layerInsertIndex;
}

void ARC::LayerStack::PushFront(std::unique_ptr<Layer> overlay)
{
   std::scoped_lock lock(m_mutex);
   m_layers.emplace_back(std::move(overlay));
}

std::unique_ptr<ARC::Layer> ARC::LayerStack::Pop(Layer* layer)
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

std::unique_ptr<ARC::Layer> ARC::LayerStack::PopFront(Layer* overlay)
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

void ARC::LayerStack::Clear()
{
   std::scoped_lock lock(m_mutex);
   m_layers.clear();
   m_layerInsertIndex = 0;
}
