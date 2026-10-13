// ARC_SETTINGS and Settings<T>() (settings arc S2, spec s4.3, s4.6): one cvar
// per reflected field with the attribute metadata, the typed block of the
// published snapshot, sharing across unrelated publishes, refusals, and the
// Dist-style registry that compiles a Dev field out.
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Config/CVarDecl.hpp>
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Config/Settings.hpp>

#include "Helpers/SettingsProbe.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The process registry, at static init -- this is the only TU that does it.
ARC_SETTINGS(SettingsProbe::ProbeSettings);

using namespace Arcane;

namespace
{
    constexpr std::string_view kModule = "settings-struct-test";

    std::string Name(std::string_view field) { return "tests.settingsProbe." + std::string(field); }

    struct Unreflected
    {
        int x = 0;
    };

    struct NotSettings
    {
        std::int32_t x = 1;
    };

    ARC_REFLECT_TYPE(NotSettings)
        ARC_REFLECT_FIELD(NotSettings, x)
            ARC_REFLECT_ATTR(Tooltip, "x")
    ARC_END_REFLECT_TYPE()

    // The Settings attribute AFTER a field: that field has no codec.
    struct LateAttr
    {
        std::int32_t early = 1;
        std::int32_t late = 2;
    };

    ARC_REFLECT_TYPE(LateAttr)
        ARC_REFLECT_FIELD(LateAttr, early)
            ARC_REFLECT_ATTR(Tooltip, "Reflected before the Settings attribute.")
        ARC_REFLECT_TYPE_ATTR(Settings, "tests.lateAttr", ::Arcane::SettingScope::Project,
                                 ::Arcane::ApplyMode::Live, ::Arcane::Audience::Game)
        ARC_REFLECT_FIELD(LateAttr, late)
            ARC_REFLECT_ATTR(Tooltip, "Reflected after it.")
    ARC_END_REFLECT_TYPE()

    const SettingsFieldDesc& FieldOf(const SettingsTypeDesc& d, std::string_view field)
    {
        for (const SettingsFieldDesc& f : d.fields)
            if (f.name == Name(field))
                return f;
        FAIL("no field " << field);
        return d.fields.front();
    }

    constexpr std::uint64_t kProbeHash = Astra::TypeID<SettingsProbe::ProbeSettings>::Hash();
}

TEST_CASE("DescribeSettings maps every field and attribute onto a cvar description", "[settings]")
{
    const SettingsTypeDesc d = DescribeSettings<SettingsProbe::ProbeSettings>(kModule);
    REQUIRE(d.error.empty());
    CHECK(d.category == "tests.settingsProbe");
    CHECK(d.module == kModule);
    CHECK(d.typeHash == kProbeHash);
    REQUIRE(d.fields.size() == 13);
    CHECK(d.fields.front().name == Name("toggle"));             // field order IS CVarDesc::order
    CHECK(d.fields.back().name == Name("quality"));

    const SettingsFieldDesc& count = FieldOf(d, "count");
    CHECK(count.type == CVarType::Int32);
    CHECK(count.defaultValue == CVarValue::Int32(7));
    REQUIRE(count.min.has_value());
    CHECK(*count.min == CVarValue::Int32(1));
    CHECK(*count.max == CVarValue::Int32(100));
    CHECK(count.help == "An int32 with a range.");
    CHECK(count.displayName == "Item count");
    CHECK(count.keywords == "items amount");
    CHECK(count.flags == CVarFlags::Archive);                   // a Pref scope persists
    CHECK(count.audience == Audience::Editor);
    CHECK(count.scope == SettingScope::PreferencesProject);
    CHECK(count.apply == ApplyMode::Live);

    CHECK(FieldOf(d, "bytes").flags == (CVarFlags::Archive | CVarFlags::Dev));
    CHECK(HasFlag(FieldOf(d, "ratio").flags, CVarFlags::Deterministic));
    CHECK(FieldOf(d, "ratio").apply == ApplyMode::NextWorld);
    CHECK(FieldOf(d, "seconds").scope == SettingScope::Project);
    CHECK_FALSE(HasFlag(FieldOf(d, "seconds").flags, CVarFlags::Archive));   // Project scope: the window writes the Project rung
    CHECK(FieldOf(d, "label").widget == "path:file");
    CHECK(FieldOf(d, "label").aliases == std::vector<std::string>{ Name("caption") });
    CHECK(FieldOf(d, "tint").audience == Audience::PlayerSafe);
    CHECK(FieldOf(d, "tint").defaultValue.AsColor().g == 0.65f);
    CHECK(FieldOf(d, "quality").type == CVarType::Enum);
    CHECK(FieldOf(d, "quality").enumNames == std::vector<std::string>{ "Low", "Medium", "High" });
    CHECK(FieldOf(d, "quality").defaultValue.AsEnum() == 2);
    for (const SettingsFieldDesc& f : d.fields)
    {
        INFO(f.name);
        CHECK(f.write != nullptr);
    }
}

