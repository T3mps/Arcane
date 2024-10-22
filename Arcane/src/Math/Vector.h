#pragma once

#include "GLMType.h"
#include "glm/geometric.hpp"
#include "glm/vec2.hpp"
#include "glm/vec3.hpp"
#include "glm/vec4.hpp"

namespace ARC
{
   template <typename T>
   class Vector2 : public GLMType<glm::vec<2, T>>
   {
   public:
      Vector2() : m_glmVector(0, 0) {}
      Vector2(T x, T y) : m_glmVector(x, y) {}
      Vector2(glm::vec<2, T> glm) : m_glmVector(glm) {}
      
      ~Vector2() = default;

      T& x() { return m_glmVector.x; }
      T& y() { return m_glmVector.y; }
      const T& x() const { return m_glmVector.x; }
      const T& y() const { return m_glmVector.y; }

      Vector2<T> operator+(const Vector2<T>& rhs) const { return Vector2<T>(this->m_glmVector + rhs.m_glmVector); }
      Vector2<T> operator+(T scalar) const { return Vector2<T>(this->m_glmVector + scalar); }
      Vector2<T> operator-(const Vector2<T>& rhs) const { return Vector2<T>(this->m_glmVector - rhs.m_glmVector); }
      Vector2<T> operator-(T scalar) const { return Vector2<T>(this->m_glmVector - scalar); }
      Vector2<T> operator*(const Vector2<T>& rhs) const { return Vector2<T>(this->m_glmVector * rhs.m_glmVector); }
      Vector2<T> operator*(T scalar) const { return Vector2<T>(this->m_glmVector * scalar); }
      Vector2<T> operator/(const Vector2<T>& rhs) const { return Vector2<T>(this->m_glmVector / rhs.m_glmVector); }
      Vector2<T> operator/(T scalar) const { return Vector2<T>(this->m_glmVector / scalar); }

      Vector2<T>& operator+=(const Vector2<T>& rhs) { this->m_glmVector += rhs.m_glmVector; return *this; }
      Vector2<T>& operator+=(T scalar) { this->m_glmVector += scalar; return *this; }
      Vector2<T>& operator-=(const Vector2<T>& rhs) { this->m_glmVector -= rhs.m_glmVector; return *this; }
      Vector2<T>& operator-=(T scalar) { this->m_glmVector -= scalar; return *this; }
      Vector2<T>& operator*=(const Vector2<T>& rhs) { this->m_glmVector *= rhs.m_glmVector; return *this; }
      Vector2<T>& operator*=(T scalar) { this->m_glmVector *= scalar; return *this; }
      Vector2<T>& operator/=(const Vector2<T>& rhs) { this->m_glmVector /= rhs.m_glmVector; return *this; }
      Vector2<T>& operator/=(T scalar) { this->m_glmVector /= scalar; return *this; }

      Vector2<T> operator-() { return Vector2<T>(-this->m_glmVector); }

      T operator[](int32_t index) { return this->m_glmVector[index]; }
      const T operator[](int32_t index) const { return this->m_glmVector[index]; }

      bool operator==(const Vector2<T>& rhs) const { return this->m_glmVector == rhs.m_glmVector; }
      bool operator!=(const Vector2<T>& rhs) const { return this->m_glmVector != rhs.m_glmVector; }

      T Dot(const Vector2<T>& rhs) const { return glm::dot(this->m_glmVector, rhs.m_glmVector); }
      
      T Magnitude() const { return glm::length(this->m_glmVector); }

      Vector2<T> Normalize() const { return Vector2<T>(glm::normalize(this->m_glmVector)); }

      T Distance(const Vector2<T>& rhs) const { return glm::distance(this->m_glmVector, rhs.m_glmVector); }
      
      glm::vec<2, T>& Data() override { return this->m_glmVector; }
      const glm::vec<2, T>& Data() const override { return this->m_glmVector; }

   private:
      glm::vec<2, T> m_glmVector;
   };
   
   using Vector2i = Vector2<int32_t>;
   using Vector2u = Vector2<uint32_t>;
   using Vector2f = Vector2<float32_t>;
   using Vector2d = Vector2<float64_t>;

