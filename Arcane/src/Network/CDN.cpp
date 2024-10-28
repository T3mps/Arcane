#include "arcpch.h"
#include "CDN.h"

void ARC::CDN::RegisterSubdirectory(const std::string& subPath)
{
   if (!subPath.empty())
      s_subdirectories[subPath] = subPath;
}

ARC::Result<void> ARC::CDN::VerifyLocal(const std::filesystem::path& localPath)
{
   if (localPath.empty())
      return ARC_MAKE_ERROR(ErrorCode::InvalidArgument, "Invalid local path");

   if (std::filesystem::exists(localPath))
      return {};

   return ARC_MAKE_ERROR(ErrorCode::FileNotFound, "File does not exist locally: " + localPath.string());
}

ARC::Result<void> ARC::CDN::Download(const std::string_view subdir, const std::string_view filename, const std::filesystem::path& localPath)
{
   if (subdir.empty() || filename.empty() || localPath.empty())
      return ARC_MAKE_ERROR(ErrorCode::InvalidArgument, "Invalid arguments for download");

   auto it = s_subdirectories.find(std::string(subdir));
   if (it == s_subdirectories.end())
      return ARC_MAKE_ERROR(ErrorCode::InvalidArgument, "Subdirectory not registered: " + std::string(subdir));

   std::string fullUrl = GetBaseUrl() + (GetBaseUrl().ends_with('/') ? "" : "/") + it->second + "/" + filename.data();

   ARC_CORE_INFO("Starting download from CDN: {}", fullUrl);

   Network::RequestConfig config;
   config.progress = [](const Network::HttpProgress& progress)
   {
      static int lastPercentage = -1;
      int currentPercentage = static_cast<int>(progress.progressPercentage);
      if (currentPercentage % 10 == 0 && currentPercentage != lastPercentage)
      {
         ARC_CORE_TRACE("Download progress: {}% ({:.2f} MB/s)", currentPercentage, progress.bytesPerSecond / (1024.0f * 1024.0f));
         lastPercentage = currentPercentage;
      }
   };

   return Network::Download(fullUrl, localPath, config);
}

ARC::Result<void> ARC::CDN::DownloadAndVerify(const std::string_view subdir, const std::string_view filename, const std::filesystem::path& localPath)
{
   auto verifyResult = VerifyLocal(localPath);
   if (verifyResult)
   {
      ARC_CORE_INFO("File already verified locally: {}", localPath.string());
      return {};
   }

   ARC_CORE_INFO("File not found locally, downloading: {}", localPath.string());
   return Download(subdir, filename, localPath);
}

void ARC::CDN::SetBaseURL(const std::string_view baseURL)
{
   s_baseUrl = baseURL;
}

void ARC::CDN::ResetBaseURL()
{
   // Clearing will allow lazy initialization to set it to DEFAULT_URL
   s_baseUrl.clear();
}

std::string ARC::CDN::GetBaseUrl()
{
   if (s_baseUrl.empty())
      s_baseUrl = std::string(DEFAULT_URL);
   return s_baseUrl;
}
