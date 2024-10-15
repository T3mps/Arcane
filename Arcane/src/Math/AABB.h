#pragma once

#include "Vector.h"

namespace ARC
{
   template  <typename T = float64_t>
   class AABB
   {
   public:
      static_assert(std::is_arithmetic<T>::value, "AABB requires a numerical type");

      static constexpr size_t ELEMENT_COUNT = 4;

      T minX;
      T minY;
      T maxX;
      T maxY;

      AABB() = default;
      AABB(T x0, T y0, T x1, T y1) :
         minX(std::min<T>(x0, x1)),
         minY(std::min<T>(y0, y1)),
         maxX(std::max<T>(x0, x1)),
         maxY(std::max<T>(y0, y1))
      {}
            
      AABB<T> operator+(const AABB<T>& rhs) const { return AABB<T>(minX + rhs.minX, minY + rhs.minY, maxX + rhs.maxX, maxY + rhs.maxY); }
      AABB<T> operator+(T scalar) const { return AABB<T>(minX + scalar, minY + scalar, maxX + scalar, maxY + scalar); }
      AABB<T> operator-(const AABB<T>& rhs) const { return AABB<T>(minX - rhs.minX, minY - rhs.minY, maxX - rhs.maxX, maxY - rhs.maxY); }
      AABB<T> operator-(T scalar) const { return AABB<T>(minX - scalar, minY - scalar, maxX - scalar, maxY - scalar); }
      AABB<T> operator*(const AABB<T>& rhs) const { return AABB<T>(minX * rhs.minX, minY * rhs.minY, maxX * rhs.maxX, maxY * rhs.maxY); }
      AABB<T> operator*(T scalar) const { return AABB<T>(minX * scalar, minY * scalar, maxX * scalar, maxY * scalar); }
      AABB<T> operator/(const AABB<T>& rhs) const { return AABB<T>(minX / rhs.minX, minY / rhs.minY, maxX / rhs.maxX, maxY / rhs.maxY); }
      AABB<T> operator/(T scalar) const { return AABB<T>(minX / scalar, minY / scalar, maxX / scalar, maxY / scalar); }

      AABB<T>& operator+=(const AABB<T>& rhs)
      {
         minX += rhs.minX;
         minY += rhs.minY;
         maxX += rhs.maxX;
         maxY += rhs.maxY;
         return *this;
      }

      AABB<T>& operator+=(T scalar)
      {
         minX += scalar;
         minY += scalar;
         maxX += scalar;
         maxY += scalar;
         return *this;
      }

      AABB<T>& operator-=(const AABB<T>& rhs)
      {
         minX -= rhs.minX;
         minY -= rhs.minY;
         maxX -= rhs.maxX;
         maxY -= rhs.maxY;
         return *this;
      }

      AABB<T>& operator-=(T scalar)
      {
         minX -= scalar;
         minY -= scalar;
         maxX -= scalar;
         maxY -= scalar;
         return *this;
      }

      AABB<T>& operator*=(const AABB<T>& rhs)
      {
         minX *= rhs.minX;
         minY *= rhs.minY;
         maxX *= rhs.maxX;
         maxY *= rhs.maxY;
         return *this;
      }

      AABB<T>& operator*=(T scalar)
      {
         minX *= scalar;
         minY *= scalar;
         maxX *= scalar;
         maxY *= scalar;
         return *this;
      }

      AABB<T>& operator/=(const AABB<T>& rhs)
      {
         minX /= rhs.minX;
         minY /= rhs.minY;
         maxX /= rhs.maxX;
         maxY /= rhs.maxY;
         return *this;
      }

      AABB<T>& operator/=(T scalar)
      {
         minX /= scalar;
         minY /= scalar;
         maxX /= scalar;
         maxY /= scalar;
         return *this;
      }

      AABB<T> operator-() { return AABB<T>(-minX, -minY, -maxX, -maxY); }

      T operator[](int32_t index)
      {
         switch (index)
         {
            case 0: return minX;
            case 1: return minY;
            case 2: return maxX;
            case 3: return maxY;
            default: return -1;
         }
      }

      const T operator[](int32_t index) const
      {
         switch (index)
         {
            case 0: return minX;
            case 1: return minY;
            case 2: return maxX;
            case 3: return maxY;
            default: return -1;
         }
      }

      bool operator==(const AABB<T>& rhs) const
      {
         if constexpr (std::is_integral<T>::value)
         {
            return minX == rhs.minX && minY == rhs.minY &&
                   maxX == rhs.maxX && maxY == rhs.maxY;
         }
         else
         {
            const T epsilon = static_cast<T>(1e-5);
            return std::abs(minX - rhs.minX) < epsilon && std::abs(minY - rhs.minY) < epsilon &&
                   std::abs(maxX - rhs.maxX) < epsilon && std::abs(maxY - rhs.maxY) < epsilon;
         }
      }
      bool operator!=(const AABB<T>& rhs) const { return !(*this == rhs); }