   template <typename T>
   class Vector3 : public GLMType<glm::vec<3, T>>
   {
   public:
      Vector3() : m_glmVector(0, 0, 0) {}
      Vector3(T x, T y, T z) : m_glmVector(x, y, z) {}
      Vector3(glm::vec<3, T> glm) : m_glmVector(glm) {}

      ~Vector3() = default;

      T& x() { return m_glmVector.x; }
      T& y() { return m_glmVector.y; }
      T& z() { return m_glmVector.z; }
      const T& x() const { return m_glmVector.x; }
      const T& y() const { return m_glmVector.y; }
      const T& z() const { return m_glmVector.z; }

      Vector3<T> operator+(const Vector3<T>& rhs) const { return Vector3<T>(this->m_glmVector + rhs.m_glmVector); }
      Vector3<T> operator+(T scalar) const { return Vector3<T>(this->m_glmVector + scalar); }
      Vector3<T> operator-(const Vector3<T>& rhs) const { return Vector3<T>(this->m_glmVector - rhs.m_glmVector); }
      Vector3<T> operator-(T scalar) const { return Vector3<T>(this->m_glmVector - scalar); }
      Vector3<T> operator*(const Vector3<T>& rhs) const { return Vector3<T>(this->m_glmVector * rhs.m_glmVector); }
      Vector3<T> operator*(T scalar) const { return Vector3<T>(this->m_glmVector * scalar); }
      Vector3<T> operator/(const Vector3<T>& rhs) const { return Vector3<T>(this->m_glmVector / rhs.m_glmVector); }
      Vector3<T> operator/(T scalar) const { return Vector3<T>(this->m_glmVector / scalar); }

      Vector3<T>& operator+=(const Vector3<T>& rhs) { this->m_glmVector += rhs.m_glmVector; return *this; }
      Vector3<T>& operator+=(T scalar) { this->m_glmVector += scalar; return *this; }
      Vector3<T>& operator-=(const Vector3<T>& rhs) { this->m_glmVector -= rhs.m_glmVector; return *this; }
      Vector3<T>& operator-=(T scalar) { this->m_glmVector -= scalar; return *this; }
      Vector3<T>& operator*=(const Vector3<T>& rhs) { this->m_glmVector *= rhs.m_glmVector; return *this; }
      Vector3<T>& operator*=(T scalar) { this->m_glmVector *= scalar; return *this; }
      Vector3<T>& operator/=(const Vector3<T>& rhs) { this->m_glmVector /= rhs.m_glmVector; return *this; }
      Vector3<T>& operator/=(T scalar) { this->m_glmVector /= scalar; return *this; }

      Vector3<T> operator-() { return Vector3<T>(-this->m_glmVector); }

      T operator[](int32_t index) { return this->m_glmVector[index]; }
      const T operator[](int32_t index) const { return this->m_glmVector[index]; }

      bool operator==(const Vector3<T>& rhs) const { return this->m_glmVector == rhs.m_glmVector; }
      bool operator!=(const Vector3<T>& rhs) const { return this->m_glmVector != rhs.m_glmVector; }

      T Dot(const Vector3<T>& rhs) const { return glm::dot(this->m_glmVector, rhs.m_glmVector); }
      Vector3<T> Cross(const Vector3<T>& rhs) const { return Vector3<T>(glm::cross(this->m_glmVector, rhs.m_glmVector)); }

      T Magnitude() const { return glm::length(this->m_glmVector); }

      Vector3<T> Normalize() const { return Vector3<T>(glm::normalize(this->m_glmVector)); }

      T Distance(const Vector3<T>& rhs) const { return glm::distance(this->m_glmVector, rhs.m_glmVector); }

      glm::vec<3, T>& Data() override { return this->m_glmVector; }
      const glm::vec<3, T>& Data() const override { return this->m_glmVector; }

   private:
      glm::vec<3, T> m_glmVector;
   };

