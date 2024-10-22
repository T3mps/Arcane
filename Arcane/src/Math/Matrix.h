#pragma once

#include "GLMType.h"
#include "glm/gtc/matrix_transform.hpp"
#include "glm/mat2x2.hpp"
#include "glm/mat2x3.hpp"
#include "glm/mat2x4.hpp"
#include "glm/mat3x2.hpp"
#include "glm/mat3x3.hpp"
#include "glm/mat3x4.hpp"
#include "glm/mat4x2.hpp"
#include "glm/mat4x3.hpp"
#include "glm/mat4x4.hpp"
#include "glm/matrix.hpp"

namespace ARC
{
   template <typename T>
   class Matrix2x2 : public GLMType<glm::mat<2, 2, T>>
   {
   public:
      Matrix2x2<T>() : m_glmMatrix(static_cast<T>(1)) {}
      Matrix2x2<T>(T a, T b, T c, T d) : m_glmMatrix(a, b, c, d) {}
      Matrix2x2<T>(const glm::mat<2, 2, T>& mat) : m_glmMatrix(mat) {}

      Matrix2x2<T> operator+(const Matrix2x2<T>& rhs) const { return Matrix2x2<T>(m_glmMatrix + rhs.m_glmMatrix); }
      Matrix2x2<T> operator-(const Matrix2x2<T>& rhs) const { return Matrix2x2<T>(m_glmMatrix - rhs.m_glmMatrix); }
      Matrix2x2<T> operator*(const Matrix2x2<T>& rhs) const { return Matrix2x2<T>(m_glmMatrix * rhs.m_glmMatrix); }
      Matrix2x2<T> operator*(T scalar) const { return Matrix2x2<T>(m_glmMatrix * scalar); }

      Matrix2x2<T>& operator+=(const Matrix2x2<T>& rhs) { m_glmMatrix += rhs.m_glmMatrix; return *this; }
      Matrix2x2<T>& operator-=(const Matrix2x2<T>& rhs) { m_glmMatrix -= rhs.m_glmMatrix; return *this; }
      Matrix2x2<T>& operator*=(const Matrix2x2<T>& rhs) { m_glmMatrix *= rhs.m_glmMatrix; return *this; }
      Matrix2x2<T>& operator*=(T scalar) { m_glmMatrix *= scalar; return *this; }

      Matrix2x2<T> operator-() { return Matrix2x2<T>(-m_glmMatrix); }

      T& operator[](int32_t index) { return m_glmMatrix[index]; }
      const T& operator[](int32_t index) const { return m_glmMatrix[index]; }

      bool operator==(const Matrix2x2<T>& rhs) const { return m_glmMatrix == rhs.m_glmMatrix; }
      bool operator!=(const Matrix2x2<T>& rhs) const { return m_glmMatrix != rhs.m_glmMatrix; }

      Matrix2x2<T> Transpose() const { return Matrix2x2<T>(glm::transpose(m_glmMatrix)); }
      Matrix2x2<T> Inverse() const { return Matrix2x2<T>(glm::inverse(m_glmMatrix)); }

      T Determinant() const { return glm::determinant(m_glmMatrix); }

      glm::mat<2, 2, T>& Data() override { return m_glmMatrix; }
      const glm::mat<2, 2, T>& Data() const override { return m_glmMatrix; }

   private:
      glm::mat<2, 2, T> m_glmMatrix;
   };

   using Matrix2x2i = Matrix2x2<int32_t>;
   using Matrix2x2u = Matrix2x2<uint32_t>;
   using Matrix2x2f = Matrix2x2<float32_t>;
   using Matrix2x2d = Matrix2x2<float64_t>;

   template <typename T>
   class Matrix2x3 : public GLMType<glm::mat<2, 3, T>>
   {
   public:
      Matrix2x3<T>() : m_glmMatrix(static_cast<T>(1)) {}
      Matrix2x3<T>(T a, T b, T c, T d, T e, T f) : m_glmMatrix(a, b, c, d, e, f) {}
      Matrix2x3<T>(const glm::mat<2, 3, T>& mat) : m_glmMatrix(mat) {}

      Matrix2x3<T> operator+(const Matrix2x3<T>& rhs) const { return Matrix2x3<T>(m_glmMatrix + rhs.m_glmMatrix); }
      Matrix2x3<T> operator-(const Matrix2x3<T>& rhs) const { return Matrix2x3<T>(m_glmMatrix - rhs.m_glmMatrix); }
      Matrix2x3<T> operator*(const Matrix2x3<T>& rhs) const { return Matrix2x3<T>(m_glmMatrix * rhs.m_glmMatrix); }
      Matrix2x3<T> operator*(T scalar) const { return Matrix2x3<T>(m_glmMatrix * scalar); }

