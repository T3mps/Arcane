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

        // The file key's field ("panel", "graph.nodeBody"), or empty when the
        // key names no token.
        std::string_view TokenField(std::string_view cvarName)
        {
            if (const ThemeToken* t = FindToken(cvarName)) return t->field;
            constexpr std::string_view kPrefix = "editor.theme.";
            if (!cvarName.starts_with(kPrefix)) return {};
            if (const GraphThemeTokenInfo* g = FindGraphThemeToken(cvarName.substr(kPrefix.size()))) return g->field;
            return {};
        }

        bool WriteThemeJson(const std::filesystem::path& path, std::string_view name, const Theme::Palette& palette,
                            const GraphThemeColors* graph, std::string* error)
        {
            nlohmann::ordered_json doc;
            doc["format"]  = "arctheme";
            doc["version"] = 1;
            doc["name"]    = std::string(name);
            nlohmann::ordered_json colors = nlohmann::ordered_json::object();
            for (const ThemeToken& t : kThemeTokens)
                colors[ThemeCvarName(t.field)] = FormatHexColor(palette.*(t.palette));
            if (graph)
                for (const GraphThemeTokenInfo& t : GraphThemeTokens())
                    colors[ThemeCvarName(t.field)] = FormatHexColor(ResolveGraphThemeColor(*graph, t));
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
            const std::string_view field = TokenField(it.key());
            if (field.empty()) { out.unknownKeys.push_back(it.key()); continue; }
            const std::optional<ImVec4> c = it->is_string() ? ParseHexColor(it->get<std::string>()) : std::nullopt;
            if (!c) return std::unexpected(it.key() + ": expected \"#rrggbb\" or \"#rrggbbaa\"");
            out.colors.emplace_back(std::string(field), *c);
        }
        return out;
    }

    bool WriteThemeFile(const std::filesystem::path& path, std::string_view name, const Theme::Palette& palette,
                        std::string* error)
    {
        return WriteThemeJson(path, name, palette, nullptr, error);
    }

    bool WriteThemeFile(const std::filesystem::path& path, std::string_view name, const Theme::Palette& palette,
                        const GraphThemeColors& graph, std::string* error)
    {
        return WriteThemeJson(path, name, palette, &graph, error);
    }

    EditorThemeSettings ApplyThemeFileTo(const ThemeFile& file, EditorThemeSettings base)
    {
        for (const auto& [field, colour] : file.colors)
            for (const ThemeToken& t : kThemeTokens)
                if (t.field == field) { base.*(t.setting) = ToSettingColor(colour); break; }
        return base;
    }

    GraphThemeColors ApplyThemeFileTo(const ThemeFile& file, GraphThemeColors base)
    {
        for (const auto& [field, colour] : file.colors)
            if (const GraphThemeTokenInfo* t = FindGraphThemeToken(field))
                GraphThemeSlot(base, *t) = ToSettingColor(colour);
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

    std::vector<ContrastRow> ContrastReport(const Theme::Palette& palette, const GraphThemeColors& graph)
    {
        std::vector<ContrastRow> rows = ContrastReport(palette);
        const auto colour = [&graph](std::string_view field) { return ResolveGraphThemeColor(graph, *FindGraphThemeToken(field)); };
        const auto add = [&rows](std::string_view label, const ImVec4& fg, const ImVec4& bg, float minRatio)
        {
            const float ratio = Theme::ContrastRatio(fg, bg);
            rows.push_back({ label, ratio, minRatio, ratio >= minRatio });
        };
        const ImVec4 text  = colour("graph.nodeTitleText");
        const ImVec4 body  = colour("graph.nodeBody");
        const ImVec4 title = colour("graph.nodeTitle");
        add("Node text on node body",          text, body,  4.5f);
        add("Node text on node title",         text, title, 4.5f);
        add("Node error title on node title",  colour("graph.nodeBadgeText"), title, 4.5f);
        // The category bands carry the same title text (s5.1.4).
        add("Node text on Input band",         text, colour("graph.category.input"),         4.5f);
        add("Node text on Math band",          text, colour("graph.category.math"),          4.5f);
        add("Node text on Vector band",        text, colour("graph.category.vector"),        4.5f);
        add("Node text on Procedural band",    text, colour("graph.category.procedural"),    4.5f);
        add("Node text on Output band",        text, colour("graph.category.output"),        4.5f);
        add("Node text on Utility band",       text, colour("graph.category.utility"),       4.5f);
        add("Node text on Uncategorized band", text, colour("graph.category.uncategorized"), 4.5f);
        // Pins are marks: 3:1 on the body they sit on.
        add("float pin on node body",          colour("graph.pinScalar"),  body, 3.0f);
        add("float2 pin on node body",         colour("graph.pinVec2"),    body, 3.0f);
        add("float4 pin on node body",         colour("graph.pinVec4"),    body, 3.0f);
        add("Dynamic pin on node body",        colour("graph.pinDynamic"), body, 3.0f);
        add("Render target pin on node body",  colour("graph.pinTexture"), body, 3.0f);
        return rows;
    }
}
