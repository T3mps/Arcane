// CVarRef + Detail::RegisterCVar (settings spec 2026-10-03 s4.3, O6): a
// declaration yields a nameable handle that reads with no string lookup,
// carries its metadata and module, and maps a reflected enum through its
// ordinal. ARC_CVAR (S1-8) expands to exactly these calls.
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Config/CVarRef.hpp>
#include <Arcane/Base/Log.hpp>

#include <spdlog/sinks/callback_sink.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace Arcane;

namespace
{
    enum class CVarRefMode : std::uint8_t { Off = 0, Low = 2, High = 7 };   // non-contiguous on purpose
    ARC_REFLECT_ENUM(CVarRefMode)
        ARC_REFLECT_ENUM_VALUE(CVarRefMode, Off)
        ARC_REFLECT_ENUM_VALUE(CVarRefMode, Low)
        ARC_REFLECT_ENUM_VALUE(CVarRefMode, High)
    ARC_END_REFLECT_ENUM()

    const CVarRef<std::int32_t> probeInt = Detail::RegisterCVar<std::int32_t>("tests.ref.int", 5,
        CVarSpec<std::int32_t>{ .min = 1, .max = 10, .flags = CVarFlags::Archive, .audience = Audience::Editor,
                                .scope = SettingScope::PreferencesProject, .help = "CVarRef int probe.",
                                .keywords = "probe ref", .order = 4 });
    const CVarRef<float> probeFloat = Detail::RegisterCVar<float>("tests.ref.float", 0.25f,
        CVarSpec<float>{ .help = "CVarRef float probe." });
    const CVarRef<CVarColor> probeColor = Detail::RegisterCVar<CVarColor>("tests.ref.color", CVarColor{ 1.0f, 0.5f, 0.0f, 1.0f },
        CVarSpec<CVarColor>{ .audience = Audience::Editor, .scope = SettingScope::PreferencesMachine, .help = "CVarRef colour probe." });
    const CVarRef<CVarVec3> probeVec = Detail::RegisterCVar<CVarVec3>("tests.ref.vec", CVarVec3{ 1.0f, 2.0f, 3.0f },
        CVarSpec<CVarVec3>{ .min = CVarVec3{ 0.0f, 0.0f, 0.0f }, .max = CVarVec3{ 10.0f, 10.0f, 10.0f }, .help = "CVarRef vector probe." });
    const CVarRef<std::string> probeText = Detail::RegisterCVar<std::string>("tests.ref.text", std::string("hello"),
        CVarSpec<std::string>{ .help = "CVarRef string probe.", .widget = "path:file" });
    const CVarRef<CVarRefMode> probeMode = Detail::RegisterCVar<CVarRefMode>("tests.ref.mode", CVarRefMode::High,
        CVarSpec<CVarRefMode>{ .apply = ApplyMode::NextWorld, .help = "CVarRef enum probe." });
}

TEST_CASE("CVarRef reads its value by handle and carries the declared metadata and module", "[cvar]")
{
    CVarRegistry& reg = CVarRegistry::Get();
    REQUIRE_FALSE(probeInt.Handle().IsStale());
    CHECK(probeInt.Name() == "tests.ref.int");
    CHECK(reg.Find("tests.ref.int") == probeInt.Handle());
    CHECK(probeInt.Get() == 5);

    REQUIRE(reg.Set(probeInt.Handle(), CVarValue::Int32(50), SetBy::Console) == SetResult::Applied);
    reg.Publish();
    CHECK(probeInt.Get() == 10);                                  // .max clamps
    REQUIRE(reg.Set(probeInt.Handle(), CVarValue::Int32(5), SetBy::Console) == SetResult::Applied);
    reg.Publish();

    const auto meta = reg.Metadata(probeInt.Handle());
    REQUIRE(meta.has_value());
    CHECK(meta->type == CVarType::Int32);
    REQUIRE(meta->min.has_value());
    CHECK(*meta->min == CVarValue::Int32(1));
    CHECK(meta->flags == CVarFlags::Archive);   // Editor audience: no derived UserSettable
    CHECK(meta->audience == Audience::Editor);
    CHECK(meta->scope == SettingScope::PreferencesProject);
    CHECK(meta->apply == ApplyMode::Live);
    CHECK(meta->help == "CVarRef int probe.");
    CHECK(meta->keywords == "probe ref");
    CHECK(meta->order == 4);
    CHECK(meta->displayName == "Int");
    CHECK(meta->categoryPath == "Tests/Ref");
    CHECK(meta->module == "tests");                               // ArcaneTests' ARC_MODULE_NAME token
}

