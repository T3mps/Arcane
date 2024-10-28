#pragma once

namespace ARC
{
   enum Tool
   {
      Unknown = 0,
      GenerateModuleCertificate
   };

   class Tools
   {
   public:
      static Result<void> Initialize();
      
      static std::filesystem::path GetPath(Tool tool);

      [[nodiscard]] static std::filesystem::path GetToolsDirectory();
     
   private:
      static constexpr std::string_view TOOLS_CDN = "https://arcane-cdn.nyc3.digitaloceanspaces.com/tools/";

      ~Tools() = default;

      static std::filesystem::path FetchLocalDirectory();
      static void Register(Tool tool, const std::string_view localPath);

      static Result<void> VerifyOrDownload(const std::string_view localPath);

      inline static std::filesystem::path s_directory;
      inline static std::unordered_map<Tool, std::filesystem::path> s_toolMap;
   };
} // namespace ARC
