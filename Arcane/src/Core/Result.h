#pragma once

#include "Error.h"

namespace ARC
{
   template <typename T>
   class Result
   {
   public:
      Result(const T& value) : m_data(value) {}
      Result(Error error) : m_data(std::move(error)) {}

      inline bool Success() const { return std::holds_alternative<T>(m_data); }

      explicit operator bool() { return Success(); }

      operator T&() { return Value(); }

      operator T*() { return Success() ? &Value() : nullptr; }

      [[nodiscard]] typename T& Value()
      {
         ARC_ASSERT(Success(), "Attempted to access value on an error result");
         return std::get<T>(m_data);
      }

      [[nodiscard]] Error& Error()
      {
         ARC_ASSERT(!Success(), "Attempted to access error on a success result");
         return std::get<ARC::Error>(m_data);
      }

      std::string GetErrorMessage() const
      {
         if (Success())
            return "No error.";
         return std::get<ARC::Error>(m_data).ToString();
      }

   private:
      std::variant<T, ARC::Error> m_data;
   };

   template <>
   class Result<void>
   {
   public:
      Result() : m_data(Error::Create(ErrorCode::None, std::string())) {}

      Result(Error error) : m_data(std::move(error)) {}

      explicit operator bool() { return Success(); }

      inline bool Success() const { return m_data.Code() == 0; }

      [[nodiscard]] Error& Error()
      {
         ARC_ASSERT(!Success(), "Attempted to access error on an success result");
         return m_data;
      }

      std::string GetErrorMessage() const
      {
         if (Success())
            return "No error.";
         return m_data.ToString();
      }

   private:
      ARC::Error m_data;
   };
} // namespace ARC
