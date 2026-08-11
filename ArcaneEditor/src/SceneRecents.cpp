#include "SceneRecents.hpp"

#include <Arcane/Base/Log.hpp>

#include <Json.hpp>   // the workspace's vendored nlohmann::json header

#include <algorithm>
#include <fstream>
#include <optional>
#include <sstream>
#include <system_error>

namespace Arcane::Editor::SceneRecents
{
namespace
{
    // nullopt == the file exists but could not be read (or does not exist at
    // all -- LoadFile treats both as "start empty", so the distinction RecentProjects'
    // ReadWhole makes does not matter here; there is no "never clobber" contract
    // to protect).
    std::optional<std::string> ReadWhole(const std::filesystem::path& p)
    {
        std::error_code ec;
        if (!std::filesystem::exists(p, ec))
            return std::string{};

        std::ifstream in(p, std::ios::binary);
        if (!in)
            return std::nullopt;

        std::ostringstream ss;
        ss << in.rdbuf();
        if (in.bad())
            return std::nullopt;
        return ss.str();
    }
}

std::filesystem::path FileFor(const std::filesystem::path& projectRoot)
{
    return projectRoot / "Saved" / "recent_scenes.json";
}

List Parse(const std::string& jsonText)
{
    List out;
    if (jsonText.empty())
        return out;

    nlohmann::json doc = nlohmann::json::parse(jsonText, nullptr, /*allow_exceptions*/ false);
    if (doc.is_discarded() || !doc.is_object())
        return out;

    // Version gate BEFORE touching `scenes`: a newer format's entries need
    // not mean what this build assumes they mean.
    if (const auto v = doc.find("version"); v != doc.end() && v->is_number_integer())
    {
        if (v->get<int>() > kFormatVersion)
            return out;
    }

    const auto scenes = doc.find("scenes");
    if (scenes == doc.end() || !scenes->is_array())
        return out;

    out.paths.reserve(scenes->size());
    for (const nlohmann::json& e : *scenes)
    {
        if (!e.is_string())
            continue;   // skip non-string entries rather than fail the whole list
        std::string s = e.get<std::string>();
        if (!s.empty())
            out.paths.push_back(std::move(s));
    }
    return out;
}

std::string Serialize(const List& list)
{
    nlohmann::json doc;
    doc["version"] = kFormatVersion;
    doc["scenes"]  = list.paths;
    return doc.dump(2);   // 2-space, matching RecentProjects.cpp's convention
}

void Push(List& list, const std::filesystem::path& scenePath)
{
    const std::string key = scenePath.lexically_normal().generic_string();
    if (key.empty())
        return;
    std::erase(list.paths, key);
    list.paths.insert(list.paths.begin(), key);
    if (list.paths.size() > kMaxEntries)
        list.paths.resize(kMaxEntries);
}

List LoadFile(const std::filesystem::path& file)
{
    if (file.empty())
        return {};
    const std::optional<std::string> text = ReadWhole(file);
    if (!text)
        return {};
    return Parse(*text);
}

void SaveFile(const std::filesystem::path& file, const List& list)
{
    if (file.empty())
        return;

    std::error_code ec;
    if (file.has_parent_path())
    {
        std::filesystem::create_directories(file.parent_path(), ec);
        ec.clear();
    }

    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        ARC_WARN("Recent Scenes: could not open '{}' for write", file.generic_string());
        return;
    }
    const std::string text = Serialize(list);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out)
        ARC_WARN("Recent Scenes: write to '{}' failed", file.generic_string());
}
}
