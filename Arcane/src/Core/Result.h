#pragma once

#include <iostream>
#include <stdexcept>
#include <string>
#include <variant>

#include "Common.h"
#include "Error.h"

namespace Arcane
{
   template<typename T>
   class Result
   {
   public:
      Result(const T& value) : m_data(value) {}
      Result(Error error) : m_data(std::move(error)) {}

      inline bool Success() const { return std::holds_alternative<T>(m_data); }

      explicit operator bool() { return Success(); }

      [[nodiscard]] typename T& Value()
      {
         ARC_ASSERT(!Success(), "Attempted to access value on an error result");
         return std::get<T>(m_data);
      }

      [[nodiscard]] Error& Error()
      {
         ARC_ASSERT(Success(), "Attempted to access error on a success result");
         return std::get<Error>(m_data);
      }

      std::string GetErrorMessage() const
      {
         if (Success())
            return "No error.";
         return std::get<Arcane::Error>(m_data).ToString();
      }

   private:
      std::variant<T, Arcane::Error> m_data;
   };

   template<>
   class Result<void>
   {
   public:
      Result() : m_data(Error::Create(ErrorCode::None, std::string())) {}

      Result(Error error) : m_data(std::move(error)) {}

      inline bool Success() const { return m_data.Code() == ErrorCode::None; }

      explicit operator bool() { return Success(); }

      [[nodiscard]] Error& Error()
      {
         ARC_ASSERT(Success(), "Attempted to access error on an success result");
         return m_data;
      }

      std::string GetErrorMessage() const
      {
         if (Success())
            return "No error.";
         return m_data.ToString();
      }

   private:
      Arcane::Error m_data;
   };
} // namespace Arcane
