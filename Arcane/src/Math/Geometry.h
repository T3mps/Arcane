#pragma once

#include <algorithm>
#include <cmath>

#include "Common.h"

namespace Arcane
{
   struct Rectangle
   {
      float x, y, width, height;

      Rectangle(float x = 0.0f, float y = 0.0f, float width = 0.0f, float height = 0.0f) :
         x(x),
         y(y),
         width(width),
         height(height)
      {}

      bool Contains(float px, float py) const
      {
         return px >= x && px <= x + width &&
                py >= y && py <= y + height;
      }

      bool Contains(const Rectangle& other) const
      {
         return other.x >= x && other.x + other.width <= x + width &&
                other.y >= y && other.y + other.height <= y + height;
      }

      bool Intersects(const Rectangle& other) const
      {
         return x < other.x + other.width && x + width > other.x &&
                y < other.y + other.height && y + height > other.y;
      }

      void Translate(float dx, float dy)
      {
         x += dx;
         y += dy;
      }

      void Expand(float dw, float dh)
      {
         x -= dw;
         y -= dh;
         width += 2 * dw;
         height += 2 * dh;
      }

      bool operator==(const Rectangle& other) const
      {
         return std::abs(x - other.x)         < 0.001f && std::abs(y - other.y) < 0.001f &&
                std::abs(width - other.width) < 0.001f && std::abs(height - other.height) < 0.001f;
      }
      bool operator!=(const Rectangle& other) const { return !(*this == other); }
   };
}