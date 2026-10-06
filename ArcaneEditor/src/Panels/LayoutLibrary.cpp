#include "Panels/LayoutLibrary.hpp"

#include <Arcane/Platform/Paths.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iterator>
#include <system_error>

namespace Arcane::Editor
{
    namespace
    {
        std::string Lower(std::string_view s)
        {
            std::string out(s);
            std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return out;
        }

        bool GuidShaped(std::string_view s)
        {
            if (s.size() != 36) return false;
            for (std::size_t i = 0; i < s.size(); ++i)
            {
                const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
                if (dash ? s[i] != '-' : !std::isxdigit(static_cast<unsigned char>(s[i]))) return false;
            }
            return true;
        }

        std::string_view Trim(std::string_view s)
        {
            while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
            while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
            return s;
        }
    }

    std::optional<std::string> ValidateLayoutName(std::string_view name)
    {
        if (name.empty()) return std::string("Name the layout");
        if (name.size() > 64) return std::string("At most 64 characters");
        if (name.front() == ' ' || name.back() == ' ' || name.back() == '.') return std::string("No leading/trailing space and no trailing dot");
        for (const char c : name)
            if (static_cast<unsigned char>(c) < 0x20 || std::string_view("\\/:*?\"<>|").find(c) != std::string_view::npos)
                return std::string("Not allowed in a file name: \\ / : * ? \" < > |");
        const std::string low = Lower(name);
        static constexpr std::array<std::string_view, 22> kDevices = { "con", "prn", "aux", "nul",
            "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8", "com9",
            "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9" };
        if (low == "." || low == ".." || std::find(kDevices.begin(), kDevices.end(), low) != kDevices.end())
            return std::string("Reserved by Windows");
        if (low == "default" || low == "session" || GuidShaped(name))
            return std::string("Reserved for the per-project session layout");
        return std::nullopt;
    }

    std::vector<std::string> LayoutLibrary::List() const
    {
        std::vector<std::string> out;
        std::error_code ec;
        if (!std::filesystem::is_directory(m_dir, ec)) return out;
        for (const auto& e : std::filesystem::directory_iterator(m_dir, ec))
        {
            if (!e.is_regular_file(ec) || Lower(e.path().extension().string()) != ".ini") continue;
            std::string stem = e.path().stem().string();
            if (!ValidateLayoutName(stem)) out.push_back(std::move(stem));
        }
        std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return Lower(a) < Lower(b); });
        return out;
    }

    bool LayoutLibrary::Exists(std::string_view name) const
    {
        std::error_code ec;
        return std::filesystem::is_regular_file(PathOf(name), ec);
    }

    bool LayoutLibrary::Save(std::string_view name, std::string_view iniText, std::string* error) const
    {
        if (const auto why = ValidateLayoutName(name)) { if (error) *error = *why; return false; }
        std::error_code ec;
        std::filesystem::create_directories(m_dir, ec);
        const std::filesystem::path target = PathOf(name);
        const std::filesystem::path tmp = target.string() + ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            out.write(iniText.data(), static_cast<std::streamsize>(iniText.size()));
            if (!out) { if (error) *error = "cannot write " + tmp.generic_string(); return false; }
        }
        std::filesystem::rename(tmp, target, ec);
        if (ec)
        {
            std::error_code ignored;
            std::filesystem::remove(tmp, ignored);
            if (error) *error = "cannot replace " + target.generic_string() + ": " + ec.message();
            return false;
        }
        return true;
    }

    std::optional<std::string> LayoutLibrary::Load(std::string_view name) const
    {
        std::ifstream in(PathOf(name), std::ios::binary);
        if (!in) return std::nullopt;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    bool LayoutLibrary::Delete(std::string_view name) const
    {
        std::error_code ec;
        return std::filesystem::remove(PathOf(name), ec) && !ec;
    }

    std::filesystem::path NamedLayoutDir()
    {
        return Arcane::Paths::Join(Arcane::Paths::Location::EditorUserDir, Arcane::Paths::Current(), "Layouts");
    }

    std::filesystem::path SessionLayoutDir()
    {
        const std::filesystem::path named = NamedLayoutDir();
        return named.empty() ? std::filesystem::path{} : named / "Session";
    }

    LayoutSeed SeedSessionLayout(const std::filesystem::path& target, const std::filesystem::path& preS4File,
                                 const LayoutLibrary& named, std::string_view defaultName,
                                 const std::filesystem::path& legacyExeIni)
    {
        std::error_code ec;
        std::filesystem::create_directories(target.parent_path(), ec);
        const auto copy = [&](const std::filesystem::path& from)
        {
            std::error_code e;
            return !from.empty() && std::filesystem::is_regular_file(from, e)
                && std::filesystem::copy_file(from, target, e) && !e;
        };
        if (copy(preS4File)) return LayoutSeed::PreS4File;
        if (!defaultName.empty() && named.Exists(defaultName) && copy(named.PathOf(defaultName))) return LayoutSeed::NamedDefault;
        if (copy(legacyExeIni)) return LayoutSeed::LegacyExeIni;
        return LayoutSeed::None;
    }

    PanelVisibility ParseOpenPanels(std::string_view list)
    {
        PanelVisibility vis;
        if (Trim(list) == "*") return vis;
        vis.visible.fill(false);
        std::size_t start = 0;
        for (std::size_t i = 0; i <= list.size(); ++i)
        {
            if (i != list.size() && list[i] != ',') continue;
            const std::string item = Lower(Trim(list.substr(start, i - start)));
            for (const PanelInfo& p : kPanels)
                if (Lower(p.name) == item) vis.visible[static_cast<std::size_t>(p.id)] = true;
            start = i + 1;
        }
        return vis;
    }

    std::string FormatOpenPanels(const PanelVisibility& vis)
    {
        std::string out;
        bool all = true;
        for (const PanelInfo& p : kPanels)
        {
            if (p.permanent) continue;
            if (!vis.visible[static_cast<std::size_t>(p.id)]) { all = false; continue; }
            if (!out.empty()) out += ',';
            out += p.name;
        }
        return all ? std::string("*") : out;
    }
}
