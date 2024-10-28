#include "arcpch.h"
#include "Geometry.h"

void ARC::Rectangle::Translate(float dx, float dy)
{
   x += dx;
   y += dy;
}

void ARC::Rectangle::Expand(float dw, float dh)
{
   x -= dw;
   y -= dh;
   width += 2 * dw;
   height += 2 * dh;
}
