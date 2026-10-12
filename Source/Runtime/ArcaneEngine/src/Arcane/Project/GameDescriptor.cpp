#include <Arcane/Project/GameDescriptor.hpp>

#include <Json.hpp>

#include <fstream>
#include <system_error>

namespace Arcane
{
    std::string GameDescriptorFileName(std::string_view moduleName)
    {
        return std::string(moduleName) + ".arcmodule";
    }

    std::string RenderDefaultGameDescriptor(std::string_view moduleName)
    {
        // nlohmann::ordered_json keeps the key order build/modules.lua's
        // descriptors use, and escapes the name correctly.
        nlohmann::ordered_json deps = nlohmann::ordered_json::array();
        for (const std::string_view module : kDefaultGameModules)
            deps.push_back(std::string(module));

        nlohmann::ordered_json doc;
        doc["name"]         = std::string(moduleName);
        doc["type"]         = "Game";
        doc["dependencies"] = { { "public", nlohmann::ordered_json::array() }, { "private", deps } };
        doc["thirdParty"]   = { { "public", nlohmann::ordered_json::array() },
                                { "private", nlohmann::ordered_json::array() } };
        return doc.dump(2) + "\n";
    }

    bool WriteDefaultGameDescriptorIfAbsent(const std::filesystem::path& sourceDir, std::string_view moduleName)
    {
        const std::filesystem::path file = sourceDir / GameDescriptorFileName(moduleName);
        std::error_code ec;
        if (std::filesystem::exists(file, ec))
            return true;   // never overwrite: the project owns its module list
        std::filesystem::create_directories(sourceDir, ec);
        std::ofstream out(file, std::ios::binary);
        if (!out)
            return false;
        out << RenderDefaultGameDescriptor(moduleName);
        return static_cast<bool>(out);
    }
}
