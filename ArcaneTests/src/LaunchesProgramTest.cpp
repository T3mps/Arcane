// Settings S7-SEC: project config never chooses a program to run.
//
// A setting whose value names a program Arcane executes carries
// CVarFlags::LaunchesProgram. Its value is honoured only from the machine:
// --set (CommandLine), the machine-wide preferences rung (EditorUser) and the
// default. Every rung that lives in a project -- Config/ (Project), a plugin's
// Config/ (Plugin), Saved/Config (User), the legacy .arcproj settings block,
// and any folder inside the project or a plugin root -- keeps the default and
// warns once per key, naming the file, the key and the reason. A
// PreferencesMachine setting is refused from Project and Plugin config too.
// [cvar][launch]

#include <catch2/catch_test_macros.hpp>

#include <Arcane/AssetPipeline/TextureMetaSettings.hpp>
#include <Arcane/Base/Diagnostics.hpp>
#include <Arcane/Base/Engine.hpp>   // ExecutablePathUtf8: the default report dir
#include <Arcane/Base/Log.hpp>
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Build/BuildToolSettings.hpp>
#include <Arcane/Build/Toolchain.hpp>
#include <Arcane/Config/CVarConfig.hpp>
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Config/PreferenceScope.hpp>
#include <Arcane/Platform/LaunchPath.hpp>
#include <Arcane/Project/Project.hpp>

#include <Project/ModuleBuild.hpp>   // source-compiled into ArcaneTests
#include <Settings/SettingsEdit.hpp> // source-compiled into ArcaneTests

#include "Helpers/CVarTestDesc.hpp"
#include "Helpers/ConstantScan.hpp"
#include "Helpers/SettingsSweep.hpp"
#include "Helpers/TestTypeContext.hpp"
#include "Helpers/UserDataDirs.hpp"

#include <Json.hpp>

#include <spdlog/sinks/callback_sink.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <regex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>   // CommandLineToArgvW: the crash monitor's line parses back exactly
#pragma comment(lib, "Shell32.lib")
#endif

using namespace Arcane;

namespace
{
    namespace fs = std::filesystem;

    // The launch-site audit (S7-SEC report): every path setting, and whether a
    // launch site runs its value. A path setting not listed here fails the
    // sweep below until it is classified.
    struct PathSettingClass { std::string_view name; bool launches; std::string_view consumer; };
    constexpr PathSettingClass kPathSettings[] = {
        { "build.premakePath",        true,  "arcbuild ProcessRunner (CreateProcessW) via Toolchain::ResolvePremake" },
        { "build.msbuildPath",        true,  "arcbuild ProcessRunner (CreateProcessW) via Toolchain::ResolveMsBuild" },
        { "build.makePath",           true,  "arcbuild ProcessRunner (CreateProcessW) via Toolchain::ResolveMake" },
        { "build.ninjaPath",          true,  "arcbuild ProcessRunner (CreateProcessW) via Toolchain::ResolveNinja" },
        { "build.ideExecutable",      true,  "IdeLaunch::Launch (CreateProcessW) via Toolchain::ResolveDevenv" },
        { "diagnostics.reporterPath", true,  "Diagnostics SpawnReporter / LaunchMonitor (CreateProcessW)" },
        { "net.protocolPath",         false, "ProtocolLoader reads it as JSON data" },
        { "diagnostics.dumpDir",      false, "crash and hang reports are written there" },
        { "log.dir",                  false, "the engine log file is written there" },
    };
    constexpr std::string_view kProgramReason = "names a program; set it in Preferences (machine) or with --set";

    const PathSettingClass* Classified(std::string_view name)
    {
        for (const PathSettingClass& c : kPathSettings)
            if (c.name == name) return &c;
        return nullptr;
    }

    std::vector<std::string> LaunchingNames()
    {
        std::vector<std::string> out;
        for (const PathSettingClass& c : kPathSettings)
            if (c.launches) out.emplace_back(c.name);
        return out;
    }

    fs::path Scratch(const char* tag)
    {
        const fs::path d = fs::temp_directory_path() / (std::string("arcane_s7sec_") + tag);
        std::error_code ec;
        fs::remove_all(d, ec);
        fs::create_directories(d, ec);
        return d;
    }

    void Write(const fs::path& p, const std::string& text)
    {
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        std::ofstream(p, std::ios::binary) << text;
    }

    // build.json + diagnostics.json in `dir`, every LaunchesProgram key = `exe`.
    void WriteProgramKeys(const fs::path& dir, const fs::path& exe)
    {
        const std::string e = exe.generic_string();
        nlohmann::json build = nlohmann::json::object();
        for (const char* key : { "premakePath", "msbuildPath", "makePath", "ninjaPath", "ideExecutable" })
            build[key] = e;
        Write(dir / "build.json", build.dump(2));
        Write(dir / "diagnostics.json", nlohmann::json{ { "reporterPath", e } }.dump(2));
    }