TEST_CASE("RegisterSettings registers one cvar per field and Settings<T>() reads the published block", "[settings]")
{
    CVarRegistry reg;
    REQUIRE(RegisterSettings<SettingsProbe::ProbeSettings>(reg, kModule));
    for (const char* f : { "toggle", "count", "mask", "offset", "bytes", "ratio", "seconds", "label", "tint", "size",
                           "axis", "rect", "quality" })
    {
        INFO(f);
        CHECK_FALSE(reg.Find(Name(f)).IsStale());
    }
    CHECK(reg.Get(reg.Find(Name("count")))->AsInt32() == 7);
    CHECK(reg.Explain(Name("count"))->help == "An int32 with a range.");
    CHECK(HasFlag(reg.Explain(Name("ratio"))->flags, CVarFlags::Deterministic));

    const SettingsProbe::ProbeSettings& before = Settings<SettingsProbe::ProbeSettings>(reg);
    CHECK(before.count == 7);
    CHECK(before.label == "probe");
    CHECK(before.quality == SettingsProbe::Quality::High);

    REQUIRE(reg.Set(reg.Find(Name("count")), CVarValue::Int32(500), SetBy::Code) == SetResult::Applied);
    REQUIRE(reg.Set(reg.Find(Name("quality")), CVarValue::Enum(1), SetBy::Code) == SetResult::Applied);
    REQUIRE(reg.Set(reg.Find(Name("label")), CVarValue::String("x"), SetBy::Code) == SetResult::Applied);
    CHECK(Settings<SettingsProbe::ProbeSettings>(reg).count == 7);      // pending until Publish
    reg.Publish();
    const SettingsProbe::ProbeSettings& after = Settings<SettingsProbe::ProbeSettings>(reg);
    CHECK(after.count == 100);                                           // clamped by Range(1, 100)
    CHECK(after.quality == SettingsProbe::Quality::Medium);
    CHECK(after.label == "x");
    CHECK(after.toggle == true);
}

TEST_CASE("A publish that touches no field of a settings struct shares its block", "[settings]")
{
    CVarRegistry reg;
    REQUIRE(RegisterSettings<SettingsProbe::ProbeSettings>(reg, kModule));
    reg.Publish();
    const void* block = reg.SettingsBlock(kProbeHash);
    REQUIRE(block != nullptr);
    REQUIRE(reg.Set(reg.Find("console.historySize"), CVarValue::Int32(65), SetBy::Code) == SetResult::Applied);
    reg.Publish();
    CHECK(reg.SettingsBlock(kProbeHash) == block);
    REQUIRE(reg.Set(reg.Find(Name("toggle")), CVarValue::Bool(false), SetBy::Code) == SetResult::Applied);
    reg.Publish();
    CHECK(reg.SettingsBlock(kProbeHash) != block);
    CHECK_FALSE(Settings<SettingsProbe::ProbeSettings>(reg).toggle);
}

