#pragma once

namespace ARC
{
   template <typename T>
   class Singleton
   {
   public:
      static T& GetInstance()
      {
         static T instance;
         return instance;
      }

   protected:
      Singleton() = default;
      virtual ~Singleton() = default;

      Singleton(const Singleton& other) = delete;
      Singleton& operator=(const Singleton& other) = delete;

      Singleton(Singleton&& other) = delete;
      Singleton& operator=(Singleton&& other) = delete;
   };
} // namespace ARC
