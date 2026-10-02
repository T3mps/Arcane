#include "Project/StartPageModel.hpp"

#include <ctime>
#include <filesystem>

namespace Arcane::Editor
{
    std::string RelativeOpened(std::uint64_t then, std::uint64_t now)
    {
        if (then == 0) return {};
        if (then >= now || now - then < 60) return "opened just now";
        const std::uint64_t s = now - then;
        auto ago = [](std::uint64_t n, const char* unit)
        { return "opened " + std::to_string(n) + " " + unit + (n == 1 ? "" : "s") + " ago"; };
        if (s < 3600)           return ago(s / 60, "minute");
        if (s < 86400)          return ago(s / 3600, "hour");
        if (s < 2 * 86400)      return "opened yesterday";
        if (s < 30ull * 86400)  return ago(s / 86400, "day");
        std::tm tm{};
        const std::time_t t = static_cast<std::time_t>(then);
#if defined(_WIN32)
        if (localtime_s(&tm, &t) != 0) return {};
#else
        if (!localtime_r(&t, &tm)) return {};
#endif
        char buf[32];
        std::strftime(buf, sizeof buf, "opened on %Y-%m-%d", &tm);
        return buf;
    }

    StartPageModel BuildStartPage(const RecentSelection& selection, std::uint64_t nowUnix)
    {
        StartPageModel m;
        for (const RecentProject& r : selection.visible)
            m.rows.push_back({ r.name, r.path, RelativeOpened(r.lastOpenedUnix, nowUnix) });
        if (selection.hiddenForAbi > 0) m.hiddenLine = Recents::HiddenForAbiLine(selection.hiddenForAbi);
        return m;
    }

    std::string DialogStartDir(const RecentSelection& selection)
    {
        if (selection.visible.empty()) return {};
        std::string p = selection.visible.front().path;
        while (p.size() > 1 && (p.back() == '/' || p.back() == '\\')) p.pop_back();
        std::filesystem::path dir(p);
        if (dir.extension() == ".arcproj") dir = dir.parent_path();   // a manifest names its folder (Project::Open's rule)
        return dir.parent_path().string();
    }
}
