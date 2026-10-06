#include "Settings/ThemePresets.hpp"

#include "Project/ModuleBuild.hpp"

#include <Arcane/Config/CVarRegistry.hpp>

#include <Json.hpp>

#include <charconv>
#include <cstdio>
#include <fstream>
#include <iterator>

namespace Arcane::Editor
{
    namespace
    {
        const ThemeToken* FindToken(std::string_view cvarName)
        {
            constexpr std::string_view kPrefix = "editor.theme.";
            if (!cvarName.starts_with(kPrefix)) return nullptr;
            const std::string_view field = cvarName.substr(kPrefix.size());
            for (const ThemeToken& t : kThemeTokens)
                if (t.field == field) return &t;
            return nullptr;
        }
    }

    std::optional<ImVec4> ParseHexColor(std::string_view text)
    {
        if ((text.size() != 7 && text.size() != 9) || text[0] != '#') return std::nullopt;
        unsigned v[4] = { 0, 0, 0, 255 };
        const std::size_t channels = (text.size() - 1) / 2;
        for (std::size_t i = 0; i < channels; ++i)
        {
            const char* first = text.data() + 1 + i * 2;
            const char* last  = first + 2;
            const auto r = std::from_chars(first, last, v[i], 16);
            if (r.ec != std::errc{} || r.ptr != last) return std::nullopt;
        }
        return ImVec4(v[0] / 255.0f, v[1] / 255.0f, v[2] / 255.0f, v[3] / 255.0f);
    }

    std::string FormatHexColor(const ImVec4& c)
    {
        const ImU32 u = ImGui::ColorConvertFloat4ToU32(c);
        char buf[10];
        std::snprintf(buf, sizeof buf, "#%02x%02x%02x%02x",
                      static_cast<unsigned>((u >> IM_COL32_R_SHIFT) & 0xFFu), static_cast<unsigned>((u >> IM_COL32_G_SHIFT) & 0xFFu),
                      static_cast<unsigned>((u >> IM_COL32_B_SHIFT) & 0xFFu), static_cast<unsigned>((u >> IM_COL32_A_SHIFT) & 0xFFu));
        return buf;
    }

    std::expected<ThemeFile, std::string> ReadThemeFile(const std::filesystem::path& path)
    {
        const std::string fileName = path.filename().generic_string();
        std::ifstream in(path, std::ios::binary);
        if (!in) return std::unexpected("cannot open " + path.generic_string());
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const nlohmann::json doc = nlohmann::json::parse(text, nullptr, /*allow_exceptions*/ false);
        if (doc.is_discarded() || !doc.is_object()) return std::unexpected(fileName + " is not a JSON object");
        const auto format = doc.find("format");
        if (format == doc.end() || !format->is_string() || format->get<std::string>() != "arctheme")
            return std::unexpected(fileName + ": \"format\" must be \"arctheme\"");
        const auto version = doc.find("version");
        if (version == doc.end() || !version->is_number_integer() || version->get<int>() != 1)
            return std::unexpected(fileName + ": unsupported \"version\" (this editor reads 1)");
        const auto colors = doc.find("colors");
        if (colors == doc.end() || !colors->is_object()) return std::unexpected(fileName + " has no \"colors\" object");

        ThemeFile out;
        const auto name = doc.find("name");
        out.name = (name != doc.end() && name->is_string()) ? name->get<std::string>() : path.stem().string();
        for (auto it = colors->begin(); it != colors->end(); ++it)
        {
            const ThemeToken* token = FindToken(it.key());
            if (!token) { out.unknownKeys.push_back(it.key()); continue; }
            const std::optional<ImVec4> c = it->is_string() ? ParseHexColor(it->get<std::string>()) : std::nullopt;
            if (!c) return std::unexpected(it.key() + ": expected \"#rrggbb\" or \"#rrggbbaa\"");
            out.colors.emplace_back(std::string(token->field), *c);
        }
        return out;
    }

    bool WriteThemeFile(const std::filesystem::path& path, std::string_view name, const Theme::Palette& palette,
                        std::string* error)
    {
        nlohmann::ordered_json doc;
        doc["format"]  = "arctheme";
        doc["version"] = 1;
        doc["name"]    = std::string(name);
        nlohmann::ordered_json colors = nlohmann::ordered_json::object();
        for (const ThemeToken& t : kThemeTokens)
            colors[ThemeCvarName(t.field)] = FormatHexColor(palette.*(t.palette));
        doc["colors"] = std::move(colors);

        const std::filesystem::path tmp = path.string() + ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            out << doc.dump(2) << '\n';
            if (!out)
            {
                if (error) *error = "cannot write " + tmp.generic_string();
                return false;
            }
        }
        std::error_code ec;
        std::filesystem::rename(tmp, path, ec);   // atomic replace on one volume
        if (ec)
        {
            std::error_code ignored;
            std::filesystem::remove(tmp, ignored);
            if (error) *error = "cannot replace " + path.generic_string() + ": " + ec.message();
            return false;
        }
        return true;
    }

    EditorThemeSettings ApplyThemeFileTo(const ThemeFile& file, EditorThemeSettings base)
    {
        for (const auto& [field, colour] : file.colors)
            for (const ThemeToken& t : kThemeTokens)
                if (t.field == field) { base.*(t.setting) = ToSettingColor(colour); break; }
        return base;
    }

    std::size_t ApplyThemeToRegistry(const ThemeFile& file, Arcane::CVarRegistry& registry)
    {
        std::size_t applied = 0;
        for (const auto& [field, colour] : file.colors)
        {
            const Arcane::CVarHandle h = registry.Find(ThemeCvarName(field));
            if (h.IsStale()) continue;
            if (registry.Set(h, Arcane::CVarValue::Color(ToSettingColor(colour)), Arcane::SetBy::EditorUser,
                             "editor", Arcane::CVarContext::Editor) == Arcane::SetResult::Applied)
                ++applied;
        }
        return applied;
    }

    std::filesystem::path ThemePresetDir() { return ModuleBuild::ExeDir() / "data" / "EditorThemes"; }

    std::vector<ContrastRow> ContrastReport(const Theme::Palette& p)
    {
        std::vector<ContrastRow> rows;
        rows.reserve(kThemeContrastPairs.size());
        for (const ThemeContrastPair& pair : kThemeContrastPairs)
        {
            const float ratio = Theme::ContrastRatio(p.*(pair.fg), p.*(pair.bg));
            rows.push_back({ pair.label, ratio, pair.minRatio, ratio >= pair.minRatio });
        }
        return rows;
    }
}