TEST_CASE("CVarRef covers float, Color, Vec3 and string, with per-component clamping", "[cvar]")
{
    CVarRegistry& reg = CVarRegistry::Get();
    CHECK(probeFloat.Get() == 0.25f);
    CHECK(probeColor.Get() == CVarColor{ 1.0f, 0.5f, 0.0f, 1.0f });
    CHECK(probeVec.Get() == CVarVec3{ 1.0f, 2.0f, 3.0f });
    CHECK(probeText.Get() == "hello");
    CHECK(reg.Metadata(probeText.Handle())->widget == "path:file");
    CHECK(reg.Metadata(probeColor.Handle())->scope == SettingScope::PreferencesMachine);
    CHECK(reg.Metadata(probeFloat.Handle())->audience == Audience::Game);   // CVarSpec defaults
    CHECK(reg.Metadata(probeFloat.Handle())->scope == SettingScope::Project);

    REQUIRE(reg.Set(probeVec.Handle(), CVarValue::Vec3(CVarVec3{ -1.0f, 5.0f, 99.0f }), SetBy::Code) == SetResult::Applied);
    reg.Publish();
    CHECK(probeVec.Get() == CVarVec3{ 0.0f, 5.0f, 10.0f });
    REQUIRE(reg.Set(probeVec.Handle(), CVarValue::Vec3(CVarVec3{ 1.0f, 2.0f, 3.0f }), SetBy::Code) == SetResult::Applied);
    reg.Publish();
}

TEST_CASE("CVarRef<enum>: names from reflection, stored as the ordinal, read back as the enumerator", "[cvar]")
{
    CVarRegistry& reg = CVarRegistry::Get();
    REQUIRE_FALSE(probeMode.Handle().IsStale());
    CHECK(probeMode.Get() == CVarRefMode::High);
    const auto meta = reg.Metadata(probeMode.Handle());
    REQUIRE(meta.has_value());
    CHECK(meta->type == CVarType::Enum);
    CHECK(meta->enumNames == std::vector<std::string>{ "Off", "Low", "High" });
    CHECK(meta->apply == ApplyMode::NextWorld);
    CHECK(reg.Get(probeMode.Handle())->AsEnum() == 2);            // the ordinal, not High's value 7

    REQUIRE(reg.Set(probeMode.Handle(), CVarValue::Enum(1), SetBy::Code) == SetResult::Applied);
    reg.Publish();
    CHECK(probeMode.Get() == CVarRefMode::Low);
    REQUIRE(reg.Set(probeMode.Handle(), CVarValue::Enum(2), SetBy::Code) == SetResult::Applied);
    reg.Publish();

    // RegisterCVar read the names through Astra's MetaFactory and registered
    // nothing, so this module's static-init pending reflection queue was NOT
    // drained into its default TypeContext: main()'s SetTypeContext still
    // delivered the enum's metadata to the shared context.
    CHECK(Astra::GetMeta<CVarRefMode>() != nullptr);
}

TEST_CASE("a refused declaration logs once, holds a stale handle, and reads its declared default", "[cvar]")
{
    std::string captured;
    int count = 0;
    auto sink = std::make_shared<spdlog::sinks::callback_sink_mt>([&](const spdlog::details::log_msg& m) {
        captured.assign(m.payload.data(), m.payload.size());
        ++count;
    });
    Log::Engine()->sinks().push_back(sink);
    const CVarRef<std::int32_t> refused = Detail::RegisterCVar<std::int32_t>("tests.ref.noHelp", 3, CVarSpec<std::int32_t>{});
    auto& sinks = Log::Engine()->sinks();
    sinks.erase(std::remove(sinks.begin(), sinks.end(), sink), sinks.end());

    CHECK(refused.Handle().IsStale());
    CHECK(refused.Get() == 3);
    CHECK(count == 1);
    CHECK(captured.find("tests.ref.noHelp") != std::string::npos);
    CHECK(captured.find("help") != std::string::npos);
    CHECK(CVarRegistry::Get().Find("tests.ref.noHelp").IsStale());
}
