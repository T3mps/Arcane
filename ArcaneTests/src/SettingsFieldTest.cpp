// The settings-field map (settings arc S2, spec s4.3): the compile-time type
// map, the codec ARC_REFLECT_FIELD attaches inside a Settings block (and
// ONLY there), and the Arcane::Attr settings attributes. The compile-FAIL half
// (an unmappable field stops the build) is scripts/settings-compile-fail.ps1.
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Reflection.hpp>

#include "Helpers/SettingsProbe.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace Arcane;

namespace
{
    static_assert(Detail::kIsSettingsFieldType<bool>);
    static_assert(Detail::kIsSettingsFieldType<std::int32_t>);
    static_assert(Detail::kIsSettingsFieldType<std::uint32_t>);
    static_assert(Detail::kIsSettingsFieldType<std::int64_t>);
    static_assert(Detail::kIsSettingsFieldType<std::uint64_t>);
    static_assert(Detail::kIsSettingsFieldType<std::size_t>);   // == std::uint64_t on x64 MSVC
    static_assert(Detail::kIsSettingsFieldType<float>);
    static_assert(Detail::kIsSettingsFieldType<double>);
    static_assert(Detail::kIsSettingsFieldType<std::string>);
    static_assert(Detail::kIsSettingsFieldType<CVarColor>);
    static_assert(Detail::kIsSettingsFieldType<CVarVec2>);
    static_assert(Detail::kIsSettingsFieldType<CVarVec3>);
    static_assert(Detail::kIsSettingsFieldType<CVarVec4>);
    static_assert(Detail::kIsSettingsFieldType<SettingsProbe::Quality>);
    static_assert(!Detail::kIsSettingsFieldType<std::vector<int>>);
    static_assert(!Detail::kIsSettingsFieldType<long>);           // 32-bit on MSVC, but not std::int32_t
    static_assert(!Detail::kIsSettingsFieldType<std::uint8_t>);
    static_assert(!Detail::kIsSettingsFieldType<std::int16_t>);
    static_assert(!Detail::kIsSettingsFieldType<const char*>);
    static_assert(Detail::SettingsCVarType<std::uint64_t>() == CVarType::UInt64);
    static_assert(Detail::SettingsCVarType<CVarVec3>() == CVarType::Vec3);
    static_assert(Detail::SettingsCVarType<SettingsProbe::Quality>() == CVarType::Enum);

    // A plain reflected struct: an unmappable field is fine here, and no field
    // may carry a codec -- ARC_REFLECT_FIELD is Astra's Field() outside a
    // Settings block.
    struct PlainComponent
    {
        float            speed = 1.0f;
        std::vector<int> path;
    };

    ARC_REFLECT_TYPE(PlainComponent)
        ARC_REFLECT_FIELD(PlainComponent, speed)
        ARC_REFLECT_FIELD(PlainComponent, path)
    ARC_END_REFLECT_TYPE()

    template <class T>
    Astra::TypeMeta BuildMeta()
    {
        REQUIRE(static_cast<bool>(Astra::Detail::MetaFactory<T>::fn));
        return Astra::Detail::MetaFactory<T>::fn();
    }

    const Detail::SettingsCodec* CodecOf(const Astra::TypeMeta& meta, std::string_view field)
    {
        const Astra::FieldInfo* f = meta.GetField(field);
        REQUIRE(f != nullptr);
        return f->GetAttribute<Detail::SettingsCodec>();
    }
}

TEST_CASE("Settings attributes: the type attribute and every field attribute land on the reflected metadata", "[settings]")
{
    const Astra::TypeMeta meta = BuildMeta<SettingsProbe::ProbeSettings>();
    const auto* type = meta.GetAttribute<Attr::Settings>();
    REQUIRE(type != nullptr);
    CHECK(type->category == "tests.settingsProbe");
    CHECK(type->scope == SettingScope::PreferencesProject);
    CHECK(type->apply == ApplyMode::Live);
    CHECK(type->audience == Audience::Editor);

    const Astra::FieldInfo* count = meta.GetField("count");
    REQUIRE(count != nullptr);
    REQUIRE(count->GetAttribute<Attr::Keywords>() != nullptr);
    CHECK(count->GetAttribute<Attr::Keywords>()->words == "items amount");
    CHECK(count->GetDisplayName() == "Item count");
    CHECK(meta.GetField("ratio")->HasAttribute<Attr::Deterministic>());
    CHECK(meta.GetField("ratio")->GetAttribute<Attr::Apply>()->mode == ApplyMode::NextWorld);
    CHECK(meta.GetField("seconds")->GetAttribute<Attr::Scope>()->scope == SettingScope::Project);
    CHECK(meta.GetField("label")->GetAttribute<Attr::Widget>()->hint == "path:file");
    CHECK(meta.GetField("tint")->HasAttribute<Attr::PlayerSafe>());
    CHECK(meta.GetField("bytes")->GetAttribute<Attr::Flags>()->flags == CVarFlags::Dev);
}

