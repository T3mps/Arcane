// Config files for the S1 types (settings spec 2026-10-03 s4.8, s11.2): Color
// as "#RRGGBBAA" (or [r,g,b,a] on read), Vec* as arrays, Enum by name; a
// numeric Enum is accepted with a warning and rewritten as its name; a Color
// saved at its default still counts as the default after the hex round trip.
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Config/CVarConfig.hpp>
#include <Arcane/Config/CVarFormat.hpp>
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Base/Log.hpp>

#include <spdlog/sinks/callback_sink.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

using namespace Arcane;

namespace
{
    std::string ReadText(const std::filesystem::path& file)
    {
        std::ifstream in(file, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    void WriteText(const std::filesystem::path& file, const std::string& text)
    {
        std::filesystem::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary) << text;
    }
    void Roster(CVarRegistry& reg)
    {
        const auto add = [&](const char* name, CVarType type, CVarValue def) {
            CVarDesc d{ .name = name, .type = type, .defaultValue = std::move(def), .flags = CVarFlags::Archive,
                        .help = "file probe", .module = "test" };
            if (type == CVarType::Enum) d.enumNames = { "Off", "Low", "High" };
            REQUIRE_FALSE(reg.Register(d).IsStale());
        };
        add("look.tint", CVarType::Color, CVarValue::Color(CVarColor{ 1.0f, 1.0f, 1.0f, 1.0f }));
        add("look.offset", CVarType::Vec2, CVarValue::Vec2(CVarVec2{}));
        add("look.origin", CVarType::Vec3, CVarValue::Vec3(CVarVec3{}));
        add("look.rect", CVarType::Vec4, CVarValue::Vec4(CVarVec4{}));
        add("look.mode", CVarType::Enum, CVarValue::Enum(0));
    }
    struct LogCapture
    {
        std::string last;
        int count = 0;
        std::shared_ptr<spdlog::sinks::callback_sink_mt> sink;
        LogCapture()
        {
            sink = std::make_shared<spdlog::sinks::callback_sink_mt>([this](const spdlog::details::log_msg& m) {
                last.assign(m.payload.data(), m.payload.size());
                ++count;
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
    };
}

TEST_CASE("cvar files round-trip Color, Vec and Enum as hex, arrays and names", "[cvar]")
{
    const auto user = std::filesystem::temp_directory_path() / "arcane-cvar-types-archive";
    std::filesystem::remove_all(user);

    CVarRegistry reg;
    Roster(reg);
    const CVarColor orange = *CVarColorFromHex("#FF8800FF");
    const auto set = [&](const char* name, CVarValue v) { REQUIRE(reg.Set(reg.Find(name), std::move(v), SetBy::User, "test") == SetResult::Applied); };
    set("look.tint", CVarValue::Color(orange));
    set("look.offset", CVarValue::Vec2(CVarVec2{ 1.5f, -2.0f }));
    set("look.origin", CVarValue::Vec3(CVarVec3{ 1.0f, 2.0f, 3.0f }));
    set("look.rect", CVarValue::Vec4(CVarVec4{ 0.0f, 0.25f, 0.5f, 1.0f }));
    set("look.mode", CVarValue::Enum(2));
    reg.Publish();
    WriteCVarArchive(reg, user);

    const auto doc = nlohmann::json::parse(ReadText(user / "look.json"));
    INFO(doc.dump());
    CHECK(doc.at("tint") == "#FF8800FF");
    REQUIRE(doc.at("offset").is_array());
    CHECK(doc.at("offset").size() == 2);
    CHECK(doc.at("offset").at(0).get<float>() == 1.5f);
    CHECK(doc.at("offset").at(1).get<float>() == -2.0f);
    CHECK(doc.at("origin").size() == 3);
    CHECK(doc.at("rect").size() == 4);
    CHECK(doc.at("mode") == "High");

    CVarRegistry fresh;
    Roster(fresh);
    CHECK(ApplyCVarDirectory(fresh, user, SetBy::User, "user").unknownKeys.empty());
    fresh.Publish();
    CHECK(fresh.Get(fresh.Find("look.tint"))->AsColor() == orange);
    CHECK(fresh.Get(fresh.Find("look.offset"))->AsVec2() == CVarVec2{ 1.5f, -2.0f });
    CHECK(fresh.Get(fresh.Find("look.origin"))->AsVec3() == CVarVec3{ 1.0f, 2.0f, 3.0f });
    CHECK(fresh.Get(fresh.Find("look.rect"))->AsVec4() == CVarVec4{ 0.0f, 0.25f, 0.5f, 1.0f });
    CHECK(fresh.Get(fresh.Find("look.mode"))->AsEnum() == 2);
    std::filesystem::remove_all(user);
}

TEST_CASE("a Color reads from [r,g,b(,a)] linear, and a wrong shape is reported, not applied", "[cvar]")
{
    CVarRegistry reg;
    Roster(reg);
    const nlohmann::json arrays = { { "tint", { 0.5, 0.25, 1.0 } } };
    CHECK(ApplyCVarCategory(reg, "look", arrays, SetBy::Project, false, "project").unknownKeys.empty());
    reg.Publish();
    CHECK(reg.Get(reg.Find("look.tint"))->AsColor() == CVarColor{ 0.5f, 0.25f, 1.0f, 1.0f });

    const nlohmann::json bad = { { "offset", { 1.0, 2.0, 3.0 } }, { "origin", "1 2 3" }, { "mode", "Medium" }, { "tint", "#12" } };
    const CVarApplyReport report = ApplyCVarCategory(reg, "look", bad, SetBy::Project, false, "project");
    CHECK(report.typeMismatches.size() == 4);   // declared, wrong shape: a mismatch, not an unknown key (spec s12)
    CHECK(report.unknownKeys.empty());
    reg.Publish();
    CHECK(reg.Get(reg.Find("look.offset"))->AsVec2() == CVarVec2{});      // untouched
    CHECK(reg.Get(reg.Find("look.mode"))->AsEnum() == 0);
}

TEST_CASE("a numeric Enum in a file applies with one warning and is saved back as its name", "[cvar]")
{
    const auto user = std::filesystem::temp_directory_path() / "arcane-cvar-enum-number";
    std::filesystem::remove_all(user);
    WriteText(user / "look.json", R"({"mode": 2})");

    CVarRegistry reg;
    Roster(reg);
    CVarApplyReport report;
    {
        LogCapture log;
        report = ApplyCVarDirectory(reg, user, SetBy::User, "user");
        CHECK(log.count == 1);
        CHECK(log.last.find("look.mode") != std::string::npos);
        CHECK(log.last.find("High") != std::string::npos);
    }
    CHECK(report.unknownKeys.empty());
    reg.Publish();
    CHECK(reg.Get(reg.Find("look.mode"))->AsEnum() == 2);
    WriteCVarArchive(reg, user);
    CHECK(nlohmann::json::parse(ReadText(user / "look.json")).at("mode") == "High");

    WriteText(user / "look.json", R"({"mode": 9})");           // outside the names: refused
    CVarRegistry other;
    Roster(other);
    const CVarApplyReport outside = ApplyCVarDirectory(other, user, SetBy::User, "user");
    CHECK(outside.typeMismatches == std::vector<std::string>{ "look.mode" });
    CHECK(outside.unknownKeys.empty());
    std::filesystem::remove_all(user);
}

TEST_CASE("a Color saved at its default and reloaded still counts as the default: compare within one 8-bit sRGB step", "[cvar]")
{
    // A linear default does not survive the file's 8-bit sRGB hex bit-exactly
    // (the mesh preview light here), so "is default" -- the Modified filter and
    // the reset arrow -- must not compare with == (settings plan Review Focus 3).
    const CVarColor light{ 0.45f, 0.7f, 0.8f, 1.0f };
    const CVarColor reloaded = *CVarColorFromHex(CVarColorToHex(light));
    CHECK_FALSE(reloaded == light);
    CHECK(CVarColorNearlyEqual(reloaded, light));

    // Through the files: a User value picked at the default, archived, read back.
    const auto user = std::filesystem::temp_directory_path() / "arcane-cvar-color-default";
    std::filesystem::remove_all(user);
    const auto declare = [&](CVarRegistry& reg) {
        const CVarDesc d{ .name = "look.light", .type = CVarType::Color, .defaultValue = CVarValue::Color(light),
                          .flags = CVarFlags::Archive, .help = "file probe", .module = "test" };
        REQUIRE_FALSE(reg.Register(d).IsStale());
    };
    CVarRegistry reg;
    declare(reg);
    REQUIRE(reg.Set(reg.Find("look.light"), CVarValue::Color(light), SetBy::User, "test") == SetResult::Applied);
    reg.Publish();
    WriteCVarArchive(reg, user);

    CVarRegistry fresh;
    declare(fresh);
    CHECK(ApplyCVarDirectory(fresh, user, SetBy::User, "user").unknownKeys.empty());
    fresh.Publish();
    const CVarHandle handle = fresh.Find("look.light");
    const CVarColor back = fresh.Get(handle)->AsColor();
    const CVarColor def = fresh.Metadata(handle)->defaultValue.AsColor();
    CHECK(CVarColorToHex(back) == CVarColorToHex(def));
    CHECK(CVarColorNearlyEqual(back, def));
    std::filesystem::remove_all(user);

    // One step apart is the same colour; two are not; alpha counts.
    CHECK(CVarColorNearlyEqual(*CVarColorFromHex("#80808080"), *CVarColorFromHex("#81818181")));
    CHECK(CVarColorNearlyEqual(*CVarColorFromHex("#81818181"), *CVarColorFromHex("#80808080")));
    CHECK_FALSE(CVarColorNearlyEqual(*CVarColorFromHex("#808080FF"), *CVarColorFromHex("#828080FF")));
    CHECK_FALSE(CVarColorNearlyEqual(*CVarColorFromHex("#808080FF"), *CVarColorFromHex("#808082FF")));
    CHECK_FALSE(CVarColorNearlyEqual(*CVarColorFromHex("#808080FF"), *CVarColorFromHex("#808080FD")));
}
