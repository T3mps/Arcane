#include <Arcane/Config/CVarConfig.hpp>

#include <Arcane/Base/Diagnostics.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Config/CVarFormat.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <system_error>
#include <tuple>
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

        nlohmann::json ArchiveJson(const CVarValue& value, const std::vector<std::string>& enumNames)
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
            case CVarType::Color: return CVarColorToHex(value.AsColor());
            case CVarType::Vec2: { const CVarVec2 v = value.AsVec2(); return nlohmann::json::array({ v.x, v.y }); }
            case CVarType::Vec3: { const CVarVec3 v = value.AsVec3(); return nlohmann::json::array({ v.x, v.y, v.z }); }
            case CVarType::Vec4: { const CVarVec4 v = value.AsVec4(); return nlohmann::json::array({ v.x, v.y, v.z, v.w }); }
            case CVarType::Enum:
            {
                const std::int32_t ordinal = value.AsEnum();
                if (ordinal >= 0 && static_cast<std::size_t>(ordinal) < enumNames.size())
                    return enumNames[static_cast<std::size_t>(ordinal)];
                return ordinal;
            }
            default: return nullptr;
            }
        }

        // Exactly `count` numbers from a JSON array.
        bool JsonFloats(const nlohmann::json& j, float* out, std::size_t count)
        {
            if (!j.is_array() || j.size() != count) return false;
            for (std::size_t i = 0; i < count; ++i)
            {
                if (!j[i].is_number()) return false;
                out[i] = j[i].get<float>();
            }
            return true;
        }

        // A file value as `type`; nullopt = the wrong shape (spec s12: refused
        // and reported). `numericEnum` is set when an Enum came as a number.
        std::optional<CVarValue> ValueFromJson(const nlohmann::json& j, CVarType type, const std::vector<std::string>& enumNames,
                                               std::optional<std::int32_t>& numericEnum)
        {
            switch (type)
            {
            case CVarType::Bool:
                if (!j.is_boolean()) return std::nullopt;
                return CVarValue::Bool(j.get<bool>());
            case CVarType::Int32:
                if (!j.is_number_integer()) return std::nullopt;
                return CVarValue::Int32(j.get<std::int32_t>());
            case CVarType::UInt32:
                if (!j.is_number_unsigned() && !j.is_number_integer()) return std::nullopt;
                return CVarValue::UInt32(j.get<std::uint32_t>());
            case CVarType::Int64:
                if (!j.is_number_integer()) return std::nullopt;
                return CVarValue::Int64(j.get<std::int64_t>());
            case CVarType::UInt64:
                if (!j.is_number_unsigned() && !j.is_number_integer()) return std::nullopt;
                return CVarValue::UInt64(j.get<std::uint64_t>());
            case CVarType::Float32:
                if (!j.is_number()) return std::nullopt;
                return CVarValue::Float32(j.get<float>());
            case CVarType::Float64:
                if (!j.is_number()) return std::nullopt;
                return CVarValue::Float64(j.get<double>());
            case CVarType::String:
                if (!j.is_string()) return std::nullopt;
                return CVarValue::String(j.get<std::string>());
            case CVarType::Color:
            {
                if (j.is_string())
                {
                    if (const auto c = CVarColorFromHex(j.get<std::string>())) return CVarValue::Color(*c);
                    return std::nullopt;
                }
                float f[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
                if (JsonFloats(j, f, 3) || JsonFloats(j, f, 4)) return CVarValue::Color(CVarColor{ f[0], f[1], f[2], f[3] });
                return std::nullopt;
            }
            case CVarType::Vec2: { float f[2] = {}; if (!JsonFloats(j, f, 2)) return std::nullopt; return CVarValue::Vec2(CVarVec2{ f[0], f[1] }); }
            case CVarType::Vec3: { float f[3] = {}; if (!JsonFloats(j, f, 3)) return std::nullopt; return CVarValue::Vec3(CVarVec3{ f[0], f[1], f[2] }); }
            case CVarType::Vec4:
            {
                float f[4] = {};
                if (!JsonFloats(j, f, 4)) return std::nullopt;
                return CVarValue::Vec4(CVarVec4{ f[0], f[1], f[2], f[3] });
            }
            case CVarType::Enum:
            {
                if (j.is_string())
                {
                    if (const auto ordinal = CVarEnumOrdinal(enumNames, j.get<std::string>())) return CVarValue::Enum(*ordinal);
                    return std::nullopt;
                }
                if (j.is_number_integer())
                {
                    const std::int64_t n = j.get<std::int64_t>();
                    if (n >= 0 && n < static_cast<std::int64_t>(enumNames.size()))
                    {
                        numericEnum = static_cast<std::int32_t>(n);
                        return CVarValue::Enum(static_cast<std::int32_t>(n));
                    }
                }
                return std::nullopt;
            }
            }
            return std::nullopt;
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

        // Removes the leaf FindLeaf would find; an object it leaves empty goes too.
        bool EraseLeaf(nlohmann::json& doc, std::string_view key)
        {
            if (!doc.is_object()) return false;
            if (auto it = doc.find(std::string(key)); it != doc.end() && !it->is_object())
            {
                doc.erase(it);
                return true;
            }
            for (std::size_t dot = key.find('.'); dot != std::string_view::npos; dot = key.find('.', dot + 1))
            {
                auto it = doc.find(std::string(key.substr(0, dot)));
                if (it == doc.end() || !it->is_object()) continue;
                if (EraseLeaf(*it, key.substr(dot + 1)))
                {
                    if (it->empty()) doc.erase(it);
                    return true;
                }
            }
            return false;
        }

        std::optional<std::string> ReadWholeFile(const std::filesystem::path& file)
        {
            std::ifstream in(file, std::ios::binary);
            if (!in) return std::nullopt;
            return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }

        // 1-based line of a key's first mention: the path relative to the
        // category (a flat "graph.x"), else its last segment (nested). 0 = not found.
        int LineOfKey(const std::string& text, const std::string& name, const std::string& category)
        {
            const std::string rel = name.size() > category.size() + 1 ? name.substr(category.size() + 1) : name;
            std::size_t pos = text.find("\"" + rel + "\"");
            if (pos == std::string::npos)
                if (const auto dot = rel.rfind('.'); dot != std::string::npos)
                    pos = text.find("\"" + rel.substr(dot + 1) + "\"");
            if (pos == std::string::npos) return 0;
            return 1 + static_cast<int>(std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(pos), '\n'));
        }

        // What a walk may touch. A non-empty `onlyModule` means only the cvars
        // that module declared, and no unknown-key report (settings spec s4.4).
        // `apply` false reads and reports only (ValidateCVarLayers): no Set.
        struct WalkOptions
        {
            std::string_view onlyModule;
            bool             apply = true;
        };

        void Walk(CVarRegistry& registry, const std::string& prefix, const nlohmann::json& node,
                  SetBy by, std::string_view sourceModule, const WalkOptions& opts, CVarApplyReport& report)
        {
            if (!node.is_object()) return;
            for (auto it = node.begin(); it != node.end(); ++it)
            {
                const std::string name = prefix.empty() ? it.key() : prefix + "." + it.key();
                if (it->is_object())
                {
                    Walk(registry, name, *it, by, sourceModule, opts, report);
                    continue;
                }
                // Resolve, not Find: a file names a cvar the way a PERSON wrote
                // it, so a renamed cvar's old key still lands (settings spec s4.7).
                const CVarHandle handle = registry.Resolve(name);
                if (handle.IsStale())
                {
                    // A Dev cvar a Dist build compiled out is ignored silently (spec s12).
                    if (opts.onlyModule.empty() && !registry.IsCompiledOut(name)) report.unknownKeys.push_back(name);
                    continue;
                }
                if (!opts.onlyModule.empty() && registry.ModuleOf(handle) != opts.onlyModule)
                    continue;
                const auto current = registry.Get(handle);
                if (!current) continue;
                std::vector<std::string> enumNames;
                if (current->type == CVarType::Enum)
                    if (const auto meta = registry.Metadata(handle)) enumNames = meta->enumNames;
                std::optional<std::int32_t> numericEnum;
                std::optional<CVarValue> value = ValueFromJson(*it, current->type, enumNames, numericEnum);
                if (!value)
                {
                    report.typeMismatches.push_back(name);   // the wrong shape for its type: refused, reported
                    continue;
                }
                if (numericEnum && opts.apply)   // the validating pass stays quiet: the applying one already said it
                    ARC_WARN("cvar: '{}' gives an Enum as the number {}; write \"{}\" -- the next archive write saves the name",
                             name, *numericEnum, enumNames[static_cast<std::size_t>(*numericEnum)]);
                if (opts.apply) registry.Set(handle, std::move(*value), by, sourceModule, CVarContext::Editor);
            }
        }

        CVarApplyReport ApplyCategory(CVarRegistry& registry, std::string_view category, const nlohmann::json& doc,
                                      SetBy by, bool documentShaped, std::string_view sourceModule, const WalkOptions& opts)
        {
            CVarApplyReport report;
            if (documentShaped || !doc.is_object()) return report;
            Walk(registry, std::string(category), doc, by, sourceModule, opts, report);
            return report;
        }
    }

    CVarApplyReport ApplyCVarCategory(CVarRegistry& registry, std::string_view category,
                                      const nlohmann::json& doc, SetBy by, bool documentShaped,
                                      std::string_view sourceModule)
    {
        return ApplyCategory(registry, category, doc, by, documentShaped, sourceModule, WalkOptions{});
    }

    CVarApplyReport ApplyCVarDirectory(CVarRegistry& registry, const std::filesystem::path& dir,
                                       SetBy by, std::string_view sourceModule, std::string_view onlyModule)
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
            const CVarApplyReport part = ApplyCategory(registry, stem, doc, by, IsDocumentCategory(stem), sourceModule,
                                                       WalkOptions{ onlyModule });
            report.unknownKeys.insert(report.unknownKeys.end(), part.unknownKeys.begin(), part.unknownKeys.end());
            report.typeMismatches.insert(report.typeMismatches.end(), part.typeMismatches.begin(), part.typeMismatches.end());
        }
        return report;
    }

    std::vector<CVarConfigIssue> ValidateCVarLayers(CVarRegistry& registry, const LayerSources& layers)
    {
        std::vector<CVarConfigIssue> issues;
        for (const CVarLayerDir& layer : layers.dirs)
        {
            std::error_code ec;
            if (!std::filesystem::is_directory(layer.dir, ec)) continue;
            std::vector<std::filesystem::path> files;
            for (const auto& entry : std::filesystem::directory_iterator(layer.dir, ec))
                if (entry.is_regular_file() && entry.path().extension() == ".json") files.push_back(entry.path());
            std::sort(files.begin(), files.end());
            for (const std::filesystem::path& file : files)
            {
                const std::string stem = file.stem().string();
                if (IsDocumentCategory(stem)) continue;
                const std::optional<std::string> text = ReadWholeFile(file);
                if (!text) continue;
                const auto doc = nlohmann::json::parse(*text, nullptr, false);
                if (doc.is_discarded() || !doc.is_object()) continue;   // the archive's .bad path owns broken files
                const CVarApplyReport report = ApplyCategory(registry, stem, doc, layer.by, false, layer.sourceModule,
                                                             WalkOptions{ {}, false });
                for (const std::string& key : report.unknownKeys)
                    issues.push_back(CVarConfigIssue{ CVarConfigIssue::Kind::UnknownKey, file, key, LineOfKey(*text, key, stem) });
                for (const std::string& key : report.typeMismatches)
                    issues.push_back(CVarConfigIssue{ CVarConfigIssue::Kind::TypeMismatch, file, key, LineOfKey(*text, key, stem) });
            }
        }
        return issues;
    }

    void PublishCVarConfigDiagnostics(const std::vector<CVarConfigIssue>& issues, CVarConfigLog log)
    {
        // The last LOGGED set, keyed (kind, file, key). Main thread only, like
        // every publish site (OpenProject, CloseProject, a module (re)load).
        // The rows always carry the whole set; the log is a DELTA against this,
        // so an issue a file keeps across hot reloads warns once, a key fixed
        // and then broken again warns again, and a shrinking set (a close, a
        // module whose keys just became known) logs nothing.
        using LoggedKey = std::tuple<CVarConfigIssue::Kind, std::string, std::string>;
        static std::set<LoggedKey> logged;

        std::vector<Diagnostic> rows;
        rows.reserve(issues.size());
        std::set<LoggedKey> loggedNow;
        for (const CVarConfigIssue& issue : issues)
        {
            const std::string fileName = issue.file.filename().string();
            const std::string path     = issue.file.generic_string();
            Diagnostic d;
            d.scope = DiagScope::Project;
            if (issue.kind == CVarConfigIssue::Kind::UnknownKey)
            {
                d.severity = DiagSeverity::Warning;
                d.code     = "config.cvar.unknown-key";
                d.message  = "Unknown setting '" + issue.key + "' in " + fileName + ".";
                d.detail   = "No loaded module declares it, so it was not applied. Check the spelling, or load the module that declares it.";
            }
            else
            {
                d.severity = DiagSeverity::Error;
                d.code     = "config.cvar.type-mismatch";
                d.message  = "Setting '" + issue.key + "' in " + fileName + " has the wrong type.";
                d.detail   = "The value was refused; the setting keeps the value of the rungs below it.";
            }
            d.locator = DiagLocator::File(path, issue.line);
            if (log == CVarConfigLog::Now)
            {
                LoggedKey id{ issue.kind, path, issue.key };
                if (!logged.contains(id))
                    ARC_WARN("cvar config: {} '{}' at {}:{}", d.code, issue.key, path, issue.line);
                loggedNow.insert(std::move(id));
            }
            rows.push_back(std::move(d));
        }
        // Deferred leaves the logged set alone: the publish that follows the
        // module load is then the first to log, and names only what survived.
        if (log == CVarConfigLog::Now)
            logged = std::move(loggedNow);
        Diagnostics::Publish("config.cvars", rows);
    }

    void CVarRegistry::ApplyLayersFor(std::string_view module, const LayerSources& layers)
    {
        if (module.empty()) return;
        for (const CVarLayerDir& layer : layers.dirs)
            (void)ApplyCVarDirectory(*this, layer.dir, layer.by, layer.sourceModule, module);
        for (const std::string& item : layers.commandLine)
        {
            const auto eq = item.find('=');
            if (eq == std::string::npos || eq == 0) continue;   // ApplyCVarCommandLine warned at boot
            const std::string name = item.substr(0, eq);
            // Resolve, as Execute does below: a `--set old.name=...` written against
            // a renamed cvar belongs to the module that declared the NEW name.
            const CVarHandle handle = Resolve(name);
            if (handle.IsStale() || ModuleOf(handle) != module) continue;
            const ExecResult result = Execute(name + " " + item.substr(eq + 1), layers.commandLineContext, SetBy::CommandLine);
            if (!result.ok) ARC_WARN("cvar: --set {}: {}", name, result.text);
        }
        Publish();
    }

    void WriteCVarArchive(const CVarRegistry& registry, const std::filesystem::path& userDir)
    {
        // category -> the (key, value) pairs it owns, plus the renamed keys
        // (aliases' old names) this write retires from the file. Ordered, so
        // the writes are too.
        struct CategoryWrite
        {
            std::vector<std::pair<std::string, nlohmann::json>> values;
            std::vector<std::string> retired;
        };
        std::map<std::string, CategoryWrite> owned;
        std::set<std::string> written;
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
            std::vector<std::string> enumNames;
            if (entry.type == CVarType::Enum)
                if (const auto meta = registry.Metadata(registry.Find(entry.name))) enumNames = meta->enumNames;
            nlohmann::json json = ArchiveJson(*value, enumNames);
            if (json.is_null()) continue;
            written.insert(entry.name);
            owned[std::move(category)].values.emplace_back(entry.name.substr(dot + 1), std::move(json));
        }
        // settings spec s4.7: a renamed cvar's old key goes wherever its new one is written.
        for (const auto& [oldName, newName] : registry.Aliases())
        {
            if (!written.contains(newName)) continue;
            const auto dot = oldName.find('.');
            if (dot == std::string::npos) continue;          // a dot-less old name was never archived
            std::string category = oldName.substr(0, dot);
            if (IsDocumentCategory(category)) continue;
            owned[std::move(category)].retired.push_back(oldName.substr(dot + 1));
        }
        if (owned.empty()) return;
        std::error_code ec;
        std::filesystem::create_directories(userDir, ec);
        for (auto& [category, write] : owned)
        {
            const std::filesystem::path file = userDir / (category + ".json");
            const std::optional<std::string> before = ReadWholeFile(file);
            if (write.values.empty() && !before) continue;   // only retirements, and no file to retire them from
            nlohmann::json doc = nlohmann::json::object();
            if (before)
            {
                auto parsed = nlohmann::json::parse(*before, nullptr, false);
                if (!parsed.is_discarded() && parsed.is_object())
                    doc = std::move(parsed);
                else
                {
                    if (write.values.empty()) continue;      // never back up or replace a file only to retire a key
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
            for (const std::string& key : write.retired)
                EraseLeaf(doc, key);
            for (auto& [key, value] : write.values)
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
            bool written2 = false;   // `written` is the set of archived names above
            std::error_code renamed;
            {
                std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
                out << text;
                out.flush();
                written2 = static_cast<bool>(out);
            }
            if (written2)
                std::filesystem::rename(tmp, file, renamed);   // replaces: the old file or the new, never half of one
            if (!written2 || renamed)
            {
                ARC_WARN("cvar: cannot write '{}'{}{}", file.generic_string(), renamed ? ": " : "",
                         renamed ? renamed.message() : std::string());
                std::error_code ignored;
                std::filesystem::remove(tmp, ignored);
            }
        }
    }

    void ApplyCVarCommandLine(CVarRegistry& registry, const std::vector<std::string>& sets, CVarContext ctx)
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
            const ExecResult result = registry.Execute(name + " " + value, ctx, SetBy::CommandLine);
            if (!result.ok) ARC_WARN("cvar: --set {}: {}", name, result.text);
        }
    }
}
