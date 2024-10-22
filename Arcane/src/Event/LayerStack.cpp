#include "arcpch.h"
#include "LayerStack.h"

ARC::LayerStack::LayerStack() : m_layerInsertIndex(0U)
{
}

ARC::LayerStack::~LayerStack()
{
	Clear();
}

void ARC::LayerStack::Push(Layer* layer)
{
	m_layers.emplace(m_layers.begin() + m_layerInsertIndex, layer);
	++m_layerInsertIndex;
}

void ARC::LayerStack::PushFront(Layer* overlay)
{
	m_layers.emplace_back(overlay);
}

void ARC::LayerStack::Pop(Layer* layer)
{
	auto it = std::find(m_layers.begin(), m_layers.end(), layer);
	if (it != m_layers.end())
	{
		m_layers.erase(it);
		--m_layerInsertIndex;
	}
}

void ARC::LayerStack::PopFront(Layer* overlay)
{
	auto it = std::find(m_layers.begin(), m_layers.end(), overlay);
	if (it != m_layers.end())
		m_layers.erase(it);
}

void ARC::LayerStack::Clear()
{
	for (Layer* layer : m_layers)
	{
		if (layer)
		{
			layer->OnDetach();
			delete layer;
		}
	}
}