      Matrix2x3<T>& operator+=(const Matrix2x3<T>& rhs) { m_glmMatrix += rhs.m_glmMatrix; return *this; }
      Matrix2x3<T>& operator-=(const Matrix2x3<T>& rhs) { m_glmMatrix -= rhs.m_glmMatrix; return *this; }
      Matrix2x3<T>& operator*=(const Matrix2x3<T>& rhs) { m_glmMatrix *= rhs.m_glmMatrix; return *this; }
      Matrix2x3<T>& operator*=(T scalar) { m_glmMatrix *= scalar; return *this; }

      Matrix2x3<T> operator-() const { return Matrix2x3<T>(-m_glmMatrix); }

      T& operator[](int32_t index) { return m_glmMatrix[index]; }
      const T& operator[](int32_t index) const { return m_glmMatrix[index]; }

      bool operator==(const Matrix2x3<T>& rhs) const { return m_glmMatrix == rhs.m_glmMatrix; }
      bool operator!=(const Matrix2x3<T>& rhs) const { return m_glmMatrix != rhs.m_glmMatrix; }

      Matrix2x3<T> Transpose() const { return Matrix2x3<T>(glm::transpose(m_glmMatrix)); }
      Matrix2x3<T> Inverse() const { return Matrix2x3<T>(glm::inverse(m_glmMatrix)); }

      T Determinant() const { return glm::determinant(m_glmMatrix); }

      glm::mat<2, 3, T>& Data() override { return m_glmMatrix; }
      const glm::mat<2, 3, T>& Data() const override { return m_glmMatrix; }
      
   private:
      glm::mat<2, 3, T> m_glmMatrix;
   };
   
   using Matrix2x3i = Matrix2x3<int32_t>;
   using Matrix2x3u = Matrix2x3<uint32_t>;
   using Matrix2x3f = Matrix2x3<float32_t>;
   using Matrix2x3d = Matrix2x3<float64_t>;

   template <typename T>
   class Matrix2x4 : public GLMType<glm::mat<2, 4, T>>
   {
   public:
      Matrix2x4<T>() : m_glmMatrix(static_cast<T>(1)) {}
      Matrix2x4<T>(T a, T b, T c, T d, T e, T f, T g, T h) : m_glmMatrix(a, b, c, d, e, f, g, h) {}
      Matrix2x4<T>(const glm::mat<2, 4, T>& mat) : m_glmMatrix(mat) {}

      Matrix2x4<T> operator+(const Matrix2x4<T>& rhs) const { return Matrix2x4<T>(m_glmMatrix + rhs.m_glmMatrix); }
      Matrix2x4<T> operator-(const Matrix2x4<T>& rhs) const { return Matrix2x4<T>(m_glmMatrix - rhs.m_glmMatrix); }
      Matrix2x4<T> operator*(const Matrix2x4<T>& rhs) const { return Matrix2x4<T>(m_glmMatrix * rhs.m_glmMatrix); }
      Matrix2x4<T> operator*(T scalar) const { return Matrix2x4<T>(m_glmMatrix * scalar); }

      Matrix2x4<T>& operator+=(const Matrix2x4<T>& rhs) { m_glmMatrix += rhs.m_glmMatrix; return *this; }
      Matrix2x4<T>& operator-=(const Matrix2x4<T>& rhs) { m_glmMatrix -= rhs.m_glmMatrix; return *this; }
      Matrix2x4<T>& operator*=(const Matrix2x4<T>& rhs) { m_glmMatrix *= rhs.m_glmMatrix; return *this; }
      Matrix2x4<T>& operator*=(T scalar) { m_glmMatrix *= scalar; return *this; }
      
      Matrix2x4<T> operator-() const { return Matrix2x4<T>(-m_glmMatrix); }

      T& operator[](int32_t index) { return m_glmMatrix[index]; }
      const T& operator[](int32_t index) const { return m_glmMatrix[index]; }

      bool operator==(const Matrix2x4<T>& rhs) const { return m_glmMatrix == rhs.m_glmMatrix; }
      bool operator!=(const Matrix2x4<T>& rhs) const { return m_glmMatrix != rhs.m_glmMatrix; }

