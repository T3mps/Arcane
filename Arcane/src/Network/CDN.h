#pragma once

#include "Network.h"

namespace ARC
{
   class CDN
   {
   public:
      static constexpr std::string_view DEFAULT_URL = "https://arcane-cdn.nyc3.digitaloceanspaces.com";
      
      static void RegisterSubdirectory(const std::string& subPath);

      static ARC::Result<void> VerifyLocal(const std::filesystem::path& localPath);
      static ARC::Result<void> Download(const std::string_view subdir, const std::string_view filename, const std::filesystem::path& localPath);
      static ARC::Result<void> DownloadAndVerify(const std::string_view subdir, const std::string_view filename, const std::filesystem::path& localPath);

      static void SetBaseURL(const std::string_view baseURL);
      static void ResetBaseURL();

   private:
      static std::string GetBaseUrl();
      
      static inline std::string s_baseUrl;
      static inline std::unordered_map<std::string, std::string> s_subdirectories;
   };
} // namespace ARC