TEST_CASE("ARC_REFLECT_FIELD attaches a codec inside a Settings block and nothing anywhere else", "[settings]")
{
    const Astra::TypeMeta probe = BuildMeta<SettingsProbe::ProbeSettings>();
    REQUIRE(probe.fields.size() == 13);
    for (const Astra::FieldInfo& f : probe.fields)
    {
        INFO(f.name);
        CHECK(f.GetAttribute<Detail::SettingsCodec>() != nullptr);
    }
    CHECK(CodecOf(probe, "toggle")->type == CVarType::Bool);
    CHECK(CodecOf(probe, "count")->type == CVarType::Int32);
    CHECK(CodecOf(probe, "mask")->type == CVarType::UInt32);
    CHECK(CodecOf(probe, "offset")->type == CVarType::Int64);
    CHECK(CodecOf(probe, "bytes")->type == CVarType::UInt64);
    CHECK(CodecOf(probe, "ratio")->type == CVarType::Float32);
    CHECK(CodecOf(probe, "seconds")->type == CVarType::Float64);
    CHECK(CodecOf(probe, "label")->type == CVarType::String);
    CHECK(CodecOf(probe, "tint")->type == CVarType::Color);
    CHECK(CodecOf(probe, "size")->type == CVarType::Vec2);
    CHECK(CodecOf(probe, "axis")->type == CVarType::Vec3);
    CHECK(CodecOf(probe, "rect")->type == CVarType::Vec4);
    CHECK(CodecOf(probe, "quality")->type == CVarType::Enum);

    const Astra::TypeMeta plain = BuildMeta<PlainComponent>();
    REQUIRE(plain.fields.size() == 2);
    for (const Astra::FieldInfo& f : plain.fields)
    {
        INFO(f.name);
        CHECK(f.GetAttribute<Detail::SettingsCodec>() == nullptr);
    }
}

TEST_CASE("Settings codecs round-trip every mapped type through a CVarValue", "[settings]")
{
    const Astra::TypeMeta meta = BuildMeta<SettingsProbe::ProbeSettings>();
    SettingsProbe::ProbeSettings source;
    source.toggle  = false;
    source.count   = 42;
    source.mask    = 0xF0u;
    source.offset  = -9000000000LL;
    source.bytes   = 1ull << 40;
    source.ratio   = 0.125f;
    source.seconds = 1.5;
    source.label   = "renamed";
    source.tint    = { 0.1f, 0.2f, 0.3f, 0.4f };
    source.size    = { 5.0f, 6.0f };
    source.axis    = { 1.0f, 0.0f, 0.0f };
    source.rect    = { 1.0f, 2.0f, 3.0f, 4.0f };
    source.quality = SettingsProbe::Quality::Medium;

    SettingsProbe::ProbeSettings target;
    for (const Astra::FieldInfo& f : meta.fields)
    {
        INFO(f.name);
        const auto* codec = f.GetAttribute<Detail::SettingsCodec>();
        REQUIRE(codec != nullptr);
        const CVarValue v = codec->read(&source);
        CHECK(v.type == codec->type);
        codec->write(&target, v);
    }
    CHECK(target.toggle == false);
    CHECK(target.count == 42);
    CHECK(target.mask == 0xF0u);
    CHECK(target.offset == -9000000000LL);
    CHECK(target.bytes == (1ull << 40));
    CHECK(target.ratio == 0.125f);
    CHECK(target.seconds == 1.5);
    CHECK(target.label == "renamed");
    CHECK(target.tint.r == 0.1f);
    CHECK(target.tint.a == 0.4f);
    CHECK(target.size.y == 6.0f);
    CHECK(target.axis.x == 1.0f);
    CHECK(target.rect.w == 4.0f);
    CHECK(target.quality == SettingsProbe::Quality::Medium);
}

TEST_CASE("Enum codecs store the declared INDEX, not the value, and keep the field on a bad index or type", "[settings]")
{
    const Astra::TypeMeta meta = BuildMeta<SettingsProbe::ProbeSettings>();
    const auto* codec = CodecOf(meta, "quality");
    REQUIRE(codec != nullptr);
    SettingsProbe::ProbeSettings s;                              // quality = High (= 4)
    CHECK(codec->read(&s).AsEnum() == 2);
    CHECK(codec->enumNames() == std::vector<std::string>{ "Low", "Medium", "High" });
    codec->write(&s, CVarValue::Enum(0));
    CHECK(s.quality == SettingsProbe::Quality::Low);
    codec->write(&s, CVarValue::Enum(9));                       // out of range: kept
    CHECK(s.quality == SettingsProbe::Quality::Low);
    codec->write(&s, CVarValue::Int32(1));                      // wrong type: kept
    CHECK(s.quality == SettingsProbe::Quality::Low);
    CHECK(CodecOf(meta, "count")->enumNames().empty());
}

TEST_CASE("Range bounds convert to the field's cvar type; bool, string and enum have none", "[settings]")
{
    const Astra::TypeMeta meta = BuildMeta<SettingsProbe::ProbeSettings>();
    CHECK(CodecOf(meta, "count")->bound(100.0)->AsInt32() == 100);
    CHECK(CodecOf(meta, "mask")->bound(-3.0)->AsUInt32() == 0u);  // a negative bound clamps to 0, never wraps
    CHECK(CodecOf(meta, "bytes")->bound(1e7)->AsUInt64() == 10000000u);
    CHECK(CodecOf(meta, "seconds")->bound(0.5)->AsFloat64() == 0.5);
    CHECK(CodecOf(meta, "tint")->bound(0.5)->AsColor().g == 0.5f);
    CHECK(CodecOf(meta, "rect")->bound(2.0)->AsVec4().w == 2.0f);
    CHECK_FALSE(CodecOf(meta, "toggle")->bound(1.0).has_value());
    CHECK_FALSE(CodecOf(meta, "label")->bound(1.0).has_value());
    CHECK_FALSE(CodecOf(meta, "quality")->bound(1.0).has_value());
}
