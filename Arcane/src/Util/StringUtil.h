#pragma once

namespace ARC
{
   namespace StringUtil
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

      template <typename T>
      std::string ToString(const T& value)
      {
         if constexpr (std::is_same_v<T, std::wstring>)
         {
            int32_t size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int32_t>(value.size()), NULL, 0, NULL, NULL);
            std::string result;
            result.resize(size);
            WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int32_t>(value.size()), result.data(), size, NULL, NULL);
            return result;
         }
         else if constexpr (std::is_same_v<T, const wchar_t*> || std::is_same_v<T, wchar_t*>)
         {
            int32_t size = WideCharToMultiByte(CP_UTF8, 0, value, static_cast<int32_t>(std::wcslen(value)), NULL, 0, NULL, NULL);
            std::string result;
            result.resize(size);
            WideCharToMultiByte(CP_UTF8, 0, value, static_cast<int32_t>(std::wcslen(value)), result.data(), size, NULL, NULL);
            return result;
         }
         else if constexpr (std::is_floating_point_v<T>)
         {
            std::ostringstream oss;
            oss.precision(std::numeric_limits<T>::max_digits10);
            oss << value;
            return oss.str();
         }
         else if constexpr (std::is_same_v<T, char*> || std::is_same_v<T, const char*>)
         {
            return std::string(value);
         }
         else if constexpr (std::is_convertible_v<T, std::string>)
         {
            return value;
         }
         else
         {
            std::ostringstream oss;
            oss << value;
            return oss.str();
         }
      }

      template <typename T>
      std::wstring ToWString(const T& value)
      {
         if constexpr (std::is_same_v<T, std::string>)
         {
            int32_t size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int32_t>(value.size()), NULL, 0);
            std::wstring result;
            result.resize(size);
            MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int32_t>(value.size()), result.data(), size);
            return result;
         }
         else if constexpr (std::is_same_v<T, const wchar_t*> || std::is_same_v<T, wchar_t*>)
         {
            return std::wstring(value);
         }
         else if constexpr (std::is_floating_point_v<T>)
         {
            std::wostringstream woss;
            woss.precision(std::numeric_limits<T>::max_digits10);
            woss << value;
            return woss.str();
         }
         else if constexpr (std::is_same_v<T, char*> || std::is_same_v<T, const char*>)
         {
            int32_t size = MultiByteToWideChar(CP_UTF8, 0, value, static_cast<int32_t>(std::strlen(value)), NULL, 0);
            std::wstring result;
            result.resize(size);
            MultiByteToWideChar(CP_UTF8, 0, value, static_cast<int32_t>(std::strlen(value)), result.data(), size);
            return result;
         }
         else if constexpr (std::is_convertible_v<T, std::wstring>)
         {
            return value;
         }
         else
         {
            std::wostringstream woss;
            woss << value;
            return woss.str();
         }
      }
   } // namespace StringUtil
} // namespace ARC
