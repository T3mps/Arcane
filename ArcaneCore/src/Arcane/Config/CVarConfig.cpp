#include <Arcane/Config/CVarConfig.hpp>

#include <Arcane/Base/Log.hpp>

#include <fstream>
#include <system_error>
#include <unordered_map>

namespace Arcane
{
    namespace
    {
        bool IsDocumentCategory(std::string_view category)
        {
            return category == "input";
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
        std::unordered_map<std::string, nlohmann::json> docs;
        for (const CVarListEntry& entry : registry.List())
        {
            if (!Any(entry.flags, CVarFlags::Archive)) continue;
            const auto explained = registry.Explain(entry.name);
            if (!explained || explained->setBy < SetBy::User) continue;
            const auto dot = entry.name.find('.');
            if (dot == std::string::npos) continue;
            const std::string category = entry.name.substr(0, dot);
            const std::string key = entry.name.substr(dot + 1);
            nlohmann::json& doc = docs[category];
            if (!doc.is_object()) doc = nlohmann::json::object();
            const CVarValue& value = explained->published;
            switch (value.type)
            {
            case CVarType::Bool: doc[key] = value.AsBool(); break;
            case CVarType::Int32: doc[key] = value.AsInt32(); break;
            case CVarType::UInt32: doc[key] = value.AsUInt32(); break;
            case CVarType::Int64: doc[key] = value.AsInt64(); break;
            case CVarType::UInt64: doc[key] = value.AsUInt64(); break;
            case CVarType::Float32: doc[key] = value.AsFloat32(); break;
            case CVarType::Float64: doc[key] = value.AsFloat64(); break;
            case CVarType::String: doc[key] = value.AsString(); break;
            default: break;
            }
        }
        if (docs.empty()) return;
        std::error_code ec;
        std::filesystem::create_directories(userDir, ec);
        for (const auto& [category, doc] : docs)
        {
            std::ofstream out(userDir / (category + ".json"), std::ios::binary);
            if (!out) continue;
            out << doc.dump(2);
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
