#include <catch2/catch_test_macros.hpp>

#include <Arcane/Input/InputActionAsset.hpp>
#include <Arcane/Input/InputBindingProfile.hpp>

#include <filesystem>
#include <fstream>

namespace
{
    Arcane::InputActionAsset ProfileAsset()
    {
        return *Arcane::InputActionAsset::FromJson(nlohmann::json::parse(R"JSON({
            "version":1,"id":"11111111-1111-4111-8111-111111111111",
            "defaultMap":"22222222-2222-4222-8222-222222222222","controlSchemes":[],
            "actionMaps":[{"id":"22222222-2222-4222-8222-222222222222","name":"Player","actions":[
              {"id":"33333333-3333-4333-8333-333333333333","name":"Jump","type":"Button","bindings":[
                {"id":"44444444-4444-4444-8444-444444444444","path":"<Keyboard>/space"}]},
              {"id":"55555555-5555-4555-8555-555555555555","name":"Move","type":"Axis1D","bindings":[
                {"id":"66666666-6666-4666-8666-666666666666","composite":"1DAxis","parts":[
                  {"id":"77777777-7777-4777-8777-777777777777","name":"positive","path":"<Keyboard>/scancode/d"},
                  {"id":"88888888-8888-4888-8888-888888888888","name":"negative","path":"<Keyboard>/scancode/a"}]}]}
            ]}]
        })JSON"));
    }

    Arcane::Guid Id(const char* text) { return *Arcane::Guid::FromString(text); }
    const auto kJump = Id("44444444-4444-4444-8444-444444444444");
    const auto kComposite = Id("66666666-6666-4666-8666-666666666666");
    const auto kPositive = Id("77777777-7777-4777-8777-777777777777");

    std::filesystem::path TempProfile()
    {
        return std::filesystem::temp_directory_path() / ("arcane-profile-" + Arcane::Guid::Generate().ToString() + ".json");
    }
}

TEST_CASE("input profile: save reload reset and preserve asset", "[input][profile]")
{
    const auto asset = ProfileAsset();
    const auto authored = asset.ToJson();
    Arcane::InputBindingProfile profile;
    REQUIRE(profile.SetOverride(kJump, "<Keyboard>/scancode/w", asset));
    REQUIRE(profile.SetOverride(kPositive, "<Keyboard>/scancode/s", asset));
    CHECK(profile.Dirty());
    const auto path = TempProfile();
    REQUIRE(profile.Save(path));
    CHECK_FALSE(profile.Dirty());

    Arcane::InputBindingProfile loaded;
    CHECK(loaded.Load(path, asset).status == Arcane::ProfileLoadStatus::Loaded);
    CHECK(loaded.Overrides().at(kJump) == "<Keyboard>/scancode/w");
    CHECK(loaded.Overrides().at(kPositive) == "<Keyboard>/scancode/s");
    CHECK(loaded.RemoveOverride(kJump));
    CHECK_FALSE(loaded.Overrides().contains(kJump));
    loaded.Reset();
    CHECK(loaded.Overrides().empty());
    CHECK(asset.ToJson() == authored);
    std::filesystem::remove(path);
}

TEST_CASE("input profile: stale entries are ignored and malformed load is transactional", "[input][profile]")
{
    const auto asset = ProfileAsset();
    const auto path = TempProfile();
    Arcane::InputBindingProfile profile;
    REQUIRE(profile.SetOverride(kJump, "<Keyboard>/scancode/w", asset));
    {
        std::ofstream out(path);
        out << R"JSON({"version":1,"overrides":{"44444444-4444-4444-8444-444444444444":"<Keyboard>/scancode/s","99999999-9999-4999-8999-999999999999":"<Keyboard>/space"}})JSON";
    }
    auto result = profile.Load(path, asset);
    CHECK(result.status == Arcane::ProfileLoadStatus::Loaded);
    CHECK_FALSE(result.diagnostics.empty());
    CHECK(profile.Overrides().at(kJump) == "<Keyboard>/scancode/s");
    {
        std::ofstream out(path);
        out << "{ broken json";
    }
    CHECK(profile.Load(path, asset).status == Arcane::ProfileLoadStatus::Invalid);
    CHECK(profile.Overrides().at(kJump) == "<Keyboard>/scancode/s");
    std::filesystem::remove(path);
}

TEST_CASE("input profile: validates binding paths and atomic write failure", "[input][profile]")
{
    const auto asset = ProfileAsset();
    Arcane::InputBindingProfile profile;
    CHECK_FALSE(profile.SetOverride(kComposite, "<Keyboard>/space", asset));
    CHECK_FALSE(profile.SetOverride(kJump, "<Wheel>/up", asset));
    CHECK_FALSE(profile.SetOverride(Arcane::Guid::Nil(), "<Keyboard>/space", asset));
    REQUIRE(profile.SetOverride(kPositive, "<Keyboard>/scancode/w", asset));
    const auto path = TempProfile();
    REQUIRE(profile.Save(path));
    const auto before = [&] { std::ifstream in(path); return std::string(std::istreambuf_iterator<char>(in), {}); }();
    REQUIRE(profile.SetOverride(kJump, "<Keyboard>/scancode/s", asset));
    CHECK_FALSE(profile.Save(path / "not-a-file"));
    const auto after = [&] { std::ifstream in(path); return std::string(std::istreambuf_iterator<char>(in), {}); }();
    CHECK(before == after);
    CHECK(profile.Dirty());
    std::filesystem::remove(path);
}

TEST_CASE("input profile: exports and imports a profile", "[input][profile]")
{
    const auto asset = ProfileAsset();
    const auto path = TempProfile();
    Arcane::InputBindingProfile source;
    REQUIRE(source.SetOverride(kJump, "<Keyboard>/scancode/w", asset));
    REQUIRE(source.Export(path));
    CHECK(source.Dirty());
    Arcane::InputBindingProfile imported;
    CHECK(imported.Import(path, asset).status == Arcane::ProfileLoadStatus::Loaded);
    CHECK(imported.Overrides().at(kJump) == "<Keyboard>/scancode/w");
    CHECK(imported.Dirty());
    std::filesystem::remove(path);
}

TEST_CASE("input profile: a game-side rebind to Keypad + is accepted", "[input][profile]")
{
    const auto asset = ProfileAsset();
    Arcane::InputBindingProfile profile;
    CHECK(profile.SetOverride(kJump, "<Keyboard>/scancode/keypad +", asset));
}