    // A real file, so a refusal is never "the path does not exist".
    fs::path MakeExe(const fs::path& dir, const char* name)
    {
        const fs::path exe = dir / name;
        Write(exe, "MZ");
        return exe;
    }

    std::size_t CountOf(const std::string& text, std::string_view needle)
    {
        std::size_t n = 0;
        for (std::size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size()))
            ++n;
        return n;
    }

    // The engine log, every line, for the case.
    struct LogCapture
    {
        std::string text;
        std::shared_ptr<spdlog::sinks::callback_sink_mt> sink;
        LogCapture()
        {
            sink = std::make_shared<spdlog::sinks::callback_sink_mt>([this](const spdlog::details::log_msg& m) {
                text.append(m.payload.data(), m.payload.size());
                text.push_back('\n');
            });
            Log::Engine()->sinks().push_back(sink);
        }
        ~LogCapture()
        {
            auto& sinks = Log::Engine()->sinks();
            sinks.erase(std::remove(sinks.begin(), sinks.end(), sink), sinks.end());
        }
        LogCapture(const LogCapture&) = delete;
        LogCapture& operator=(const LogCapture&) = delete;

        // The one log line that reports `key` as refused.
        [[nodiscard]] std::string LineFor(std::string_view key) const
        {
            const std::string needle = "config.cvar.refused '" + std::string(key) + "'";
            const std::size_t at = text.find(needle);
            if (at == std::string::npos) return {};
            const std::size_t begin = text.rfind('\n', at);
            const std::size_t end = text.find('\n', at);
            return text.substr(begin == std::string::npos ? 0 : begin + 1, end - (begin == std::string::npos ? 0 : begin + 1));
        }
    };

    // The Problems rows the case published under "config.cvars".
    struct DiagCapture
    {
        std::vector<Diagnostic> last;
        bool seen = false;
        DiagCapture() { Diagnostics::SetSink(&Sink, this); }
        ~DiagCapture() { Diagnostics::SetSink(nullptr, nullptr); }
        DiagCapture(const DiagCapture&) = delete;
        DiagCapture& operator=(const DiagCapture&) = delete;
        static void Sink(std::string_view key, std::span<const Diagnostic> diags, void* user)
        {
            if (key != "config.cvars") return;
            auto* self = static_cast<DiagCapture*>(user);
            self->last.assign(diags.begin(), diags.end());
            self->seen = true;
        }
        [[nodiscard]] const Diagnostic* Refused(std::string_view key) const
        {
            for (const Diagnostic& d : last)
                if (d.code == "config.cvar.refused" && d.message.find("'" + std::string(key) + "'") != std::string::npos)
                    return &d;
            return nullptr;
        }
    };

    // The global rungs a case may touch, dropped however it exits.
    struct RungCleanup
    {
        ~RungCleanup()
        {
            CVarRegistry& r = CVarRegistry::Get();
            r.RevertLayer(SetBy::EditorUser);
            r.RevertLayer(SetBy::CommandLine);
            r.RevertLayer(SetBy::Project);
            r.RevertLayer(SetBy::User);
            r.PublishImmediate();
        }
    };

    bool HoldsRung(std::string_view name, SetBy rung)
    {
        return CVarRegistry::Get().RungValue(name, rung).has_value();
    }

    // Each launching name present in THIS build (Dist proves the Dev ones are
    // compiled out, and a compiled-out key in a file is ignored silently: spec s12).
    std::vector<std::string> LaunchingNamesInThisBuild()
    {
        std::vector<std::string> out;
        for (const std::string& name : LaunchingNames())
            if (Test::InThisBuild(name)) out.push_back(name);
        return out;
    }

    // Opens `root` in a fresh Runtime, checks every LaunchesProgram key kept its
    // default, has no record on `rung`, and was reported ONCE with the file and
    // the reason. `fileDir` is where the refused files live.
    void CheckRefusedAtOpen(const fs::path& root, const fs::path& fileDir, SetBy rung,
                            const fs::path& editorUserDir = {})
    {
        DiagCapture cap;
        LogCapture log;
        CVarRegistry& reg = CVarRegistry::Get();
        Runtime rt(Test::Process());
        if (!editorUserDir.empty()) rt.SetEditorUserConfigDir(editorUserDir);
        REQUIRE(rt.OpenProject(root));
        for (const std::string& name : LaunchingNamesInThisBuild())
        {
            INFO(name);
            CHECK(reg.Get(reg.Find(name))->AsString().empty());
            CHECK_FALSE(HoldsRung(name, rung));
            const Diagnostic* row = cap.Refused(name);
            REQUIRE(row);
            CHECK(row->severity == DiagSeverity::Warning);
            CHECK(row->message.find(std::string(kProgramReason)) != std::string::npos);
            CHECK(row->locator.kind == DiagLocator::Kind::File);
            CHECK(fs::path(row->locator.file).parent_path().lexically_normal() == fileDir.lexically_normal());
            CHECK(CountOf(log.text, "config.cvar.refused '" + name + "'") == 1);
            const std::string line = log.LineFor(name);
            CHECK(line.find(std::string(kProgramReason)) != std::string::npos);
            const std::string file = name.starts_with("build.") ? "build.json" : "diagnostics.json";
            CHECK(line.find(file) != std::string::npos);
        }
        if constexpr (kDistBuild)
            CHECK(log.text.find("config.cvar.refused 'build.premakePath'") == std::string::npos);   // compiled out: silent
        rt.CloseProject();
    }
}

