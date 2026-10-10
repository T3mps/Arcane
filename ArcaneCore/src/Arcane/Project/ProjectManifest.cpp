#include <Arcane/Project/ProjectManifest.hpp>

#include <Arcane/Base/Diagnostics.hpp>
#include <Arcane/Base/Log.hpp>   // ARC_WARN / ARC_INFO

#include <Json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace Arcane
{
    std::optional<ProjectManifest> ProjectManifest::FromJson(const nlohmann::json& doc)
    try
    {
        if (!doc.is_object())
            return std::nullopt;

        ProjectManifest m;

        // Required: formatVersion (> 0), name (non-empty), engine.abi (int).
        if (!doc.contains("formatVersion") || !doc["formatVersion"].is_number_integer())
            return std::nullopt;
        m.formatVersion = doc["formatVersion"].get<int>();
        if (m.formatVersion <= 0)
            return std::nullopt;

        if (!doc.contains("name") || !doc["name"].is_string())
            return std::nullopt;
        m.name = doc["name"].get<std::string>();
        if (m.name.empty())
            return std::nullopt;

        if (!doc.contains("engine") || !doc["engine"].is_object()
            || !doc["engine"].contains("abi") || !doc["engine"]["abi"].is_number_integer())
            return std::nullopt;
        m.engineAbi = doc["engine"]["abi"].get<int>();

        // Optional. doc.value(key, default) throws nlohmann::json::type_error if the key
        // exists with the wrong type -- caught by the function-try-block below so a
        // type-mismatched optional field yields nullopt rather than propagating an
        // exception (same contract as the required-field guards above).
        m.description = doc.value("description", std::string{});
        m.company     = doc.value("company", std::string{});
        m.gameModule  = doc.value("gameModule", std::string{});
        m.bootScene   = doc.value("bootScene", std::string{});
        m.inputActions = doc.value("inputActions", std::string{});
        m.guid        = doc.value("guid", std::string{});

        // sourceDir: optional, default "Source". Strict and loud (plan ruling
        // S3): it must name Source/ itself or a directory under it, with no
        // ".." segment, no backslash and no leading slash -- a module
        // directory outside the source:// mount could never register a
        // created class, and a silently-accepted bad value is the failure
        // class this engine keeps meeting. build/arcane.lua applies the
        // identical rule on the build side.
        if (doc.contains("sourceDir"))
        {
            std::string dir = doc.value("sourceDir", std::string{});   // type_error -> nullopt via the try/catch
            while (!dir.empty() && dir.back() == '/')
                dir.pop_back();
            const bool underSource = dir == "Source" || dir.rfind("Source/", 0) == 0;
            const bool escapes = dir.find("..") != std::string::npos
                              || dir.find('\\') != std::string::npos
                              || dir.find("//") != std::string::npos;
            if (!underSource || escapes)
                return std::nullopt;
            m.sourceDir = std::move(dir);
        }

        if (doc.contains("plugins") && doc["plugins"].is_array())
        {
            for (const auto& p : doc["plugins"])
            {
                if (!p.is_object() || !p.contains("name") || !p["name"].is_string())
                    continue;
                PluginRef ref;
                ref.name    = p["name"].get<std::string>();
                ref.enabled = p.value("enabled", true);
                m.plugins.push_back(std::move(ref));
            }
        }

        // Legacy splash block -> app.splash.* for the one-time migration. A
        // non-object block is leniently ignored, as before; inside an object a
        // wrong-typed scalar still throws via .value() (caught below), so the
        // whole manifest still fails as it always did. Only the fields the old
        // parser read are carried, each in its cvar file shape, so migration
        // never writes an unknown key or a type mismatch into Config/app.json.
        if (doc.contains("splash") && doc["splash"].is_object())
        {
            const auto& sp = doc["splash"];
            nlohmann::json splash = nlohmann::json::object();
            if (sp.contains("enabled"))            splash["enabled"]            = sp.value("enabled", true);
            if (sp.contains("image"))              splash["image"]              = sp.value("image", std::string{});
            if (sp.contains("showProgress"))       splash["showProgress"]       = sp.value("showProgress", false);
            if (sp.contains("minDurationSeconds")) splash["minDurationSeconds"] = sp.value("minDurationSeconds", 0.0f);
            // backgroundColor was [r, g, b] in sRGB-normalised floats ((0.05,
            // 0.05, 0.06) is 0x0D0D0F, the splash's one colour); the cvar file
            // shape is "#RRGGBBAA" (sRGB hex), so the bytes carry over exactly.
            // Malformed (wrong type, too short, a non-number) stays lenient: the
            // default applies and nothing migrates, as the old parse did.
            if (sp.contains("backgroundColor") && sp["backgroundColor"].is_array()
                && sp["backgroundColor"].size() >= 3
                && sp["backgroundColor"][0].is_number() && sp["backgroundColor"][1].is_number()
                && sp["backgroundColor"][2].is_number())
            {
                const auto byte = [&](std::size_t i)
                {
                    const float v = std::clamp(sp["backgroundColor"][i].get<float>(), 0.0f, 1.0f);
                    return static_cast<unsigned>(std::lround(v * 255.0f));
                };
                char hex[10];
                std::snprintf(hex, sizeof hex, "#%02X%02X%02XFF", byte(0), byte(1), byte(2));
                splash["backgroundColor"] = std::string(hex);
            }
            if (!splash.empty())
                m.legacySettings["app"]["splash"] = std::move(splash);
        }

        // Only a valid legacy gravity becomes a project setting. Malformed
        // shapes retain the old lenient behavior and contribute no migration.
        if (doc.contains("physics") && doc["physics"].is_object())
        {
            const auto& ph = doc["physics"];
            if (ph.contains("gravity") && ph["gravity"].is_array() && ph["gravity"].size() >= 2
                && ph["gravity"][0].is_number() && ph["gravity"][1].is_number())
            {
                float x = ph["gravity"][0].get<float>();
                float y = ph["gravity"][1].get<float>();
                // v1 -> v2 (F4 plan 1 final review, F2a): a formatVersion 1
                // manifest authored its gravity +Y DOWN (the Hub stamped
                // [0, 9.81] from 2026-09-11); the engine is +Y up since F4, so
                // the y component is negated on read. A v1 manifest WITHOUT
                // the block never enters here and takes the v2 default above.
                if (m.formatVersion < 2)
                {
                    y = -y;
                    ARC_INFO("ProjectManifest: formatVersion 1 physics.gravity was authored +Y down; "
                             "read as ({}, {}) (+Y up, format 2)", x, y);
                }
                m.legacySettings["physics"]["gravity"] = { x, y };
            }
        }

        return m;
    }
    catch (const nlohmann::json::exception&)
    {
        return std::nullopt;
    }

    namespace
    {
        // KEY OWNERSHIP: "project" (fixed key). LoadFile is the SINGLE owner
        // of every manifest-shaped-file diagnostic's CONTENT (code/message/
        // File-locator), for EVERY caller -- both the project's own .arcproj
        // (Project::Open's top-level call, made BEFORE a Project instance even
        // exists) and a plugin's .arcplugin descriptor (Project::Open's
        // per-plugin validation call, made from INSIDE an already-constructed
        // Project). LoadFile is the only code that knows which of open/parse/
        // schema actually failed, so it is always the one that DECIDES the
        // diagnostic here.
        //
        // WHO PUBLISHES differs by caller, via `outDiag`:
        //   - outDiag == nullptr (the default): this function publishes
        //     directly. Safe for the top-level manifest load, because on that
        //     failure Project::Open returns std::nullopt immediately afterward
        //     (Project.cpp) without ever touching "project" again -- this
        //     publish is the final, authoritative word for that open attempt.
        //   - outDiag != nullptr: this function FILLS *outDiag and does NOT
        //     publish. Project::Open's plugin-descriptor validation loop uses
        //     this, because ITS OWN unconditional "project" publish at the end
        //     of Open() would otherwise silently retract a direct publish made
        //     here mid-loop (Publish() replaces the whole key, and Open() has
        //     no way to read back what LoadFile just published to fold it in).
        //     Open() instead appends the filled diagnostic verbatim to its own
        //     accumulator -- same code/message/locator LoadFile decided, no
        //     re-deriving or re-wording it caller-side (which is what let an
        //     unreadable descriptor mis-surface as "invalid" before this).
        void ReportManifestFailure(const std::filesystem::path& file, DiagSeverity severity,
                                    const char* code, std::string message, Diagnostic* outDiag)
        {
            Diagnostic d;
            d.severity = severity;
            d.scope    = DiagScope::Project;
            d.code     = code;
            d.message  = std::move(message);
            d.locator  = DiagLocator::File(file.generic_string());
            if (outDiag)
                *outDiag = std::move(d);
            else
                Diagnostics::Publish("project", std::vector<Diagnostic>{ std::move(d) });
        }
    }

    std::optional<ProjectManifest> ProjectManifest::LoadFile(const std::filesystem::path& file,
                                                              Diagnostic* outDiag)
    {
        std::ifstream in(file, std::ios::binary);
        if (!in)
        {
            ARC_WARN("ProjectManifest: cannot open '{}'", file.generic_string());
            ReportManifestFailure(file, DiagSeverity::Error, "project.manifest.unreadable",
                                   "Could not open '" + file.generic_string() + "'.", outDiag);
            return std::nullopt;
        }
        std::stringstream ss;
        ss << in.rdbuf();

        nlohmann::json doc;
        try
        {
            doc = nlohmann::json::parse(ss.str());
        }
        catch (const std::exception& e)
        {
            ARC_WARN("ProjectManifest: parse failed for '{}': {}", file.generic_string(), e.what());
            ReportManifestFailure(file, DiagSeverity::Error, "project.manifest.unreadable",
                                   "Could not parse '" + file.generic_string() + "': " + e.what(), outDiag);
            return std::nullopt;
        }
        auto m = FromJson(doc);
        if (!m)
        {
            ARC_WARN("ProjectManifest: schema invalid in '{}'", file.generic_string());
            ReportManifestFailure(file, DiagSeverity::Error, "project.manifest.invalid",
                                   "'" + file.generic_string() + "' is not a valid project manifest.", outDiag);
        }
        return m;
    }
}