      void Set(T minXVal, T minYVal, T maxXVal, T maxYVal)
      {
         minX = minXVal;
         minY = minYVal;
         maxX = maxXVal;
         maxY = maxYVal;
      }

      AABB<T>& Negate()
      {
         minX = -minX;
         minY = -minY;
         maxX = -maxX;
         maxY = -maxY;
         return *this;
      }

      void Translate(T x, T y)
      {
         minX += x;
         maxX += x;
         minY += y;
         maxY += y;
      }

      void Translate(const Vector2<T>& translation) { Translate(translation.x(), translation.y()); }

      AABB<T>& Union(const AABB<T>& aabb)
      {
         minX = std::min<T>(minX, aabb.minX);
         minY = std::min<T>(minY, aabb.minY);
         maxX = std::max<T>(maxX, aabb.maxX);
         maxY = std::max<T>(maxY, aabb.maxY);
         return *this;
      }

      AABB<T>& Union(const AABB<T>& aabbA, const AABB<T>& aabbB)
      {
         minX = std::min<T>(aabbA.minX, aabbB.minX);
         minY = std::min<T>(aabbA.minY, aabbB.minY);
         maxX = std::max<T>(aabbA.maxX, aabbB.maxX);
         maxY = std::max<T>(aabbA.maxY, aabbB.maxY);
         return *this;
      }

      AABB<T>& Intersection(const AABB<T>& aabb)
      {
         minX = std::max<T>(minX, aabb.minX);
         minY = std::max<T>(minY, aabb.minY);
         maxX = std::min<T>(maxX, aabb.maxX);
         maxY = std::min<T>(maxY, aabb.maxY);

         if (minX > maxX || minY > maxY)
         {
            minX = 0;
            minY = 0;
            maxX = 0;
            maxY = 0;
         }

         return *this;
      }

      AABB<T>& Intersection(const AABB<T>& aabbA, const AABB<T>& aabbB)
      {
         minX = std::max<T>(aabbA.minX, aabbB.minX);
         minY = std::max<T>(aabbA.minY, aabbB.minY);
         maxX = std::min<T>(aabbA.maxX, aabbB.maxX);
         maxY = std::min<T>(aabbA.maxY, aabbB.maxY);

         if (minX > maxX || minY > maxY)
         {
            minX = 0;
            minY = 0;
            maxX = 0;
            maxY = 0;
         }

         return *this;
      }

      bool Intersects(const AABB<T>& other) const
      {
         return !(maxX < other.minX || minX > other.maxX ||
                  maxY < other.minY || minY > other.maxY);
      }

      constexpr T GetWidth() const { return maxX - minX; }
      constexpr T GetHeight() const { return maxX - minX; }
      
      constexpr T GetPerimeter() const { return 2 * (maxX - minX + maxY - minY); }
      constexpr T GetArea() const { return (maxX - minX) * (maxY - minY); }

      static AABB<T> Zero() { return AABB<T>(0, 0, 0, 0); }
      static AABB<T> One() { return AABB<T>(0, 0, 1, 1); }
   };

   using AABBi = AABB<int32_t>;
   using AABBf = AABB<float32_t>;
   using AABBd = AABB<float64_t>;
} // namespace ARC

namespace std
{
   template  <typename T>
   struct tuple_size<ARC::AABB<T>> : integral_constant<size_t, ARC::AABB<T>::ELEMENT_COUNT> {};

   template  <size_t N, typename T>
   struct tuple_element<N, ARC::AABB<T>> { using type = T; };

   template  <size_t N, typename T>
   constexpr T& get(ARC::AABB<T>& aabb)
   {
      static_assert(N < ARC::AABB<T>::ELEMENT_COUNT, "Index out of bounds");
      if constexpr (N == 0)
         return aabb.minX;
      else if constexpr (N == 1)
         return aabb.minY;
      else if constexpr (N == 2)
         return aabb.maxX;
      else return aabb.maxY;
   }

   template  <size_t N, typename T>
   constexpr const T& get(const ARC::AABB<T>& aabb)
   {
      static_assert(N < ARC::AABB<T>::ELEMENT_COUNT, "Index out of bounds");
      if constexpr (N == 0)
         return aabb.minX;
      else if constexpr (N == 1)
         return aabb.minY;
      else if constexpr (N == 2)
         return aabb.maxX;
      else return aabb.maxY;
   }
} // namespace std
