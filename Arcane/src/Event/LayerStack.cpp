#include "arcpch.h"
#include "LayerStack.h"

ARC::LayerStack::LayerStack() : m_layerInsertIndex(0U)
{
}

ARC::LayerStack::~LayerStack()
{
	Clear();
}

void ARC::LayerStack::Clear()
{
   std::scoped_lock lock(m_mutex);
   m_layers.clear();
   m_layerInsertIndex = 0;
}