TEST_CASE("Registration and module unload preserve typed settings and retire snapshots", "[settings]")
{
    CVarRegistry reg;
    REQUIRE(RegisterSettings<SettingsProbe::ProbeSettings>(reg, kModule));
    REQUIRE(reg.Set(reg.Find(Name("count")), CVarValue::Int32(23), SetBy::Code) == SetResult::Applied);
    reg.Publish();
    const void* block = reg.SettingsBlock(kProbeHash);
    REQUIRE(block != nullptr);
    std::weak_ptr<const CVarSnapshot> original = reg.Snapshot();

    REQUIRE_FALSE(reg.Register(CVarDesc{ "tests.later.first", CVarType::Bool, CVarValue::Bool(true),
                                          {}, {}, {}, "First later cvar.", "later-module" }).IsStale());
    CHECK(reg.SettingsBlock(kProbeHash) == block);
    CHECK(Settings<SettingsProbe::ProbeSettings>(reg).count == 23);
    CHECK_FALSE(original.expired());

    REQUIRE_FALSE(reg.Register(CVarDesc{ "tests.later.second", CVarType::Bool, CVarValue::Bool(true),
                                          {}, {}, {}, "Second later cvar.", "later-module" }).IsStale());
    CHECK(reg.SettingsBlock(kProbeHash) == block);
    CHECK_FALSE(original.expired());

    reg.UnregisterModule("later-module");
    CHECK(reg.SettingsBlock(kProbeHash) == block);
    CHECK(Settings<SettingsProbe::ProbeSettings>(reg).count == 23);
    CHECK(original.expired());
}

TEST_CASE("AliasName on a settings field resolves the old name", "[settings]")
{
    CVarRegistry reg;
    REQUIRE(RegisterSettings<SettingsProbe::ProbeSettings>(reg, kModule));
    const ExecResult r = reg.Execute(Name("caption") + " renamed", CVarContext::Editor);
    CHECK(r.ok);
    reg.Publish();
    CHECK(Settings<SettingsProbe::ProbeSettings>(reg).label == "renamed");
}

TEST_CASE("RegisterSettings refuses a struct with no reflection, no Settings attribute, a field before it, or a second registration", "[settings]")
{
    CVarRegistry reg;
    CHECK_FALSE(RegisterSettings<Unreflected>(reg, kModule));
    CHECK(reg.LastError().find("no ARC_REFLECT_TYPE") != std::string::npos);
    CHECK_FALSE(RegisterSettings<NotSettings>(reg, kModule));
    CHECK(reg.LastError().find("ARC_REFLECT_TYPE_ATTR(Settings") != std::string::npos);
    CHECK_FALSE(RegisterSettings<LateAttr>(reg, kModule));
    CHECK(reg.LastError().find("field 'early' has no settings codec") != std::string::npos);
    CHECK(reg.Find("tests.lateAttr.late").IsStale());                    // nothing half-registered
    REQUIRE(RegisterSettings<SettingsProbe::ProbeSettings>(reg, kModule));
    CHECK_FALSE(RegisterSettings<SettingsProbe::ProbeSettings>(reg, "another-module"));
    CHECK(reg.LastError().find("already registered by module 'settings-struct-test'") != std::string::npos);
    CHECK(Settings<Unreflected>(reg).x == 0);                            // never registered: the struct's defaults
}

TEST_CASE("A Dev field that a Dist-style registry refuses keeps its declared default in the block", "[settings]")
{
    CVarRegistry reg{ false };
    REQUIRE(RegisterSettings<SettingsProbe::ProbeSettings>(reg, kModule));
    CHECK(reg.Find(Name("bytes")).IsStale());
    CHECK_FALSE(reg.Find(Name("count")).IsStale());
    reg.Publish();
    CHECK(Settings<SettingsProbe::ProbeSettings>(reg).bytes == 16384u);
}

