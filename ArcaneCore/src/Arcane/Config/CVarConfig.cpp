#include <Arcane/Config/CVarConfig.hpp>

#include <Arcane/Base/Log.hpp>

#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <system_error>
#include <utility>

namespace Arcane
{
    namespace
    {
        bool IsDocumentCategory(std::string_view category)
        {
            return category == "input";
        }

        // The value the User rung holds: its newest record, whatever rung
        // currently wins. nullptr when the User rung never set this cvar.
        const CVarValue* NewestUserValue(const CVarExplain& explained)
        {
            for (auto it = explained.history.rbegin(); it != explained.history.rend(); ++it)
                if (it->by == SetBy::User)
                    return &it->value;
            return nullptr;
        }

        nlohmann::json ArchiveJson(const CVarValue& value)
        {
            switch (value.type)
            {
            case CVarType::Bool: return value.AsBool();
            case CVarType::Int32: return value.AsInt32();
            case CVarType::UInt32: return value.AsUInt32();
            case CVarType::Int64: return value.AsInt64();
            case CVarType::UInt64: return value.AsUInt64();
            case CVarType::Float32: return value.AsFloat32();
            case CVarType::Float64: return value.AsFloat64();
            case CVarType::String: return value.AsString();
            default: return nullptr;
            }
        }

        // The leaf `key` names in `doc` the way Walk reads it: the flat key
        // itself, or nested objects whose names join with dots to `key`.
        // nullptr when the document holds no such leaf.
        nlohmann::json* FindLeaf(nlohmann::json& doc, std::string_view key)
        {
            if (!doc.is_object()) return nullptr;
            if (auto it = doc.find(std::string(key)); it != doc.end() && !it->is_object())
                return &*it;
            for (std::size_t dot = key.find('.'); dot != std::string_view::npos; dot = key.find('.', dot + 1))
            {
                auto it = doc.find(std::string(key.substr(0, dot)));
                if (it == doc.end() || !it->is_object()) continue;
                if (nlohmann::json* leaf = FindLeaf(*it, key.substr(dot + 1)))
                    return leaf;
            }
            return nullptr;
        }

        std::optional<std::string> ReadWholeFile(const std::filesystem::path& file)
        {
            std::ifstream in(file, std::ios::binary);
            if (!in) return std::nullopt;
            return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }

        void Walk(CVarRegistry& registry, const std::string& prefix, const nlohmann::json& node,
                  SetBy by, std::string_view sourceModule, std::vector<std::string>& unknown)
        {
            if (!node.is_object()) return;
            for (auto it = node.begin(); it != node.end(); ++it)
            {
                const std::string name = prefix.empty() ? it.key() : prefix + "." + it.key();
                if (it->is_object())
                {
                    Walk(registry, name, *it, by, sourceModule, unknown);
                    continue;
                }
                const CVarHandle handle = registry.Find(name);
                if (handle.IsStale())
                {
                    unknown.push_back(name);
                    continue;
                }
                CVarValue value = CVarValue::Bool(false);
                const auto current = registry.Get(handle);
                if (!current) continue;
                switch (current->type)
                {
                case CVarType::Bool:
                    if (!it->is_boolean()) { unknown.push_back(name); continue; }
                    value = CVarValue::Bool(it->get<bool>());
                    break;
                case CVarType::Int32:
                    if (!it->is_number_integer()) { unknown.push_back(name); continue; }
                    value = CVarValue::Int32(it->get<std::int32_t>());
                    break;
                case CVarType::UInt32:
                    if (!it->is_number_unsigned() && !it->is_number_integer()) { unknown.push_back(name); continue; }
                    value = CVarValue::UInt32(it->get<std::uint32_t>());
                    break;
                case CVarType::Int64:
                    if (!it->is_number_integer()) { unknown.push_back(name); continue; }
                    value = CVarValue::Int64(it->get<std::int64_t>());
                    break;
                case CVarType::UInt64:
                    if (!it->is_number_unsigned() && !it->is_number_integer()) { unknown.push_back(name); continue; }
                    value = CVarValue::UInt64(it->get<std::uint64_t>());
                    break;
                case CVarType::Float32:
                    if (!it->is_number()) { unknown.push_back(name); continue; }
                    value = CVarValue::Float32(it->get<float>());
                    break;
                case CVarType::Float64:
                    if (!it->is_number()) { unknown.push_back(name); continue; }
                    value = CVarValue::Float64(it->get<double>());
                    break;
                case CVarType::String:
                    if (!it->is_string()) { unknown.push_back(name); continue; }
                    value = CVarValue::String(it->get<std::string>());
                    break;
                default:
                    unknown.push_back(name);
                    continue;
                }
                registry.Set(handle, std::move(value), by, sourceModule, Permission::Editor);
            }
        }
    }