      Matrix2x4<T> Transpose() const { return Matrix2x4<T>(glm::transpose(m_glmMatrix)); }
      Matrix2x4<T> Inverse() const { return Matrix2x4<T>(glm::inverse(m_glmMatrix)); }

      T Determinant() const { return glm::determinant(m_glmMatrix); }

      glm::mat<2, 4, T>& Data() override { return m_glmMatrix; }
      const glm::mat<2, 4, T>& Data() const override { return m_glmMatrix; }
      
   private:
      glm::mat<2, 4, T> m_glmMatrix;
   };
   
   using Matrix2x4i = Matrix2x4<int32_t>;
   using Matrix2x4u = Matrix2x4<uint32_t>;
   using Matrix2x4f = Matrix2x4<float32_t>;
   using Matrix2x4d = Matrix2x4<float64_t>;

   template <typename T>
   class Matrix3x2 : public GLMType<glm::mat<3, 2, T>>
   {
   public:
      Matrix3x2<T>() : m_glmMatrix(static_cast<T>(1)) {}
      Matrix3x2<T>(T a, T b, T c, T d, T e, T f) : m_glmMatrix(a, b, c, d, e, f) {}
      Matrix3x2<T>(const glm::mat<3, 2, T>& mat) : m_glmMatrix(mat) {}

      Matrix3x2<T> operator+(const Matrix3x2<T>& rhs) const { return Matrix3x2<T>(m_glmMatrix + rhs.m_glmMatrix); }
      Matrix3x2<T> operator-(const Matrix3x2<T>& rhs) const { return Matrix3x2<T>(m_glmMatrix - rhs.m_glmMatrix); }
      Matrix3x2<T> operator*(const Matrix3x2<T>& rhs) const { return Matrix3x2<T>(m_glmMatrix * rhs.m_glmMatrix); }
      Matrix3x2<T> operator*(T scalar) const { return Matrix3x2<T>(m_glmMatrix * scalar); }

      Matrix3x2<T>& operator+=(const Matrix3x2<T>& rhs) { m_glmMatrix += rhs.m_glmMatrix; return *this; }
      Matrix3x2<T>& operator-=(const Matrix3x2<T>& rhs) { m_glmMatrix -= rhs.m_glmMatrix; return *this; }
      Matrix3x2<T>& operator*=(const Matrix3x2<T>& rhs) { m_glmMatrix *= rhs.m_glmMatrix; return *this; }
      Matrix3x2<T>& operator*=(T scalar) { m_glmMatrix *= scalar; return *this; }

      Matrix3x2<T> operator-() const { return Matrix3x2<T>(-m_glmMatrix); }

      T& operator[](int32_t index) { return m_glmMatrix[index]; }
      const T& operator[](int32_t index) const { return m_glmMatrix[index]; }

      bool operator==(const Matrix3x2<T>& rhs) const { return m_glmMatrix == rhs.m_glmMatrix; }
      bool operator!=(const Matrix3x2<T>& rhs) const { return m_glmMatrix != rhs.m_glmMatrix; }

      Matrix3x2<T> Transpose() const { return Matrix3x2<T>(glm::transpose(m_glmMatrix)); }
      Matrix3x2<T> Inverse() const { return Matrix3x2<T>(glm::inverse(m_glmMatrix)); }
      
      T Determinant() const { return glm::determinant(m_glmMatrix); }
      
      glm::mat<3, 2, T>& Data() override { return m_glmMatrix; }
      const glm::mat<3, 2, T>& Data() const override { return m_glmMatrix; }
      
   private:
      glm::mat<3, 2, T> m_glmMatrix;
   };
   
   using Matrix3x2i = Matrix3x2<int32_t>;
   using Matrix3x2u = Matrix3x2<uint32_t>;
   using Matrix3x2f = Matrix3x2<float32_t>;
   using Matrix3x2d = Matrix3x2<float64_t>;
   
   template <typename T>
   class Matrix3x3 : public GLMType<glm::mat<3, 3, T>>
   {
   public:
      Matrix3x3<T>() : m_glmMatrix(static_cast<T>(1)) {}
      Matrix3x3<T>(T a, T b, T c, T d, T e, T f, T g, T h, T i) : m_glmMatrix(a, b, c, d, e, f, g, h, i) {}
      Matrix3x3<T>(const glm::mat<3, 3, T>& mat) : m_glmMatrix(mat) {}

