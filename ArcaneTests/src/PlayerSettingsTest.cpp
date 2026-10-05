// Settings arc S7 (spec s8.1): the PlayerSafe slice of the registry, read and
// written in the session's context. This file starts with the descriptor read
// (CVarRegistry::ListEx) that PlayerSettings::List is built on.
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Config/CVarRegistry.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

using namespace Arcane;

namespace
{
    const CVarListEntryEx* FindEntry(const std::vector<CVarListEntryEx>& list, std::string_view name)
    {
        const auto it = std::find_if(list.begin(), list.end(), [&](const CVarListEntryEx& e) { return e.name == name; });
        return it == list.end() ? nullptr : &*it;
    }
}

TEST_CASE("CVarRegistry::ListEx carries every descriptor field a settings menu needs", "[player-settings]")
{
    CVarRegistry reg;

    CVarDesc vol;
    vol.name = "audio.masterVolume";
    vol.type = CVarType::Float32;
    vol.defaultValue = CVarValue::Float32(0.8f);
    vol.min = CVarValue::Float32(0.0f);
    vol.max = CVarValue::Float32(1.0f);
    vol.flags = CVarFlags::Archive;
    vol.help = "Master output volume.";
    vol.module = "test-s7";
    vol.audience = Audience::PlayerSafe;
    vol.scope = SettingScope::Project;
    vol.apply = ApplyMode::Live;
    vol.order = 3;
    REQUIRE_FALSE(reg.Register(vol).IsStale());

    CVarDesc quality;
    quality.name = "render.quality";
    quality.type = CVarType::Enum;
    quality.defaultValue = CVarValue::Enum(1);
    quality.enumNames = { "Low", "Medium", "High" };
    quality.help = "Overall quality preset.";
    quality.module = "test-s7";
    quality.audience = Audience::PlayerSafe;
    quality.apply = ApplyMode::Restart;
    quality.displayName = "Quality Preset";
    quality.categoryPath = "Graphics";
    REQUIRE_FALSE(reg.Register(quality).IsStale());

    CVarDesc hidden;
    hidden.name = "audio.secretKnob";
    hidden.type = CVarType::Bool;
    hidden.defaultValue = CVarValue::Bool(false);
    hidden.flags = CVarFlags::Hidden;
    hidden.module = "test-s7";
    hidden.audience = Audience::PlayerSafe;
    REQUIRE_FALSE(reg.Register(hidden).IsStale());

    REQUIRE(reg.Set(reg.Find("audio.masterVolume"), CVarValue::Float32(0.5f), SetBy::User, "user",
                    CVarContext::Editor) == SetResult::Applied);
    reg.PublishImmediate();

    const std::vector<CVarListEntryEx> list = reg.ListEx();
    const CVarListEntryEx* v = FindEntry(list, "audio.masterVolume");
    REQUIRE(v);
    CHECK(v->displayName == "Master Volume");      // derived from the last segment
    CHECK(v->categoryPath == "audio");             // derived from the dotted name
    CHECK(v->help == "Master output volume.");
    CHECK(v->type == CVarType::Float32);
    CHECK(v->audience == Audience::PlayerSafe);
    CHECK(v->scope == SettingScope::Project);
    CHECK(v->apply == ApplyMode::Live);
    CHECK(v->order == 3);
    REQUIRE(v->min);
    CHECK(v->min->AsFloat32() == 0.0f);
    REQUIRE(v->max);
    CHECK(v->max->AsFloat32() == 1.0f);
    CHECK(v->value.AsFloat32() == 0.5f);           // published
    CHECK(v->defaultValue.AsFloat32() == 0.8f);    // the Default rung, not the winner

    const CVarListEntryEx* q = FindEntry(list, "render.quality");
    REQUIRE(q);
    CHECK(q->displayName == "Quality Preset");     // declared names win over derivation
    CHECK(q->categoryPath == "Graphics");
    CHECK(q->type == CVarType::Enum);
    CHECK(q->enumNames == std::vector<std::string>{ "Low", "Medium", "High" });
    CHECK(q->value.AsEnum() == 1);
    CHECK(q->apply == ApplyMode::Restart);

    CHECK(FindEntry(list, "audio.secretKnob") == nullptr);   // Hidden is never listed
}
