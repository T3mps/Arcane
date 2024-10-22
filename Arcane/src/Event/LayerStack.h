#pragma once

#include "Layer.h"

namespace ARC
{
	class LayerStack
	{
	public:
		LayerStack();
		~LayerStack();

		Layer* operator[](size_t index)
		{
			ARC_ASSERT(index >= 0 && index < m_layers.size(), "Index out of bounds.");
			return m_layers[index];
		}

		const Layer* operator[](size_t index) const
		{
			ARC_ASSERT(index >= 0 && index < m_layers.size(), "Index out of bounds.");
			return m_layers[index];
		}

		void Push(Layer* layer);
		void PushFront(Layer* overlay);
		void Pop(Layer* layer);
		void PopFront(Layer* overlay);

		void Clear();

		size_t Size() const { return m_layers.size(); }

		std::vector<Layer*>::iterator begin() { return m_layers.begin(); }
		std::vector<Layer*>::iterator end() { return m_layers.end(); }

	private:
		std::vector<Layer*> m_layers;
		uint32_t m_layerInsertIndex;
	};
} // namespace ARC