   using Vector3i = Vector3<int32_t>;
   using Vector3u = Vector3<uint32_t>;
   using Vector3f = Vector3<float32_t>;
   using Vector3d = Vector3<float64_t>;

   template <typename T>
   class Vector4 : public GLMType<glm::vec<4, T>>
   {
   public:
      Vector4() : m_glmVector(0, 0, 0, 0) {}
      Vector4(T x, T y, T z, T w) : m_glmVector(x, y, z, w) {}
      Vector4(glm::vec<4, T> glm) : m_glmVector(glm) {}
      
      ~Vector4() = default;

      T& x() { return m_glmVector.x; }
      T& y() { return m_glmVector.y; }
      T& z() { return m_glmVector.z; }
      T& w() { return m_glmVector.w; }
      const T& x() const { return m_glmVector.x; }
      const T& y() const { return m_glmVector.y; }
      const T& z() const { return m_glmVector.z; }
      const T& w() const { return m_glmVector.w; }
      
      Vector4<T> operator+(const Vector4<T>& rhs) const { return Vector4<T>(this->m_glmVector + rhs.m_glmVector); }
      Vector4<T> operator+(T scalar) const { return Vector4<T>(this->m_glmVector + scalar); }
      Vector4<T> operator-(const Vector4<T>& rhs) const { return Vector4<T>(this->m_glmVector - rhs.m_glmVector); }
      Vector4<T> operator-(T scalar) const { return Vector4<T>(this->m_glmVector - scalar); }
      Vector4<T> operator*(const Vector4<T>& rhs) const { return Vector4<T>(this->m_glmVector * rhs.m_glmVector); }
      Vector4<T> operator*(T scalar) const { return Vector4<T>(this->m_glmVector * scalar); }
      Vector4<T> operator/(const Vector4<T>& rhs) const { return Vector4<T>(this->m_glmVector / rhs.m_glmVector); }
      Vector4<T> operator/(T scalar) const { return Vector4<T>(this->m_glmVector / scalar); }

      Vector4<T>& operator+=(const Vector4<T>& rhs) { this->m_glmVector += rhs.m_glmVector; return *this; }
      Vector4<T>& operator+=(T scalar) { this->m_glmVector += scalar; return *this; }
      Vector4<T>& operator-=(const Vector4<T>& rhs) { this->m_glmVector -= rhs.m_glmVector; return *this; }
      Vector4<T>& operator-=(T scalar) { this->m_glmVector -= scalar; return *this; }
      Vector4<T>& operator*=(const Vector4<T>& rhs) { this->m_glmVector *= rhs.m_glmVector; return *this; }
      Vector4<T>& operator*=(T scalar) { this->m_glmVector *= scalar; return *this; }
      Vector4<T>& operator/=(const Vector4<T>& rhs) { this->m_glmVector /= rhs.m_glmVector; return *this; }
      Vector4<T>& operator/=(T scalar) { this->m_glmVector /= scalar; return *this; }

      Vector4<T> operator-() { return Vector4<T>(-this->m_glmVector); }

      T operator[](int32_t index) { return this->m_glmVector[index]; }
      const T operator[](int32_t index) const { return this->m_glmVector[index]; }

      bool operator==(const Vector4<T>& rhs) const { return this->m_glmVector == rhs.m_glmVector; }
      bool operator!=(const Vector4<T>& rhs) const { return this->m_glmVector != rhs.m_glmVector; }
      
      T Magnitude() const { return glm::length(this->m_glmVector); }

      Vector4<T> Normalize() const { return Vector4<T>(glm::normalize(this->m_glmVector)); }

      T Distance(const Vector4<T>& rhs) const { return glm::distance(this->m_glmVector, rhs.m_glmVector); }

      glm::vec<4, T>& Data() override { return this->m_glmVector; }
      const glm::vec<4, T>& Data() const override { return this->m_glmVector; }

   private:
      glm::vec<4, T> m_glmVector;
   };

   using Vector4i = Vector4<int32_t>;
   using Vector4u = Vector4<uint32_t>;
   using Vector4f = Vector4<float32_t>;
   using Vector4d = Vector4<float64_t>;
} // namespace ARC