    CVarApplyReport ApplyCVarCategory(CVarRegistry& registry, std::string_view category,
                                      const nlohmann::json& doc, SetBy by, bool documentShaped,
                                      std::string_view sourceModule)
    {
        CVarApplyReport report;
        if (documentShaped || !doc.is_object()) return report;
        Walk(registry, std::string(category), doc, by, sourceModule, report.unknownKeys);
        return report;
    }

    CVarApplyReport ApplyCVarDirectory(CVarRegistry& registry, const std::filesystem::path& dir,
                                       SetBy by, std::string_view sourceModule)
    {
        CVarApplyReport report;
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec)) return report;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
        {
            if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
            std::ifstream in(entry.path(), std::ios::binary);
            if (!in) continue;
            auto doc = nlohmann::json::parse(in, nullptr, false);
            if (doc.is_discarded()) continue;
            const std::string stem = entry.path().stem().string();
            auto part = ApplyCVarCategory(registry, stem, doc, by, IsDocumentCategory(stem), sourceModule);
            report.unknownKeys.insert(report.unknownKeys.end(), part.unknownKeys.begin(), part.unknownKeys.end());
        }
        return report;
    }

    void WriteCVarArchive(const CVarRegistry& registry, const std::filesystem::path& userDir)
    {
        // category -> its owned (key, value) pairs; ordered, so the writes are too.
        std::map<std::string, std::vector<std::pair<std::string, nlohmann::json>>> owned;
        for (const CVarListEntry& entry : registry.List())
        {
            if (!HasFlag(entry.flags, CVarFlags::Archive)) continue;
            if (HasFlag(entry.flags, CVarFlags::Dev) || HasFlag(entry.flags, CVarFlags::Cheat)) continue;
            const auto dot = entry.name.find('.');
            if (dot == std::string::npos) continue;
            std::string category = entry.name.substr(0, dot);
            if (IsDocumentCategory(category)) continue;
            const auto explained = registry.Explain(entry.name);
            if (!explained) continue;
            const CVarValue* value = NewestUserValue(*explained);
            if (!value) continue;
            nlohmann::json json = ArchiveJson(*value);
            if (json.is_null()) continue;
            owned[std::move(category)].emplace_back(entry.name.substr(dot + 1), std::move(json));
        }
        if (owned.empty()) return;
        std::error_code ec;
        std::filesystem::create_directories(userDir, ec);
        for (auto& [category, values] : owned)
        {
            const std::filesystem::path file = userDir / (category + ".json");
            const std::optional<std::string> before = ReadWholeFile(file);
            nlohmann::json doc = nlohmann::json::object();
            if (before)
            {
                auto parsed = nlohmann::json::parse(*before, nullptr, false);
                if (!parsed.is_discarded() && parsed.is_object())
                    doc = std::move(parsed);
                else
                {
                    std::filesystem::path bad = file;
                    bad += ".bad";
                    std::error_code copied;
                    std::filesystem::copy_file(file, bad, std::filesystem::copy_options::overwrite_existing, copied);
                    if (copied)
                    {
                        // No backup, no overwrite: the unparsable file may be the
                        // user's only copy of a hand edit.
                        ARC_WARN("cvar: '{}' is not a JSON object and could not be kept as '{}' ({}) -- left untouched, not saved",
                                 file.generic_string(), bad.generic_string(), copied.message());
                        continue;
                    }
                    ARC_WARN("cvar: '{}' is not a JSON object -- kept as '{}', replaced",
                             file.generic_string(), bad.generic_string());
                }
            }
            for (auto& [key, value] : values)
            {
                if (nlohmann::json* leaf = FindLeaf(doc, key))
                    *leaf = std::move(value);
                else
                    doc[key] = std::move(value);
            }
            const std::string text = doc.dump(2);
            if (before && *before == text) continue;
            std::filesystem::path tmp = file;
            tmp += ".tmp";
            bool written = false;
            std::error_code renamed;
            {
                std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
                out << text;
                out.flush();
                written = static_cast<bool>(out);
            }
            if (written)
                std::filesystem::rename(tmp, file, renamed);   // replaces: the old file or the new, never half of one
            if (!written || renamed)
            {
                ARC_WARN("cvar: cannot write '{}'{}{}", file.generic_string(), renamed ? ": " : "",
                         renamed ? renamed.message() : std::string());
                std::error_code ignored;
                std::filesystem::remove(tmp, ignored);
            }
        }
    }

    void ApplyCVarCommandLine(CVarRegistry& registry, const std::vector<std::string>& sets, Permission permission)
    {
        for (const std::string& item : sets)
        {
            const auto eq = item.find('=');
            if (eq == std::string::npos || eq == 0)
            {
                ARC_WARN("cvar: --set '{}' is not name=value", item);
                continue;
            }
            const std::string name = item.substr(0, eq);
            const std::string value = item.substr(eq + 1);
            const ExecResult result = registry.Execute(name + " " + value, permission, SetBy::CommandLine);
            if (!result.ok) ARC_WARN("cvar: --set {}: {}", name, result.text);
        }
    }
}
