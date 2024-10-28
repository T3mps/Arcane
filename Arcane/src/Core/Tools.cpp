// Tools.cpp
#include "arcpch.h"
#include "Tools.h"

#ifdef ARC_PLATFORM_WINDOWS
   #include <shlobj.h>
   #include <combaseapi.h>
#endif

#include "Network/CDN.h"

ARC::Result<void> ARC::Tools::Initialize()
{
   if (!FetchLocalDirectory().empty())
   {
      ARC_CORE_INFO("Locating tools directory...");
      std::error_code ec;
      std::filesystem::create_directories(s_directory, ec);
      if (ec)
         return ARC_MAKE_ERROR(ErrorCode::OperationFailed, std::format("Failed to create tools directory: {}", ec.message()));
      ARC_CORE_INFO("Tools directory: {}", s_directory.string());
   }

   CDN::RegisterSubdirectory("tools");

   Register(Tool::GenerateModuleCertificate, "gencert.ps1");

   return {};
}

void ARC::Tools::Register(Tool tool, const std::string_view localPath)
{
   s_toolMap[tool] = s_directory / localPath;
   if (auto result = VerifyOrDownload(s_toolMap[tool].string()); !result)
   {
      ARC_CORE_ERROR("Failed to verify/download tool: {}", result.GetError().Message());
   }
}

std::filesystem::path ARC::Tools::GetPath(Tool tool)
{
   auto it = s_toolMap.find(tool);
   return it != s_toolMap.end() ? it->second : std::filesystem::path();
}

std::filesystem::path ARC::Tools::GetToolsDirectory()
{
   return s_directory;
}

std::filesystem::path ARC::Tools::FetchLocalDirectory()
{
#ifdef ARC_PLATFORM_WINDOWS
   wchar_t* localAppData = nullptr;
   if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, NULL, &localAppData)))
   {
      s_directory = std::filesystem::path(localAppData) / "Arcane" / "Tools";
      CoTaskMemFree(localAppData);
   }
#else
   const char* homeDir = getenv("HOME");
   if (homeDir)
   {
      s_directory = std::filesystem::path(homeDir) / ".local" / "share" / "ArcaneEngine" / "tools";
   }
#endif
   return s_directory;
}

ARC::Result<void> ARC::Tools::VerifyOrDownload(const std::string_view localPath)
{
   if (localPath.empty())
      return ARC_MAKE_ERROR(ErrorCode::InvalidArgument, "Invalid tool path");

   std::filesystem::path fullPath(localPath);
   std::string filename = fullPath.filename().string();

   ARC_CORE_INFO("Verifying or downloading tool: {}", filename);
   return CDN::DownloadAndVerify("tools", filename, fullPath);
}