      Matrix3x3<T> operator+(const Matrix3x3<T>& rhs) const { return Matrix3x3<T>(m_glmMatrix + rhs.m_glmMatrix); }
      Matrix3x3<T> operator-(const Matrix3x3<T>& rhs) const { return Matrix3x3<T>(m_glmMatrix - rhs.m_glmMatrix); }
      Matrix3x3<T> operator*(const Matrix3x3<T>& rhs) const { return Matrix3x3<T>(m_glmMatrix * rhs.m_glmMatrix); }
      Matrix3x3<T> operator*(T scalar) const { return Matrix3x3<T>(m_glmMatrix * scalar); }

      Matrix3x3<T>& operator+=(const Matrix3x3<T>& rhs) { m_glmMatrix += rhs.m_glmMatrix; return *this; }
      Matrix3x3<T>& operator-=(const Matrix3x3<T>& rhs) { m_glmMatrix -= rhs.m_glmMatrix; return *this; }
      Matrix3x3<T>& operator*=(const Matrix3x3<T>& rhs) { m_glmMatrix *= rhs.m_glmMatrix; return *this; }
      Matrix3x3<T>& operator*=(T scalar) { m_glmMatrix *= scalar; return *this; }

      Matrix3x3<T> operator-() const { return Matrix3x3<T>(-m_glmMatrix); }

      T& operator[](int32_t index) { return m_glmMatrix[index]; }
      const T& operator[](int32_t index) const { return m_glmMatrix[index]; }

      bool operator==(const Matrix3x3<T>& rhs) const { return m_glmMatrix == rhs.m_glmMatrix; }
      bool operator!=(const Matrix3x3<T>& rhs) const { return m_glmMatrix != rhs.m_glmMatrix; }

      Matrix3x3<T> Transpose() const { return Matrix3x3<T>(glm::transpose(m_glmMatrix)); }
      Matrix3x3<T> Inverse() const { return Matrix3x3<T>(glm::inverse(m_glmMatrix)); }
      
      T Determinant() const { return glm::determinant(m_glmMatrix); }
      
      glm::mat<3, 3, T>& Data() override { return m_glmMatrix; }
      const glm::mat<3, 3, T>& Data() const override { return m_glmMatrix; }
      
   private:
      glm::mat<3, 3, T> m_glmMatrix;
   };

   using Matrix3x3i = Matrix3x3<int32_t>;
   using Matrix3x3u = Matrix3x3<uint32_t>;
   using Matrix3x3f = Matrix3x3<float32_t>;
   using Matrix3x3d = Matrix3x3<float64_t>;

   template <typename T>
   class Matrix3x4 : public GLMType<glm::mat<3, 4, T>>
   {
   public:
      Matrix3x4<T>() : m_glmMatrix(static_cast<T>(1)) {}
      Matrix3x4<T>(T a, T b, T c, T d, T e, T f, T g, T h, T i, T j, T k, T l) : m_glmMatrix(a, b, c, d, e, f, g, h, i, j, k, l) {}
      Matrix3x4<T>(const glm::mat<3, 4, T>& mat) : m_glmMatrix(mat) {}

      Matrix3x4<T> operator+(const Matrix3x4<T>& rhs) const { return Matrix3x4<T>(m_glmMatrix + rhs.m_glmMatrix); }
      Matrix3x4<T> operator-(const Matrix3x4<T>& rhs) const { return Matrix3x4<T>(m_glmMatrix - rhs.m_glmMatrix); }
      Matrix3x4<T> operator*(const Matrix3x4<T>& rhs) const { return Matrix3x4<T>(m_glmMatrix * rhs.m_glmMatrix); }
      Matrix3x4<T> operator*(T scalar) const { return Matrix3x4<T>(m_glmMatrix * scalar); }

      Matrix3x4<T>& operator+=(const Matrix3x4<T>& rhs) { m_glmMatrix += rhs.m_glmMatrix; return *this; }
      Matrix3x4<T>& operator-=(const Matrix3x4<T>& rhs) { m_glmMatrix -= rhs.m_glmMatrix; return *this; }
      Matrix3x4<T>& operator*=(const Matrix3x4<T>& rhs) { m_glmMatrix *= rhs.m_glmMatrix; return *this; }
      Matrix3x4<T>& operator*=(T scalar) { m_glmMatrix *= scalar; return *this; }

      Matrix3x4<T> operator-() const { return Matrix3x4<T>(-m_glmMatrix); }

      T& operator[](int32_t index) { return m_glmMatrix[index]; }
      const T& operator[](int32_t index) const { return m_glmMatrix[index]; }

      bool operator==(const Matrix3x4<T>& rhs) const { return m_glmMatrix == rhs.m_glmMatrix; }
      bool operator!=(const Matrix3x4<T>& rhs) const { return m_glmMatrix != rhs.m_glmMatrix; }
      