TEST_CASE("S7-SEC: a project's Config/ cannot name a program -- every LaunchesProgram key keeps its default and warns once",
          "[cvar][launch][project]")
{
    const RungCleanup cleanup;
    const fs::path dir = Scratch("project");
    REQUIRE(Project::Create(dir / "P", "LaunchProbe").has_value());
    WriteProgramKeys(dir / "P" / "Config", MakeExe(dir, "evil.exe"));
    CheckRefusedAtOpen(dir / "P", dir / "P" / "Config", SetBy::Project);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("S7-SEC: the User rung (Saved/Config) cannot name a program", "[cvar][launch][project]")
{
    const RungCleanup cleanup;
    const fs::path dir = Scratch("user");
    REQUIRE(Project::Create(dir / "P", "LaunchProbeUser").has_value());
    const fs::path user = Test::FreshUserConfigDir(dir / "P");
    WriteProgramKeys(user, MakeExe(dir, "evil.exe"));
    CheckRefusedAtOpen(dir / "P", user, SetBy::User);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::remove_all(user, ec);
}

TEST_CASE("S7-SEC: an EditorUser folder inside the project cannot name a program", "[cvar][launch][project]")
{
    const RungCleanup cleanup;
    const fs::path dir = Scratch("inside");
    REQUIRE(Project::Create(dir / "P", "LaunchProbeInside").has_value());
    const fs::path inside = dir / "P" / "Prefs" / "Config";
    WriteProgramKeys(inside, MakeExe(dir, "evil.exe"));
    CheckRefusedAtOpen(dir / "P", inside, SetBy::EditorUser, inside);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("S7-SEC: the legacy .arcproj settings block cannot name a program", "[cvar][launch][project]")
{
    const RungCleanup cleanup;
    const fs::path dir = Scratch("legacy");
    const fs::path exe = MakeExe(dir, "evil.exe");
    ProjectManifest manifest;
    for (const char* key : { "premakePath", "msbuildPath", "makePath", "ninjaPath", "ideExecutable" })
        manifest.legacySettings["build"][key] = exe.generic_string();
    manifest.legacySettings["diagnostics"]["reporterPath"] = exe.generic_string();

    // The warning names the .arcproj the block came from (fix round 1).
    const fs::path manifestFile = dir / "P" / "LegacyProbe.arcproj";
    LogCapture log;
    CVarRegistry& reg = CVarRegistry::Get();
    ApplyLegacyManifestSettings(reg, manifest, manifestFile);
    reg.PublishImmediate();
    for (const std::string& name : LaunchingNamesInThisBuild())
    {
        INFO(name);
        CHECK(reg.Get(reg.Find(name))->AsString().empty());
        CHECK_FALSE(HoldsRung(name, SetBy::Project));
        CHECK(CountOf(log.text, "config.cvar.refused '" + name + "'") == 1);
        const std::string line = log.LineFor(name);
        INFO(line);
        CHECK(line.find(manifestFile.generic_string()) != std::string::npos);
        CHECK(line.find(std::string(kProgramReason)) != std::string::npos);
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("S7-SEC: --set and the machine-wide EditorUser rung DO choose the program", "[cvar][launch][project]")
{
    const RungCleanup cleanup;
    const fs::path dir = Scratch("machine");
    REQUIRE(Project::Create(dir / "P", "LaunchProbeMachine").has_value());
    const fs::path tool = MakeExe(dir, "tool.exe");
    const fs::path machine = dir / "machine" / "Config";   // outside the project
    WriteProgramKeys(machine, tool);

    DiagCapture cap;
    CVarRegistry& reg = CVarRegistry::Get();
    {
        Runtime rt(Test::Process());
        rt.SetEditorUserConfigDir(machine);
        REQUIRE(rt.OpenProject(dir / "P"));
        for (const std::string& name : LaunchingNamesInThisBuild())
        {
            INFO(name);
            CHECK(reg.Get(reg.Find(name))->AsString() == tool.generic_string());
            CHECK(reg.Explain(name)->setBy == SetBy::EditorUser);
            CHECK(cap.Refused(name) == nullptr);
        }
        rt.CloseProject();
        rt.SetEditorUserConfigDir({});
    }

    const fs::path other = MakeExe(dir, "other.exe");
    std::vector<std::string> sets;
    for (const std::string& name : LaunchingNames())
        sets.push_back(name + "=" + other.generic_string());
    ApplyCVarCommandLine(reg, sets, CVarContext::Editor);
    reg.PublishImmediate();
    for (const std::string& name : LaunchingNamesInThisBuild())
    {
        INFO(name);
        CHECK(reg.Get(reg.Find(name))->AsString() == other.generic_string());
        CHECK(reg.Explain(name)->setBy == SetBy::CommandLine);
    }
    if (Test::InThisBuild("build.msbuildPath"))
        CHECK(Toolchain::ResolveMsBuild() == fs::absolute(other).lexically_normal());
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("S7-SEC: a PreferencesMachine setting is refused from Project and Plugin config; PreferencesProject stays a project suggestion",
          "[cvar][launch]")
{
    const fs::path dir = Scratch("scope");
    Write(dir / "Config" / "s7sec.json", "{\n  \"machine\": 5,\n  \"perProject\": 6,\n  \"shared\": 7\n}\n");
    CVarRegistry reg;
    const auto declare = [&](const char* name, SettingScope scope) {
        CVarDesc d = Test::Desc(name, CVarValue::Int32(1), Audience::Editor, CVarFlags::Archive);
        d.scope = scope;
        return reg.Register(d);
    };
    const CVarHandle machine = declare("s7sec.machine", SettingScope::PreferencesMachine);
    const CVarHandle perProject = declare("s7sec.perProject", SettingScope::PreferencesProject);
    const CVarHandle shared = declare("s7sec.shared", SettingScope::Project);
    REQUIRE_FALSE(machine.IsStale());

    for (const SetBy rung : { SetBy::Project, SetBy::Plugin })
    {
        INFO(static_cast<int>(rung));
        const CVarApplyReport report = ApplyCVarDirectory(reg, dir / "Config", rung, "probe");
        REQUIRE(report.refused.size() == 1);
        CHECK(report.refused[0].key == "s7sec.machine");
        CHECK(report.refused[0].why == CVarRungRefusal::MachinePreference);
        reg.Publish();
        CHECK(reg.Get(machine)->AsInt32() == 1);
        CHECK(reg.Get(perProject)->AsInt32() == 6);   // "project may suggest" (spec s3.3: scope is where the DEFAULT is edited)
        CHECK(reg.Get(shared)->AsInt32() == 7);
        reg.RevertLayer(rung);
        reg.Publish();
    }
    // The per-project override ("This project", the User rung) is the user's.
    CHECK(ApplyCVarDirectory(reg, dir / "Config", SetBy::User, "user").refused.empty());
    reg.Publish();
    CHECK(reg.Get(machine)->AsInt32() == 5);

    LayerSources layers;
    layers.dirs.push_back(CVarLayerDir{ SetBy::Project, dir / "Config", "project" });
    const std::vector<CVarConfigIssue> issues = ValidateCVarLayers(reg, layers);
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].kind == CVarConfigIssue::Kind::Refused);
    CHECK(issues[0].key == "s7sec.machine");
    CHECK(issues[0].line == 2);
    CHECK(issues[0].refusal == CVarRungRefusal::MachinePreference);

    DiagCapture cap;
    PublishCVarConfigDiagnostics(issues);
    REQUIRE(cap.last.size() == 1);
    CHECK(cap.last[0].code == "config.cvar.refused");
    CHECK(cap.last[0].severity == DiagSeverity::Warning);
    CHECK(cap.last[0].message.find("s7sec.machine") != std::string::npos);
    CHECK(cap.last[0].message.find(std::string(RungRefusalReason(CVarRungRefusal::MachinePreference))) != std::string::npos);
    PublishCVarConfigDiagnostics({});
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("S7-SEC: the registry refuses a LaunchesProgram value from every config rung in a project, and from a remote console",
          "[cvar][launch]")
{
    CVarRegistry reg;
    CVarDesc d = Test::Desc("s7sec.tool", CVarValue::String(""), Audience::Server, CVarFlags::LaunchesProgram);
    d.scope = SettingScope::PreferencesMachine;
    const CVarHandle tool = reg.Register(d);
    REQUIRE_FALSE(tool.IsStale());
    const CVarValue exe = CVarValue::String("C:/tools/tool.exe");

    for (const SetBy rung : { SetBy::EngineConfig, SetBy::Plugin, SetBy::Project, SetBy::User })
    {
        INFO(static_cast<int>(rung));
        CHECK(RungRefusal(CVarFlags::LaunchesProgram, SettingScope::PreferencesMachine, rung) == CVarRungRefusal::LaunchesProgram);
        CHECK(reg.Set(tool, exe, rung) == SetResult::Denied);
        CHECK_FALSE(reg.SetRung("s7sec.tool", rung, exe, "probe"));
    }
    CHECK(reg.Set(tool, exe, SetBy::EditorUser) == SetResult::Applied);
    CHECK(reg.Set(tool, exe, SetBy::CommandLine) == SetResult::Applied);
    CHECK(reg.Set(tool, exe, SetBy::Code) == SetResult::Applied);
    CHECK(reg.Set(tool, exe, SetBy::Console, {}, CVarContext::Editor) == SetResult::Applied);
    CHECK(reg.Set(tool, exe, SetBy::Console, {}, CVarContext::ServerAdmin) == SetResult::Denied);   // an audited remote admin
    CHECK(reg.Set(tool, exe, SetBy::Console, {}, CVarContext::LocalHost) == SetResult::Denied);
    CHECK(reg.SetRung("s7sec.tool", SetBy::EditorUser, exe, "editor-user"));
    // A machine preference without the flag keeps the per-project override.
    CHECK(RungRefusal(CVarFlags::None, SettingScope::PreferencesMachine, SetBy::User) == CVarRungRefusal::None);
    CHECK(RungRefusal(CVarFlags::None, SettingScope::PreferencesProject, SetBy::Project) == CVarRungRefusal::None);
}

TEST_CASE("S7-SEC: Preferences writes a LaunchesProgram setting machine-wide only; This project is not offered", "[cvar][launch][settings-ui]")
{
    CVarRegistry reg;
    // Per-project home scope, like diagnostics.reporterPath: the flag still wins.
    CVarDesc d = Test::Desc("s7sec.reporter", CVarValue::String(""), Audience::Game, CVarFlags::LaunchesProgram | CVarFlags::Archive);
    d.scope = SettingScope::PreferencesProject;
    d.widget = "path:file";
    REQUIRE_FALSE(reg.Register(d).IsStale());

    CHECK(PreferenceTargetOf(reg, "s7sec.reporter") == PreferenceTarget::AllProjects);
    CHECK(SetPreferenceTarget(reg, "s7sec.reporter", PreferenceTarget::ThisProject) == SetResult::Denied);
    REQUIRE(EditPreference(reg, "s7sec.reporter", CVarValue::String("C:/r.exe")) == SetResult::Applied);
    CHECK(reg.RungValue("s7sec.reporter", SetBy::EditorUser).has_value());
    CHECK_FALSE(reg.RungValue("s7sec.reporter", SetBy::User).has_value());

    const std::optional<CVarDescInfo> desc = reg.Describe("s7sec.reporter");
    REQUIRE(desc);
    const Editor::RowFacts facts = Editor::ComputeRowFacts(reg, *desc, Editor::SettingsWindowKind::Preferences);
    CHECK(facts.mode == Editor::PrefMode::AllProjects);
    CHECK(facts.target == SetBy::EditorUser);
    CHECK(facts.machineOnly);
    CHECK(Editor::SwitchPrefMode(reg, *desc, Editor::PrefMode::ThisProject, {}) == nullptr);
}

TEST_CASE("S7-SEC sweep: every path setting is classified, and each one a launch site runs carries LaunchesProgram",
          "[sweep][launch]")
{
    CVarRegistry& reg = CVarRegistry::Get();
    for (const CVarListEntry& entry : reg.List())
    {
        if (entry.name.starts_with("tests.")) continue;   // the suite's own fixtures (SweepCoverageTest's rule)
        // No assertion per listed cvar: what else is registered depends on the
        // cases that ran before, and the baseline counts must not.
        const std::optional<CVarMetadata> meta = reg.Metadata(reg.Find(entry.name));
        if (!meta) FAIL("no metadata for the listed cvar " << entry.name);
        const bool launches = HasFlag(meta->flags, CVarFlags::LaunchesProgram);
        if (!meta->widget.starts_with("path:") && !launches) continue;
        INFO(entry.name << " (widget '" << meta->widget << "')");
        const PathSettingClass* c = Classified(entry.name);
        REQUIRE(c);   // a new path setting: audit its consumer and add it to kPathSettings
        CHECK(launches == c->launches);
        if (launches)
        {
            CHECK(meta->type == CVarType::String);
            CHECK(meta->scope != SettingScope::Project);   // its home rung must be one that may set it
        }
    }
    for (const PathSettingClass& c : kPathSettings)
    {
        INFO(c.name << ": " << c.consumer);
        if (!Test::InThisBuild(c.name)) continue;   // Dist: a Dev name proves its compile-out
        const std::optional<CVarMetadata> meta = reg.Metadata(reg.Find(c.name));
        REQUIRE(meta);
        CHECK(meta->widget.starts_with("path:"));
        CHECK(HasFlag(meta->flags, CVarFlags::LaunchesProgram) == c.launches);
    }
}

TEST_CASE("S7-SEC sweep: no path setting is declared outside the audit (source scan)", "[sweep][launch]")
{
    // The registry sweep above sees only what this process registers; the
    // editor and its plugins declare more. A path widget anywhere in the
    // engine's sources must be one of kPathSettings.
    const std::regex widget(R"re((Widget\s*,\s*|\.widget\s*=\s*)"path:(file|dir)")re");
    std::size_t found = 0;
    std::string where;
    const fs::path repo = Test::RepoRoot();
    for (const std::string& root : Test::ConstantScanRoots())
    {
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(repo / root, ec); it != fs::recursive_directory_iterator(); it.increment(ec))
        {
            const fs::path& p = it->path();
            if (p.extension() != ".hpp" && p.extension() != ".cpp" && p.extension() != ".h") continue;
            std::ifstream in(p, std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            const auto n = static_cast<std::size_t>(std::distance(std::sregex_iterator(text.begin(), text.end(), widget),
                                                                   std::sregex_iterator()));
            if (n == 0) continue;
            found += n;
            where += p.lexically_relative(repo).generic_string() + " x" + std::to_string(n) + "\n";
        }
    }
    INFO(where);
    CHECK(found == std::size(kPathSettings));   // a new one: classify it in kPathSettings (and flag it if launched)
}

TEST_CASE("S7-SEC: CheckLaunchPath accepts a regular file and refuses empty, missing, a directory and command-line breakers",
          "[launch]")
{
    const fs::path dir = Scratch("check");
    const fs::path exe = MakeExe(dir, "tool.exe");
    CHECK(CheckLaunchPath(exe) == LaunchPathStatus::Ok);
    CHECK(CheckLaunchPath(fs::path()) == LaunchPathStatus::Empty);
    CHECK(CheckLaunchPath(dir / "missing.exe") == LaunchPathStatus::NotFound);
    CHECK(CheckLaunchPath(dir) == LaunchPathStatus::NotAFile);
    CHECK(CheckLaunchPath(fs::path(exe.native() + fs::path::string_type(1, '"'))) == LaunchPathStatus::UnsafeCharacters);
    CHECK(CheckLaunchPath(fs::path("C:/tools/a\nb.exe")) == LaunchPathStatus::UnsafeCharacters);
    CHECK(CheckLaunchPath(fs::path("C:/tools/a\rb.exe")) == LaunchPathStatus::UnsafeCharacters);
    CHECK_FALSE(LaunchPathStatusText(LaunchPathStatus::NotFound).empty());
#if defined(_WIN32)
    // An app-execution alias (a reparse point, not a regular file to
    // std::filesystem) is a program too. Desk machines carry some.
    const char* profile = std::getenv("USERPROFILE");
    const fs::path aliases = profile ? fs::path(profile) / "AppData" / "Local" / "Microsoft" / "WindowsApps" : fs::path();
    std::error_code ec;
    fs::path alias;
    if (!aliases.empty())
        for (auto it = fs::directory_iterator(aliases, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
            if (it->path().extension() == ".exe") { alias = it->path(); break; }
    if (!alias.empty())
    {
        INFO(alias.string());
        CHECK(CheckLaunchPath(alias) == LaunchPathStatus::Ok);
    }
#endif
    // The quoting the launch sites share (RuntimeLaunch / IdeLaunch / Diagnostics).
    CHECK(QuoteWindowsArg(L"plain") == L"plain");
    CHECK(QuoteWindowsArg(L"C:\\Program Files\\x.exe") == L"\"C:\\Program Files\\x.exe\"");
    CHECK(QuoteWindowsArg(L"a\"b") == L"\"a\\\"b\"");
    CHECK(QuoteWindowsArg(L"") == L"\"\"");
    std::error_code ec2;
    fs::remove_all(dir, ec2);
}

TEST_CASE("S7-SEC: a build tool override that is not a launchable file falls back to discovery", "[launch][build]")
{
    const Test::ScopedCodeLayer codeLayer;
    if (!Test::InThisBuild("build.msbuildPath")) return;
    const fs::path dir = Scratch("toolchain");
    const fs::path exe = MakeExe(dir, "msbuild-fake.exe");
    CVarRegistry& reg = CVarRegistry::Get();
    const CVarHandle h = reg.Find("build.msbuildPath");
    reg.Set(h, CVarValue::String(exe.string()), SetBy::Code);
    reg.PublishImmediate();
    CHECK(Toolchain::ResolveMsBuild() == fs::absolute(exe).lexically_normal());
    reg.Set(h, CVarValue::String(exe.string() + "\" & calc.exe & \""), SetBy::Code);   // injection-shaped
    reg.PublishImmediate();
    CHECK(Toolchain::ResolveMsBuild() != fs::path(exe.string() + "\" & calc.exe & \""));
    reg.Set(h, CVarValue::String(dir.string()), SetBy::Code);                        // a directory
    reg.PublishImmediate();
    CHECK(Toolchain::ResolveMsBuild() != fs::absolute(dir).lexically_normal());
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("S7-SEC: the crash reporter path must be a launchable file, else the bundled reporter is used", "[launch][diag]")
{
    const fs::path dir = Scratch("reporter");
    const fs::path exe = MakeExe(dir, "MyReporter.exe");
    const fs::path exeDir = dir / "bin";
    std::string refused;
    CHECK(Diagnostics::ReporterExeFor(exe.string(), exeDir, &refused) == exe);
    CHECK(refused.empty());
    CHECK(Diagnostics::ReporterExeFor("", exeDir, &refused) == exeDir / "ArcaneCrashReporter.exe");
    CHECK(refused.empty());
    CHECK(Diagnostics::ReporterExeFor((dir / "missing.exe").string(), exeDir, &refused) == exeDir / "ArcaneCrashReporter.exe");
    CHECK(refused.find("missing.exe") != std::string::npos);
    refused.clear();
    CHECK(Diagnostics::ReporterExeFor(exe.string() + "\" --evil \"", exeDir, &refused) == exeDir / "ArcaneCrashReporter.exe");
    CHECK_FALSE(refused.empty());
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("S7-SEC: the module build's command line refuses a path that could inject a command", "[launch][editor]")
{
    Editor::ModuleBuild::DriverInputs in;
    in.driverExe     = "C:/Program Files/Arcane/arcbuild.exe";
    in.projectRoot   = "C:/Games/My Game";
    in.sdkRoot       = "C:/Program Files/Arcane";
    in.command       = "build";
    in.configuration = "Debug";
    CHECK_FALSE(Editor::ModuleBuild::ComposeDriverCommand(in).empty());
    for (const char* bad : { "C:/Games/x\" & calc & \"", "C:/Games/x\r\ncalc", "C:/Games/x\ncalc" })
    {
        INFO(bad);
        Editor::ModuleBuild::DriverInputs b = in;
        b.projectRoot = bad;
        CHECK(Editor::ModuleBuild::ComposeDriverCommand(b).empty());
        b = in;
        b.sdkRoot = bad;
        CHECK(Editor::ModuleBuild::ComposeDriverCommand(b).empty());
        b = in;
        b.driverExe = bad;
        CHECK(Editor::ModuleBuild::ComposeDriverCommand(b).empty());
    }
    Editor::ModuleBuild::DriverInputs b = in;
    b.configuration = "Debug & calc";   // the two bare tokens are words
    CHECK(Editor::ModuleBuild::ComposeDriverCommand(b).empty());
    b = in;
    b.command = "build|calc";
    CHECK(Editor::ModuleBuild::ComposeDriverCommand(b).empty());
}

// Fix round 1: arccook's project Config read is a load, so a key the Project
// rung refuses warns ONCE per key per load there too, naming the folder, the
// key and the reason (it used to be refused silently).
TEST_CASE("S7-SEC: arccook's project Config read warns once per refused key", "[cvar][launch][pipeline]")
{
    const RungCleanup cleanup;
    const fs::path dir = Scratch("arccook");
    const fs::path project = dir / "P";
    WriteProgramKeys(project / "Config", MakeExe(dir, "evil.exe"));
    const std::string folder = (project / "Config").generic_string();
    CVarRegistry& reg = CVarRegistry::Get();
    for (int load = 1; load <= 2; ++load)
    {
        INFO("load " << load);
        LogCapture log;
        (void)AssetPipeline::ApplyProjectCookConfig(project);
        for (const std::string& name : LaunchingNamesInThisBuild())
        {
            INFO(name);
            CHECK(reg.Get(reg.Find(name))->AsString().empty());
            CHECK_FALSE(HoldsRung(name, SetBy::Project));
            CHECK(CountOf(log.text, "config.cvar.refused '" + name + "'") == 1);
            const std::string line = log.LineFor(name);
            INFO(line);
            CHECK(line.find(folder) != std::string::npos);
            CHECK(line.find(std::string(kProgramReason)) != std::string::npos);
        }
        if constexpr (kDistBuild)
            CHECK(log.text.find("config.cvar.refused 'build.premakePath'") == std::string::npos);   // compiled out: silent
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// Fix round 1: the report stem rides the crash reporter's command line in a
// plain quote wrap, built on the crash thread where QuoteWindowsArg (heap) is
// forbidden. So a dumpDir holding '"', CR or LF is refused for the default
// dir at Install/RetargetDumpDir, and the crash thread scans the stem
// (ReporterStemSafe) and skips the spawn as a backstop.
TEST_CASE("S7-SEC: a dump dir or report stem that could break the reporter's command line is refused", "[launch][diag]")
{
    const fs::path exeDir = fs::path("C:/Arcane/bin");
    std::string refused;
    CHECK(Diagnostics::ReportDirFor("", exeDir, &refused) == exeDir / "diagnostics");
    CHECK(refused.empty());
    CHECK(Diagnostics::ReportDirFor("D:/Dumps/My Game", exeDir, &refused) == fs::path("D:/Dumps/My Game"));
    CHECK(refused.empty());
    for (const char* bad : { "D:/Dumps/x\" --relaunch \"calc.exe", "D:/Dumps/x\rcalc", "D:/Dumps/x\ncalc" })
    {
        INFO(bad);
        refused.clear();
        CHECK(Diagnostics::ReportDirFor(bad, exeDir, &refused) == exeDir / "diagnostics");
        CHECK(refused.find("diagnostics.dumpDir") != std::string::npos);
        CHECK(refused.find("quote or a line break") != std::string::npos);
    }

    CHECK(Diagnostics::ReporterStemSafe("C:\\Arcane\\bin\\diagnostics\\ArcaneEditor-2026-10-07_12-00-00-pid1"));
    CHECK(Diagnostics::ReporterStemSafe("D:/Dumps/My Game/ArcaneEditor-x"));
    CHECK_FALSE(Diagnostics::ReporterStemSafe(nullptr));
    CHECK_FALSE(Diagnostics::ReporterStemSafe(""));
    CHECK_FALSE(Diagnostics::ReporterStemSafe("D:/Dumps/x\" --relaunch \"calc.exe/ArcaneEditor-x"));
    CHECK_FALSE(Diagnostics::ReporterStemSafe("D:/Dumps/x\r/ArcaneEditor-x"));
    CHECK_FALSE(Diagnostics::ReporterStemSafe("D:/Dumps/x\n/ArcaneEditor-x"));
}

#if defined(_WIN32)
// Fix round 1: the live path. Install and RetargetDumpDir with a dumpDir that
// holds a quote or a line break warn once each and write the reports beside
// the exe instead; the reporter is never spawned here (spawnReporter off).
TEST_CASE("S7-SEC: Install and RetargetDumpDir refuse a dump dir that could break the reporter's command line", "[launch][diag]")
{
    const fs::path dir = Scratch("dumpdir");
    const fs::path good = dir / "good";
    const fs::path expected = fs::path(ExecutablePathUtf8()).parent_path() / "diagnostics";

    Diagnostics::Config cfg;
    cfg.appName             = "S7SecDumpDir";
    cfg.dumpDir             = (dir / "x\" --relaunch \"calc.exe").string();
    cfg.logDir              = (dir / "Logs").string();   // never the exe dir's Logs
    cfg.installCrashHandler = false;
    cfg.startHangWatchdog   = false;
    cfg.spawnReporter       = false;

    std::vector<std::string> written;
    {
        LogCapture log;
        struct Armed
        {
            explicit Armed(const Diagnostics::Config& c) { Diagnostics::Install(c); }
            ~Armed() { Diagnostics::Shutdown(); }
            Armed(const Armed&) = delete;
            Armed& operator=(const Armed&) = delete;
        } armed(cfg);
        struct Restore
        {
            fs::path to;
            ~Restore() { Diagnostics::RetargetDumpDir(to); }
        } restore{ good };

        CHECK(CountOf(log.text, "diagnostics.dumpDir '") == 1);
        const std::string first = Diagnostics::WriteReport("s7-sec dumpdir probe");
        REQUIRE_FALSE(first.empty());
        written.push_back(first);
        CHECK(first.find('"') == std::string::npos);
        CHECK(fs::equivalent(fs::path(first).parent_path(), expected));

        Diagnostics::RetargetDumpDir(dir / "y\ncalc");
        CHECK(CountOf(log.text, "diagnostics.dumpDir '") == 2);
        const std::string second = Diagnostics::WriteReport("s7-sec dumpdir probe");
        REQUIRE_FALSE(second.empty());
        written.push_back(second);
        CHECK(fs::equivalent(fs::path(second).parent_path(), expected));

        Diagnostics::RetargetDumpDir(good);   // a clean dir is honoured again, silently
        CHECK(CountOf(log.text, "diagnostics.dumpDir '") == 2);
        const std::string third = Diagnostics::WriteReport("s7-sec dumpdir probe");
        REQUIRE_FALSE(third.empty());
        CHECK(fs::equivalent(fs::path(third).parent_path(), good));
    }

    // The reports this case wrote beside the exe: every <stem>.* sibling.
    std::error_code ec;
    for (const std::string& txt : written)
    {
        const std::string stem = fs::path(txt).stem().string();
        std::vector<fs::path> mine;
        for (const auto& entry : fs::directory_iterator(expected, ec))
            if (entry.path().filename().string().starts_with(stem))
                mine.push_back(entry.path());
        for (const fs::path& p : mine)
            fs::remove(p, ec);
    }
    fs::remove_all(dir, ec);
}

// Fix round 1: the crash monitor's line (LaunchMonitor, at Install) quotes the
// exe and the session path with QuoteWindowsArg, so the reporter reads every
// argument back exactly -- a session path holding a space or a quote included.
TEST_CASE("S7-SEC: the crash monitor's command line quotes the exe and the session path", "[launch][diag]")
{
    const std::wstring exe = L"C:\\Program Files\\Arcane\\ArcaneCrashReporter.exe";
    const std::wstring sessions[] = {
        L"C:\\Arcane\\diagnostics\\ArcaneEditor-pid42.session",
        L"D:\\My Dumps\\x\" --relaunch \"calc.exe\\Editor-pid42.session",
        L"D:\\trailing\\",
    };
    for (const std::wstring& session : sessions)
    {
        const Diagnostics::MonitorCommand line = Diagnostics::MonitorCommandFor(exe, 42, session, true, L" --deadline 5");
        const std::wstring cmd = line.head + L"1234" + line.tail;
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(cmd.c_str(), &argc);
        REQUIRE(argv);
        const std::vector<std::wstring> args(argv, argv + argc);
        LocalFree(argv);
        const std::vector<std::wstring> want = { exe, L"--monitor", L"42", L"--host-handle", L"1234",
                                                 L"--session", session, L"--unattended", L"--deadline", L"5" };
        CHECK(args == want);
    }
}
#endif
