#pragma once

#include "Util/StackTrace.h"
#include "Util/UUID.h"

namespace ARC
{
   // Concept to validate that a type is a valid error enum
   template <typename E>
   concept ValidErrorCodeE = std::is_enum_v<E> && requires(E e)
   {
      { static_cast<std::underlying_type_t<E>>(e) } -> std::convertible_to<uint64_t>;
   };

   enum class ErrorCodeBeginMap : uint64_t
   {
      Core = 0,
      Loom = 1024
   };

   // Only store 1024 error codes in one error enum
   enum class ErrorCode : uint64_t
   {
      None = ErrorCodeBeginMap::Core,
      InvalidArgument,
      NullPointer,
      IndexOutOfBounds,
      OperationFailed,
      NotImplemented,
      UnsupportedOperation,
      Timeout,
      FileNotFound,
      UnknownError
   };

   // Error class that uses a variant to handle multiple types of error codes
   class Error
   {
   public:
      template <ValidErrorCodeE E>
      static Error Create(E code, const std::string_view message, const std::source_location location = std::source_location::current())
      {
         std::optional<std::string> stackTrace = std::nullopt;
#ifdef ARC_BUILD_DEBUG
         stackTrace = StackTrace::Capture();
#endif
         return Error(static_cast<uint64_t>(code), message.data(), stackTrace, location.file_name(), location.line());
      }

      uint64_t Code()                                 const { return m_code; }
      const std::string& Message()                    const { return m_message; }
      const std::optional<std::string>& StackTrace()  const { return m_stackTrace; }
      const std::string& GetFile()                    const { return m_file; }
      int32_t GetLine()                                   const { return m_line; }
      const UUID& GetUUID()                           const { return m_uuid; }

      std::string ToString() const
      {
         return "Error: " + m_message + " (Code: " + std::to_string(m_code) + "), in file: " + m_file + " at line: " + std::to_string(m_line) + (m_stackTrace ? "\nStack Trace:\n" + *m_stackTrace : "");
      }

   private:
      Error(uint64_t code, const std::string& msg, std::optional<std::string> trace = std::nullopt, std::string file = "", int32_t line = 0) :
         m_code(code),
         m_message(msg),
         m_stackTrace(trace),
         m_file(file),
         m_line(line),
         m_uuid()
      {}

      uint64_t m_code;
      std::string m_message;
      std::optional<std::string> m_stackTrace;
      std::string m_file;
      int32_t m_line;
      UUID m_uuid;
   };
} // namespace ARC

#define ARC_MAKE_ERROR(code, message) \
    ARC::Error::Create(code, message, std::source_location::current())