      glm::mat<3, 4, T>& Data() override { return m_glmMatrix; }
      const glm::mat<3, 4, T>& Data() const override { return m_glmMatrix; }
      
   private:
      glm::mat<3, 4, T> m_glmMatrix;
   };
   
   using Matrix3x4i = Matrix3x4<int32_t>;
   using Matrix3x4u = Matrix3x4<uint32_t>;
   using Matrix3x4f = Matrix3x4<float32_t>;
   using Matrix3x4d = Matrix3x4<float64_t>;
   
   template <typename T>
   class Matrix4x2 : public GLMType<glm::mat<4, 2, T>>
   {
   public:
      Matrix4x2<T>() : m_glmMatrix(static_cast<T>(1)) {}
      Matrix4x2<T>(T a, T b, T c, T d, T e, T f, T g, T h) : m_glmMatrix(a, b, c, d, e, f, g, h) {}
      Matrix4x2<T>(const glm::mat<4, 2, T>& mat) : m_glmMatrix(mat) {}

      Matrix4x2<T> operator+(const Matrix4x2<T>& rhs) const { return Matrix4x2<T>(m_glmMatrix + rhs.m_glmMatrix); }
      Matrix4x2<T> operator-(const Matrix4x2<T>& rhs) const { return Matrix4x2<T>(m_glmMatrix - rhs.m_glmMatrix); }
      Matrix4x2<T> operator*(const Matrix4x2<T>& rhs) const { return Matrix4x2<T>(m_glmMatrix * rhs.m_glmMatrix); }
      Matrix4x2<T> operator*(T scalar) const { return Matrix4x2<T>(m_glmMatrix * scalar); }

      Matrix4x2<T>& operator+=(const Matrix4x2<T>& rhs) { m_glmMatrix += rhs.m_glmMatrix; return *this; }
      Matrix4x2<T>& operator-=(const Matrix4x2<T>& rhs) { m_glmMatrix -= rhs.m_glmMatrix; return *this; }
      Matrix4x2<T>& operator*=(const Matrix4x2<T>& rhs) { m_glmMatrix *= rhs.m_glmMatrix; return *this; }
      Matrix4x2<T>& operator*=(T scalar) { m_glmMatrix *= scalar; return *this; }

      Matrix4x2<T> operator-() const { return Matrix4x2<T>(-m_glmMatrix); }

      T& operator[](int32_t index) { return m_glmMatrix[index]; }
      const T& operator[](int32_t index) const { return m_glmMatrix[index]; }

      bool operator==(const Matrix4x2<T>& rhs) const { return m_glmMatrix == rhs.m_glmMatrix; }
      bool operator!=(const Matrix4x2<T>& rhs) const { return m_glmMatrix != rhs.m_glmMatrix; }
      
      glm::mat<4, 2, T>& Data() override { return m_glmMatrix; }
      const glm::mat<4, 2, T>& Data() const override { return m_glmMatrix; }
      
   private:
      glm::mat<4, 2, T> m_glmMatrix;
   };
   
   using Matrix4x2i = Matrix4x2<int32_t>;
   using Matrix4x2u = Matrix4x2<uint32_t>;
   using Matrix4x2f = Matrix4x2<float32_t>;
   using Matrix4x2d = Matrix4x2<float64_t>;
   
   template <typename T>
   class Matrix4x3 : public GLMType<glm::mat<4, 3, T>>
   {
   public:
      Matrix4x3<T>() : m_glmMatrix(static_cast<T>(1)) {}
      Matrix4x3<T>(T a, T b, T c, T d, T e, T f, T g, T h, T i, T j, T k, T l) : m_glmMatrix(a, b, c, d, e, f, g, h, i, j, k, l) {}
      Matrix4x3<T>(const glm::mat<4, 3, T>& mat) : m_glmMatrix(mat) {}

      Matrix4x3<T> operator+(const Matrix4x3<T>& rhs) const { return Matrix4x3<T>(m_glmMatrix + rhs.m_glmMatrix); }
      Matrix4x3<T> operator-(const Matrix4x3<T>& rhs) const { return Matrix4x3<T>(m_glmMatrix - rhs.m_glmMatrix); }
      Matrix4x3<T> operator*(const Matrix4x3<T>& rhs) const { return Matrix4x3<T>(m_glmMatrix * rhs.m_glmMatrix); }
      Matrix4x3<T> operator*(T scalar) const { return Matrix4x3<T>(m_glmMatrix * scalar); }

