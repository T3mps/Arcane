#pragma once

#include "Error.h"

namespace ARC
{
   template <typename T>
   class Result
   {
   public:
      Result(const T& value) : m_data(value) {}
      Result(T&& value) : m_data(std::move(value)) {}
      Result(Error error) : m_data(std::move(error)) {}

      Result(const Result&) = delete;
      Result& operator=(const Result&) = delete;

      Result(Result&& other) noexcept = default;
      Result& operator=(Result&& other) noexcept = default;

      inline bool Success() const { return std::holds_alternative<T>(m_data); }

      explicit operator bool() { return Success(); }
      T& operator*() { return std::get<T>(m_data); /* Assumes Result is successful */ }

      const T& operator*() const { return std::get<T>(m_data);  /* Assumes Result is successful */ }
      operator T&() { return GetValue(); }
      operator T*() { return Success() ? &GetValue() : nullptr; }

      template<size_t I>
      decltype(auto) get() &
      {
         if constexpr (I == 0)
            return Success();
         else if constexpr (I == 1)
            return Success() ? GetValue() : throw std::runtime_error("...");
      }

      template<typename F>
      auto Map(F&& f) -> Result<std::invoke_result_t<F, T&>>
      {
         using ReturnType = std::invoke_result_t<F, T&>;

         if (Success())
         {
            if constexpr (std::is_void_v<ReturnType>)
            {
               f(GetValue());
               return Result<void>();
            }
            else
            {
               return f(GetValue());
            }
         }
         return GetError();
      }

      template<typename F>
      auto AndThen(F&& f) -> std::invoke_result_t<F, T&>
      {
         using ReturnType = std::invoke_result_t<F, T&>;
         static_assert(IsResult<ReturnType>::value, "AndThen requires a function that returns a Result");

         if (Success())
            return f(GetValue());
         return GetError();
      }

      [[nodiscard]] T& GetValue()
      {
         ARC_ASSERT(Success(), "Attempted to access value on an error result");
         return std::get<T>(m_data);
      }

      [[nodiscard]] const T& GetValue() const
      {
         ARC_ASSERT(Success(), "Attempted to access value on an error result");
         return std::get<T>(m_data);
      }

      [[nodiscard]] Error& GetError()
      {
         ARC_ASSERT(!Success(), "Attempted to access error on a success result");
         return std::get<ARC::Error>(m_data);
      }

      [[nodiscard]] const Error& GetError() const
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
      template<typename U>
      struct IsResult : std::false_type {};

      template<typename U>
      struct IsResult<Result<U>> : std::true_type {};

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

      template<typename F>
      auto Map(F&& f) -> Result<std::invoke_result_t<F>>
      {
         using ReturnType = std::invoke_result_t<F>;

         if (Success())
         {
            if constexpr (std::is_void_v<ReturnType>)
            {
               f();
               return Result<void>();
            }
            else
            {
               return f();
            }
         }
         return GetError();
      }

      template<typename F>
      auto AndThen(F&& f) -> std::invoke_result_t<F>
      {
         using ReturnType = std::invoke_result_t<F>;
         static_assert(IsResult<ReturnType>::value, 
            "AndThen requires a function that returns a Result");

         if (Success())
            return f();
         return GetError();
      }

      [[nodiscard]] Error& GetError()
      {
         ARC_ASSERT(!Success(), "Attempted to access error on a success result");
         return m_data;
      }

      [[nodiscard]] const Error& GetError() const
      {
         ARC_ASSERT(!Success(), "Attempted to access error on a success result");
         return m_data;
      }

      std::string GetErrorMessage() const
      {
         if (Success())
            return "No error.";
         return m_data.ToString();
      }

   private:
      template<typename U>
      struct IsResult : std::false_type {};

      template<typename U>
      struct IsResult<Result<U>> : std::true_type {};

      ARC::Error m_data;
   };
} // namespace ARC
