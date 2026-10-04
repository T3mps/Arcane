// Settings spec s4.8 / s12: a config key no cvar declares, or a value of the
// wrong JSON type, is reported -- not applied -- and surfaces in the Problems
// panel with the file and key as its locator. [cvar][diagnostics]

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/Diagnostics.hpp>
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Config/CVarConfig.hpp>
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Project/Project.hpp>

#include "Helpers/CVarTestDesc.hpp"
#include "Helpers/TestTypeContext.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

using namespace Arcane;

namespace
{
    namespace fs = std::filesystem;

    struct Capture { std::vector<std::pair<std::string, std::vector<Diagnostic>>> calls; };
    void CaptureSink(std::string_view key, std::span<const Diagnostic> diags, void* user)
    {
        static_cast<Capture*>(user)->calls.emplace_back(std::string(key), std::vector<Diagnostic>(diags.begin(), diags.end()));
    }
    const std::vector<Diagnostic>* Last(const Capture& c, std::string_view key)
    {
        for (auto it = c.calls.rbegin(); it != c.calls.rend(); ++it)
            if (it->first == key) return &it->second;
        return nullptr;
    }
    fs::path Scratch(const char* tag)
    {
        const fs::path d = fs::temp_directory_path() / (std::string("arcane_cvar_diag_") + tag);
        std::error_code ec;
        fs::remove_all(d, ec);
        fs::create_directories(d, ec);
        return d;
    }
    void Write(const fs::path& p, const std::string& text)
    {
        fs::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << text;
    }
    const CVarConfigIssue* Find(const std::vector<CVarConfigIssue>& issues, std::string_view key)
    {
        for (const CVarConfigIssue& i : issues) if (i.key == key) return &i;
        return nullptr;
    }
}

TEST_CASE("ValidateCVarLayers reports unknown keys and type mismatches with file, key and line; it applies nothing", "[cvar][diagnostics]")
{
    const fs::path dir = Scratch("validate");
    Write(dir / "Config" / "test.json", "{\n  \"knob\": \"four\",\n  \"nope\": 1,\n  \"nested\": { \"deep\": true }\n}\n");
    CVarRegistry reg;
    const CVarHandle knob = reg.Register(Test::Desc("test.knob", CVarValue::Int32(3)));
    REQUIRE_FALSE(knob.IsStale());
    LayerSources layers;
    layers.dirs.push_back(CVarLayerDir{ SetBy::Project, dir / "Config", "project" });

    const std::vector<CVarConfigIssue> issues = ValidateCVarLayers(reg, layers);
    REQUIRE(issues.size() == 3);
    const CVarConfigIssue* mismatch = Find(issues, "test.knob");
    REQUIRE(mismatch);
    CHECK(mismatch->kind == CVarConfigIssue::Kind::TypeMismatch);
    CHECK(mismatch->file.filename() == "test.json");
    CHECK(mismatch->line == 2);
    REQUIRE(Find(issues, "test.nope"));
    CHECK(Find(issues, "test.nope")->kind == CVarConfigIssue::Kind::UnknownKey);
    CHECK(Find(issues, "test.nope")->line == 3);
    REQUIRE(Find(issues, "test.nested.deep"));
    CHECK(Find(issues, "test.nested.deep")->line == 4);
    reg.Publish();
    CHECK(reg.Get(knob)->AsInt32() == 3);
    CHECK(reg.Explain("test.knob")->history.size() == 1);        // validation applied nothing

    const CVarApplyReport applied = ApplyCVarDirectory(reg, dir / "Config", SetBy::Project, "project");
    CHECK(applied.typeMismatches == std::vector<std::string>{ "test.knob" });
    CHECK(applied.unknownKeys.size() == 2);
    reg.Publish();
    CHECK(reg.Get(knob)->AsInt32() == 3);                        // the mismatch was refused
    std::error_code ec; fs::remove_all(dir, ec);
}

TEST_CASE("a Dev setting a Dist build compiled out is not an unknown key", "[cvar][diagnostics]")
{
    const fs::path dir = Scratch("dist");
    Write(dir / "Config" / "test.json", R"({ "devOnly": 1 })");
    CVarRegistry dist{ false };
    REQUIRE(dist.Register(Test::Desc("test.devOnly", CVarValue::Int32(0), Audience::Game, CVarFlags::Dev)).IsStale());
    CHECK(dist.IsCompiledOut("test.devOnly"));
    LayerSources layers;
    layers.dirs.push_back(CVarLayerDir{ SetBy::Project, dir / "Config", "project" });
    CHECK(ValidateCVarLayers(dist, layers).empty());
    std::error_code ec; fs::remove_all(dir, ec);
}

TEST_CASE("PublishCVarConfigDiagnostics: one Problems row per issue under config.cvars, a File locator at the key's line", "[cvar][diagnostics]")
{
    Capture cap;
    Diagnostics::SetSink(&CaptureSink, &cap);
    PublishCVarConfigDiagnostics({
        CVarConfigIssue{ CVarConfigIssue::Kind::UnknownKey, "C:/P/Config/render.json", "render.bogus", 3 },
        CVarConfigIssue{ CVarConfigIssue::Kind::TypeMismatch, "C:/P/Config/render.json", "render.meshCull", 2 },
    });
    const std::vector<Diagnostic>* rows = Last(cap, "config.cvars");
    REQUIRE(rows);
    REQUIRE(rows->size() == 2);
    CHECK((*rows)[0].code == "config.cvar.unknown-key");
    CHECK((*rows)[0].severity == DiagSeverity::Warning);
    CHECK((*rows)[0].scope == DiagScope::Project);
    CHECK((*rows)[0].message.find("render.bogus") != std::string::npos);
    CHECK((*rows)[0].locator.kind == DiagLocator::Kind::File);
    CHECK((*rows)[0].locator.file == "C:/P/Config/render.json");
    CHECK((*rows)[0].locator.line == 3);
    CHECK((*rows)[1].code == "config.cvar.type-mismatch");
    CHECK((*rows)[1].severity == DiagSeverity::Error);
    PublishCVarConfigDiagnostics({});
    rows = Last(cap, "config.cvars");
    REQUIRE(rows);
    CHECK(rows->empty());                                        // the whole set is republished: fixed = gone
    Diagnostics::SetSink(nullptr, nullptr);
}

TEST_CASE("Runtime::OpenProject publishes the project's config issues; a fixed file clears them on reopen", "[cvar][diagnostics][project]")
{
    Capture cap;
    Diagnostics::SetSink(&CaptureSink, &cap);
    const fs::path dir = Scratch("runtime");
    REQUIRE(Project::Create(dir / "P", "DiagProbe").has_value());
    Write(dir / "P" / "Config" / "cvardiagtest.json", R"({ "bogus": 1 })");
    Runtime rt(Test::Process());
    REQUIRE(rt.OpenProject(dir / "P"));
    const std::vector<Diagnostic>* rows = Last(cap, "config.cvars");
    REQUIRE(rows);
    REQUIRE(rows->size() == 1);
    CHECK((*rows)[0].message.find("cvardiagtest.bogus") != std::string::npos);
    CHECK(fs::path((*rows)[0].locator.file).filename() == "cvardiagtest.json");

    Write(dir / "P" / "Config" / "cvardiagtest.json", "{}");
    REQUIRE(rt.OpenProject(dir / "P"));
    rows = Last(cap, "config.cvars");
    REQUIRE(rows);
    CHECK(rows->empty());
    rt.CloseProject();
    Diagnostics::SetSink(nullptr, nullptr);
    std::error_code ec; fs::remove_all(dir, ec);
}
