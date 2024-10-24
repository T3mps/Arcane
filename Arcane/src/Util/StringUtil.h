#pragma once

namespace ARC::StringUtil
{
   const std::string EMPTY_STRING = "";
   const std::wstring EMPTY_WSTRING = L"";

   std::string ToLower(const std::string& str);
   std::wstring ToLower(const std::wstring& str);

   std::string ToUpper(const std::string& str);
   std::wstring ToUpper(const std::wstring& str);

   int32_t Compare(const std::string& str1, const std::string& str2);
   int32_t Compare(const std::wstring& str1, const std::wstring& str2);

   std::string Trim(const std::string& str);
   std::wstring Trim(const std::wstring& str);

   char* ToChar(const std::string& str);
   wchar_t* ToWChar(const std::wstring& str);

   std::string FromChar(const char* str);
   std::wstring FromWChar(const wchar_t* str);

   std::vector<std::string> Split(const std::string& str, const std::string& delimiters);
   std::vector<std::wstring> Split(const std::wstring& str, const std::wstring& delimiters);

   std::string Replace(const std::string& str, const std::string& find, const std::string& replace);
   std::wstring Replace(const std::wstring& str, const std::wstring& find, const std::wstring& replace);

   inline std::string ToString(const std::wstring& value)
   {
#ifdef ARC_PLATFORM_WINDOWS
      if (value.empty())
         return std::string();

      int size_needed = WideCharToMultiByte(CP_UTF8, 0, &value[0], static_cast<int>(value.size()), NULL, 0, NULL, NULL);
      std::string result(size_needed, 0);
      WideCharToMultiByte(CP_UTF8, 0, &value[0], static_cast<int>(value.size()), &result[0], size_needed, NULL, NULL);
      return result;
#else
      std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
      return converter.to_bytes(value);
#endif
   }

   inline std::string ToString(std::wstring_view value)
   {
      return ToString(std::wstring(value));
   }

   inline std::string ToString(const wchar_t* value)
   {
      return value ? ToString(std::wstring(value)) : std::string();
   }

   template <typename T>
   inline std::enable_if_t<std::is_integral_v<T>, std::string> ToString(T value)
   {
      char buffer[std::numeric_limits<T>::digits10 + 3];
      auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value);
      if (ec == std::errc())
      {
         return std::string(buffer, ptr);
      }
      else
      {
         return std::string();
      }
   }

   template <typename T>
   inline std::enable_if_t<std::is_floating_point_v<T>, std::string> ToString(T value)
   {
      char buffer[64];
      auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general);
      if (ec == std::errc())
      {
         return std::string(buffer, ptr);
      }
      else
      {
         return std::string();
      }
   }

   inline const std::string& ToString(const std::string& value)
   {
      return value;
   }

   inline std::string ToString(std::string_view value)
   {
      return std::string(value);
   }

   inline std::string ToString(const char* value)
   {
      return value ? std::string(value) : std::string();
   }

   template <typename T>
   inline std::enable_if_t<!std::is_arithmetic_v<T> && !std::is_convertible_v<T, std::string>, std::string> ToString(const T& value)
   {
      std::ostringstream oss;
      oss << value;
      return oss.str();
   }

   inline std::wstring ToWString(const std::string& value)
   {
#ifdef ARC_PLATFORM_WINDOWS
      if (value.empty())
         return std::wstring();

      int size_needed = MultiByteToWideChar(CP_UTF8, 0, &value[0], static_cast<int>(value.size()), NULL, 0);
      std::wstring result(size_needed, 0);
      MultiByteToWideChar(CP_UTF8, 0, &value[0], static_cast<int>(value.size()), &result[0], size_needed);
      return result;
#else
      std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
      return converter.from_bytes(value);
#endif
   }

   inline std::wstring ToWString(std::string_view value)
   {
      return ToWString(std::string(value));
   }

   inline std::wstring ToWString(const char* value)
   {
      return value ? ToWString(std::string(value)) : std::wstring();
   }

   template <typename T>
   inline std::enable_if_t<std::is_integral_v<T>, std::wstring> ToWString(T value)
   {
      wchar_t buffer[std::numeric_limits<T>::digits10 + 3];
      int result = std::swprintf(buffer, sizeof(buffer) / sizeof(wchar_t), L"%lld", static_cast<long long>(value));
      if (result >= 0)
      {
         return std::wstring(buffer);
      }
      else
      {
         return std::wstring();
      }
   }

   template <typename T>
   inline std::enable_if_t<std::is_floating_point_v<T>, std::wstring> ToWString(T value)
   {
      wchar_t buffer[64];
      int result = std::swprintf(buffer, sizeof(buffer) / sizeof(wchar_t), L"%.*g", std::numeric_limits<T>::max_digits10, value);
      if (result >= 0)
      {
         return std::wstring(buffer);
      }
      else
      {
         return std::wstring();
      }
   }

   inline const std::wstring& ToWString(const std::wstring& value)
   {
      return value;
   }

   inline std::wstring ToWString(std::wstring_view value)
   {
      return std::wstring(value);
   }

   inline std::wstring ToWString(const wchar_t* value)
   {
      return value ? std::wstring(value) : std::wstring();
   }

   template <typename T>
   inline std::enable_if_t<!std::is_arithmetic_v<T> && !std::is_convertible_v<T, std::wstring>, std::wstring> ToWString(const T& value)
   {
      std::wostringstream woss;
      woss << value;
      return woss.str();
   }
} // namespace ARC
