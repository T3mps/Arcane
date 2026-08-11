#include "Project/RecentProjects.hpp"

#include <Json.hpp>   // the workspace's vendored nlohmann::json header

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <optional>
#include <sstream>
#include <system_error>

#if defined(_WIN32)
#include <process.h>   // _getpid -- the temp-name tag; far lighter than <windows.h>
#include <cstdlib>     // _wgetenv
#define ARC_RECENTS_GETPID _getpid
#else
#include <unistd.h>
#define ARC_RECENTS_GETPID getpid
#endif

namespace Arcane::Editor::Recents
{
namespace
{
    // Mirrors store.rs STATE_FORMAT_VERSION. A document numbered ABOVE this was
    // written by a newer Hub: we read it as empty and never rewrite it, which is
    // that file's own rule applied from this side.
    constexpr std::uint32_t kFormatVersion = 1;

    constexpr std::string_view kProjExt = ".arcproj";

    // nullopt == the file EXISTS but could not be read. Distinct from an empty
    // string (no file yet, first run) because the two demand opposite writes:
    // a fresh document is correct for the second and destructive for the first.
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

    // Parsed document, or nullopt when the caller must not write it back:
    // unparseable (the Hub quarantines those, and clobbering destroys the
    // evidence) or a newer format (it belongs to a Hub that knows more).
    std::optional<nlohmann::json> ParseDocument(const std::string& text)
    {
        nlohmann::json doc = nlohmann::json::parse(text, nullptr, /*allow_exceptions*/ false);
        if (doc.is_discarded() || !doc.is_object())
            return std::nullopt;

        // Version gate BEFORE touching `items`: a newer format's entries need
        // not mean what this build assumes they mean.
        if (const auto v = doc.find("version"); v != doc.end() && v->is_number_unsigned())
        {
            if (v->get<std::uint32_t>() > kFormatVersion)
                return std::nullopt;
        }
        return doc;
    }
}

std::string NormalisePath(std::string_view p)
{
    std::string s(p);
    std::replace(s.begin(), s.end(), '\\', '/');
    while (!s.empty() && s.back() == '/')
        s.pop_back();
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string ProjectDirKey(std::string_view p)
{
    std::string norm = NormalisePath(p);
    const std::size_t slash = norm.rfind('/');
    if (slash != std::string::npos && slash > 0)
    {
        const std::string_view file(norm.data() + slash + 1, norm.size() - slash - 1);
        if (file.size() > kProjExt.size() &&
            file.substr(file.size() - kProjExt.size()) == kProjExt)
            return norm.substr(0, slash);
    }
    return norm;
}

std::filesystem::path DefaultFile()
{
#if defined(_WIN32)
    // %LOCALAPPDATA%, not %APPDATA%: every path in this file is a
    // machine-specific absolute path, so the Hub deliberately keeps it
    // machine-local (paths.rs).
    if (const wchar_t* localAppData = _wgetenv(L"LOCALAPPDATA"); localAppData && *localAppData)
        return std::filesystem::path(localAppData) / L"Arcane" / L"hub" / L"recents.archub";
#endif
    return {};
}

std::vector<RecentProject> Parse(const std::string& json)
{
    std::vector<RecentProject> out;
    if (json.empty())
        return out;

    const std::optional<nlohmann::json> doc = ParseDocument(json);
    if (!doc)
        return out;

    const auto items = doc->find("items");
    if (items == doc->end() || !items->is_array())
        return out;

    out.reserve(items->size());
    for (const nlohmann::json& e : *items)
    {
        if (!e.is_object())
            continue;

        RecentProject r;
        const auto path = e.find("path");
        if (path == e.end() || !path->is_string())
            continue;                       // no path == nothing to open
        r.path = path->get<std::string>();
        if (r.path.empty())
            continue;

        if (const auto it = e.find("name"); it != e.end() && it->is_string())
            r.name = it->get<std::string>();
        if (const auto it = e.find("engineAbi"); it != e.end() && it->is_number_unsigned())
            r.engineAbi = it->get<std::uint32_t>();

        // A nameless row still deserves a readable label rather than a blank
        // menu entry. Unreal labels with the base filename for the same reason.
        if (r.name.empty())
        {
            std::filesystem::path fsPath(r.path);
            if (fsPath.extension() == kProjExt)
                fsPath = fsPath.parent_path();
            r.name = fsPath.filename().string();
        }

        out.push_back(std::move(r));
    }
    return out;
}

std::vector<RecentProject> Load(const std::filesystem::path& file)
{
    if (file.empty())
        return {};
    const std::optional<std::string> text = ReadWhole(file);
    if (!text)
        return {};
    return Parse(*text);
}

RecentSelection Select(const std::vector<RecentProject>& all,
                       std::uint32_t thisAbi,
                       std::string_view currentProjectPath,
                       const std::function<bool(const std::string&)>& exists,
                       std::size_t cap)
{
    RecentSelection sel;

    const std::string currentKey =
        currentProjectPath.empty() ? std::string() : ProjectDirKey(currentProjectPath);

    std::vector<std::string> seen;
    seen.reserve(all.size());

    for (const RecentProject& r : all)
    {
        const std::string key = ProjectDirKey(r.path);

        // The currently-open project. Unreal drops this one too
        // (FPaths::GetProjectFilePath() == ProjectName -> continue): offering to
        // reopen what is already open is a no-op at best.
        if (!currentKey.empty() && key == currentKey)
            continue;

        // Only reachable from a hand-edited file, but one duplicated row must
        // not become two identical menu entries.
        if (std::find(seen.begin(), seen.end(), key) != seen.end())
            continue;
        seen.push_back(key);

        // ABI is tested FIRST and counted. It is the only exclusion the user is
        // told about, so a silent exclusion must not be able to swallow it and
        // make the count under-report.
        if (r.engineAbi != thisAbi)
        {
            ++sel.hiddenForAbi;
            continue;
        }

        // Disk truth NOW. The Hub stamps a `missing` flag on its own reads, but
        // that is a snapshot and may be stale by the time this menu opens.
        if (exists && !exists(r.path))
            continue;

        if (sel.visible.size() < cap)
            sel.visible.push_back(r);
    }
    return sel;
}

std::string Touch(const std::string& json,
                  const std::string& projectPath,
                  const std::string& name,
                  std::uint32_t abi,
                  std::uint64_t nowUnixSeconds)
{
    if (projectPath.empty())
        return {};

    nlohmann::json doc;
    if (json.empty())
    {
        doc = nlohmann::json{{"version", kFormatVersion}, {"items", nlohmann::json::array()}};
    }
    else
    {
        std::optional<nlohmann::json> parsed = ParseDocument(json);
        if (!parsed)
            return {};                       // unparseable or newer: do not write
        doc = std::move(*parsed);
        if (!doc.contains("items") || !doc["items"].is_array())
            doc["items"] = nlohmann::json::array();
    }

    nlohmann::json& items = doc["items"];
    const std::string key = ProjectDirKey(projectPath);

    // Reuse the EXISTING row when there is one. It carries the user's engine
    // pin, saved arguments, star and guid; rebuilding from scratch would reset
    // all of them silently -- state.rs touch_recent carries the same fields
    // forward for exactly this reason.
    nlohmann::json entry;
    for (auto it = items.begin(); it != items.end(); ++it)
    {
        if (!it->is_object())
            continue;
        const auto p = it->find("path");
        if (p == it->end() || !p->is_string())
            continue;
        if (ProjectDirKey(p->get<std::string>()) != key)
            continue;
        entry = *it;
        items.erase(it);
        break;
    }

    if (entry.is_null())
    {
        // A NEW row must carry the Hub's whole field set: its typed serde parse
        // rejects a row missing a non-defaulted field, and one rejected row
        // quarantines the user's entire list.
        entry             = nlohmann::json::object();
        entry["path"]     = projectPath;
        entry["engineId"] = nullptr;
        entry["args"]     = "";
        entry["favorite"] = false;
        entry["guid"]     = nullptr;
    }

    entry["lastOpenedUtc"] = std::to_string(nowUnixSeconds);   // UNIX SECONDS as text
    entry["engineAbi"]     = abi;
    entry["missing"]       = false;   // we just opened it; it is demonstrably present
    if (!name.empty())
        entry["name"] = name;
    else if (!entry.contains("name"))
        entry["name"] = std::filesystem::path(projectPath).filename().string();

    items.insert(items.begin(), std::move(entry));   // newest-first BY POSITION
    doc["version"] = kFormatVersion;

    return doc.dump(2);   // 2-space, matching serde_json::to_string_pretty
}

bool WriteAtomic(const std::filesystem::path& file, const std::string& text)
{
    if (file.empty() || text.empty())
        return false;

    std::error_code ec;
    if (file.has_parent_path())
    {
        std::filesystem::create_directories(file.parent_path(), ec);
        ec.clear();
    }

    // PID in the temp name for the Hub's own stated reason: two processes
    // sharing one scratch file would let either publish the other's
    // half-written data -- no corruption thanks to the rename, but a silent
    // wrong result.
    std::filesystem::path tmp = file;
    tmp += ("." + std::to_string(ARC_RECENTS_GETPID()) + ".tmp");

    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.flush();
        if (!out)
        {
            out.close();
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }

    // Replaces an existing file on Windows, so the target is either the whole
    // old document or the whole new one -- never a truncated mix.
    std::filesystem::rename(tmp, file, ec);
    if (ec)
    {
        std::error_code rmEc;
        std::filesystem::remove(tmp, rmEc);   // leave no stray temp behind
        return false;
    }
    return true;
}

void TouchFile(const std::filesystem::path& file,
               const std::string& projectPath,
               const std::string& name,
               std::uint32_t abi)
{
    if (file.empty() || projectPath.empty())
        return;

    const std::optional<std::string> before = ReadWhole(file);
    if (!before)
        return;   // exists but unreadable: never replace what we could not read

    const auto now = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    const std::string after = Touch(*before, projectPath, name, abi, now);
    if (after.empty())
        return;   // unparseable or newer format: leave it alone

    (void)WriteAtomic(file, after);   // a recents list is never worth failing an open
}
}