      Matrix4x3<T>& operator+=(const Matrix4x3<T>& rhs) { m_glmMatrix += rhs.m_glmMatrix; return *this; }
      Matrix4x3<T>& operator-=(const Matrix4x3<T>& rhs) { m_glmMatrix -= rhs.m_glmMatrix; return *this; }
      Matrix4x3<T>& operator*=(const Matrix4x3<T>& rhs) { m_glmMatrix *= rhs.m_glmMatrix; return *this; }
      Matrix4x3<T>& operator*=(T scalar) { m_glmMatrix *= scalar; return *this; }

      Matrix4x3<T> operator-() const { return Matrix4x3<T>(-m_glmMatrix); }

      T& operator[](int32_t index) { return m_glmMatrix[index]; }
      const T& operator[](int32_t index) const { return m_glmMatrix[index]; }

      bool operator==(const Matrix4x3<T>& rhs) const { return m_glmMatrix == rhs.m_glmMatrix; }
      bool operator!=(const Matrix4x3<T>& rhs) const { return m_glmMatrix != rhs.m_glmMatrix; }
      
      glm::mat<4, 3, T>& Data() override { return m_glmMatrix; }
      const glm::mat<4, 3, T>& Data() const override { return m_glmMatrix; }
      
   private:
      glm::mat<4, 3, T> m_glmMatrix;
   };
   
   using Matrix4x3i = Matrix4x3<int32_t>;
   using Matrix4x3u = Matrix4x3<uint32_t>;
   using Matrix4x3f = Matrix4x3<float32_t>;
   using Matrix4x3d = Matrix4x3<float64_t>;
   
   template <typename T>
   class Matrix4x4 : public GLMType<glm::mat<4, 4, T>>
   {
   public:
      Matrix4x4<T>() : m_glmMatrix(static_cast<T>(1)) {}
      Matrix4x4<T>(T a, T b, T c, T d, T e, T f, T g, T h, T i, T j, T k, T l, T m, T n, T o, T p) : m_glmMatrix(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p) {}
      Matrix4x4<T>(const glm::mat<4, 4, T>& mat) : m_glmMatrix(mat) {}

      Matrix4x4<T> operator+(const Matrix4x4<T>& rhs) const { return Matrix4x4<T>(m_glmMatrix + rhs.m_glmMatrix); }
      Matrix4x4<T> operator-(const Matrix4x4<T>& rhs) const { return Matrix4x4<T>(m_glmMatrix - rhs.m_glmMatrix); }
      Matrix4x4<T> operator*(const Matrix4x4<T>& rhs) const { return Matrix4x4<T>(m_glmMatrix * rhs.m_glmMatrix); }
      Matrix4x4<T> operator*(T scalar) const { return Matrix4x4<T>(m_glmMatrix * scalar); }

      Matrix4x4<T>& operator+=(const Matrix4x4<T>& rhs) { m_glmMatrix += rhs.m_glmMatrix; return *this; }
      Matrix4x4<T>& operator-=(const Matrix4x4<T>& rhs) { m_glmMatrix -= rhs.m_glmMatrix; return *this; }
      Matrix4x4<T>& operator*=(const Matrix4x4<T>& rhs) { m_glmMatrix *= rhs.m_glmMatrix; return *this; }
      Matrix4x4<T>& operator*=(T scalar) { m_glmMatrix *= scalar; return *this; }

      Matrix4x4<T> operator-() const { return Matrix4x4<T>(-m_glmMatrix); }

      T& operator[](int32_t index) { return m_glmMatrix[index]; }
      const T& operator[](int32_t index) const { return m_glmMatrix[index]; }
      
      bool operator==(const Matrix4x4<T>& rhs) const { return m_glmMatrix == rhs.m_glmMatrix; }
      bool operator!=(const Matrix4x4<T>& rhs) const { return m_glmMatrix != rhs.m_glmMatrix; }
      
      glm::mat<4, 4, T>& Data() override { return m_glmMatrix; }
      const glm::mat<4, 4, T>& Data() const override { return m_glmMatrix; }
      
   private:
      glm::mat<4, 4, T> m_glmMatrix;
   };
   
   using Matrix4x4i = Matrix4x4<int32_t>;
   using Matrix4x4u = Matrix4x4<uint32_t>;
   using Matrix4x4f = Matrix4x4<float32_t>;
   using Matrix4x4d = Matrix4x4<float64_t>;
} // namespace ARC