TEST_CASE("ARC_SETTINGS registers on the process registry at static init", "[settings]")
{
    CVarRegistry& reg = CVarRegistry::Get();
    const CVarHandle h = reg.Find(Name("count"));
    REQUIRE_FALSE(h.IsStale());
    CHECK(Settings<SettingsProbe::ProbeSettings>().count == reg.Get(h)->AsInt32());
    CHECK(Settings<SettingsProbe::ProbeSettings>().label == "probe");
}

// S2-H item 6: the registration macros name their statics with __COUNTER__,
// so two expansions on ONE line -- here through a helper macro -- compile and
// both register. With __LINE__ the second was a redefinition.
namespace SameLine
{
    struct FirstSettings
    {
        bool on = true;
    };

    ARC_REFLECT_TYPE(FirstSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "tests.sameLineFirst", ::Arcane::SettingScope::Project,
                                 ::Arcane::ApplyMode::Live, ::Arcane::Audience::Game)
        ARC_REFLECT_FIELD(FirstSettings, on)
            ARC_REFLECT_ATTR(Tooltip, "The first of two registrations on one line.")
    ARC_END_REFLECT_TYPE()

    struct SecondSettings
    {
        bool on = false;
    };

    ARC_REFLECT_TYPE(SecondSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "tests.sameLineSecond", ::Arcane::SettingScope::Project,
                                 ::Arcane::ApplyMode::Live, ::Arcane::Audience::Game)
        ARC_REFLECT_FIELD(SecondSettings, on)
            ARC_REFLECT_ATTR(Tooltip, "The second of two registrations on one line.")
    ARC_END_REFLECT_TYPE()

    ARC_CVAR(cvar_sameLineTarget, "tests.sameLine.target", std::int32_t, 1, .help = "Two same-line aliases point here.");

    ::Arcane::CommandResult First(std::string_view, void*) { return { true, "first" }; }
    ::Arcane::CommandResult Second(std::string_view, void*) { return { true, "second" }; }
}

#define ARC_TEST_TWO_SETTINGS(a, b) ARC_SETTINGS(a); ARC_SETTINGS(b)
#define ARC_TEST_TWO_ALIASES(oldA, oldB, target) ARC_CVAR_ALIAS(oldA, target); ARC_CVAR_ALIAS(oldB, target)
#define ARC_TEST_TWO_COMMANDS(nameA, fnA, nameB, fnB)     ARC_COMMAND(nameA, ::Arcane::CVarFlags::None, "Same-line command probe.", fnA);     ARC_COMMAND(nameB, ::Arcane::CVarFlags::None, "Same-line command probe.", fnB)

ARC_TEST_TWO_SETTINGS(SameLine::FirstSettings, SameLine::SecondSettings);
ARC_TEST_TWO_ALIASES("tests.sameLine.oldA", "tests.sameLine.oldB", "tests.sameLine.target");
ARC_TEST_TWO_COMMANDS("tests.sameLine.commandA", &SameLine::First, "tests.sameLine.commandB", &SameLine::Second);

TEST_CASE("ARC_SETTINGS, ARC_CVAR_ALIAS and ARC_COMMAND expand twice on one line and register both", "[settings][cvar]")
{
    CVarRegistry& reg = CVarRegistry::Get();
    CHECK_FALSE(reg.Find("tests.sameLineFirst.on").IsStale());
    CHECK_FALSE(reg.Find("tests.sameLineSecond.on").IsStale());
    CHECK(Settings<SameLine::FirstSettings>().on);
    CHECK_FALSE(Settings<SameLine::SecondSettings>().on);

    const auto aliases = reg.Aliases();
    const auto aliased = [&](std::string_view oldName) {
        return std::find(aliases.begin(), aliases.end(),
                         std::pair<std::string, std::string>(std::string(oldName), "tests.sameLine.target")) != aliases.end();
    };
    CHECK(aliased("tests.sameLine.oldA"));
    CHECK(aliased("tests.sameLine.oldB"));

    CHECK(reg.Execute("tests.sameLine.commandA", CVarContext::Editor).text == "first");
    CHECK(reg.Execute("tests.sameLine.commandB", CVarContext::Editor).text == "second");
}
