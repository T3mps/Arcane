// Arcane::GameInput / Arcane::ActionRef (input-seam spec 2026-10-02 s4): the
// read-only gameplay-input view a client world publishes as a resource, and
// the by-name action handle that re-resolves across a reconfigure.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Client/ClientRuntime.hpp>
#include <Arcane/Input/GameInput.hpp>
#include <Arcane/Input/InputActionAsset.hpp>
#include <Arcane/Input/InputSnapshot.hpp>
#include <Arcane/Input/LocalInputUser.hpp>

#include <Json.hpp>

#include <spdlog/sinks/callback_sink.h>

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>

#include "Helpers/TestTypeContext.hpp"

namespace
{
    constexpr std::uint32_t kScancodeW = 26;   // SDL_SCANCODE_W

    // Player.Jump bound to W; `jumpName` lets a test rename the action.
    Arcane::InputActionAsset JumpAsset(const char* jumpName = "Jump")
    {
        const nlohmann::json j = nlohmann::json::parse(std::string(R"({
          "version": 1, "id": "77777777-7777-4777-8777-777777777777",
          "defaultMap": "77777777-7777-4777-8777-000000000001", "controlSchemes": [],
          "actionMaps": [ { "id": "77777777-7777-4777-8777-000000000001", "name": "Player",
            "actions": [ { "id": "77777777-7777-4777-8777-000000000002", "name": ")") + jumpName + R"(",
              "type": "Button",
              "bindings": [ { "id": "77777777-7777-4777-8777-000000000003", "path": "<Keyboard>/scancode/w" } ] } ] } ] })");
        auto asset = Arcane::InputActionAsset::FromJson(j);
        REQUIRE(asset);
        return *asset;
    }
    const Arcane::Guid kProject = *Arcane::Guid::FromString("88888888-8888-4888-8888-888888888888");

    // Counts the engine logger's "action ... reads as zero" warnings for
    // Player.Jump while in scope (detaches even when a REQUIRE unwinds).
    struct MissingJumpWarnings
    {
        int count = 0;
        std::shared_ptr<spdlog::sinks::callback_sink_mt> sink = std::make_shared<spdlog::sinks::callback_sink_mt>(
            [this](const spdlog::details::log_msg& m)
            {
                const std::string_view text(m.payload.data(), m.payload.size());
                if (text.find("'Player.Jump'") != std::string_view::npos &&
                    text.find("reads as zero") != std::string_view::npos)
                    ++count;
            });
        MissingJumpWarnings()  { Arcane::Log::Engine()->sinks().push_back(sink); }
        ~MissingJumpWarnings()
        {
            auto& sinks = Arcane::Log::Engine()->sinks();
            sinks.erase(std::remove(sinks.begin(), sinks.end(), sink), sinks.end());
        }
        MissingJumpWarnings(const MissingJumpWarnings&) = delete;
        MissingJumpWarnings& operator=(const MissingJumpWarnings&) = delete;
    };
}

TEST_CASE("GameInput with no user answers zero and false", "[client][input][gameinput]")
{
    const Arcane::GameInput none;
    Arcane::ActionRef jump{"Player", "Jump"};
    CHECK_FALSE(none.HasUser());
    CHECK(none.Generation() == 0);
    CHECK_FALSE(none.Resolve(jump).has_value());
    CHECK_FALSE(none.Down(jump));
    CHECK_FALSE(none.PressedThisFixedStep(jump));
    CHECK(none.Value(jump).scalar == 0.0f);
}

TEST_CASE("GameInput reads the user's live state through an ActionRef", "[client][input][gameinput]")
{
    Arcane::LocalInputUser user;
    REQUIRE(user.Configure(JumpAsset(), kProject));
    const Arcane::GameInput in{&user};
    Arcane::ActionRef jump{"Player", "Jump"};

    Arcane::InputSnapshot held;
    held.SetScancode(kScancodeW);
    user.Update(1.0 / 60.0, held);
    CHECK(in.Down(jump));
    CHECK(in.Pressed(jump));
    user.BeginFixedStep();
    CHECK(in.PressedThisFixedStep(jump));
    user.BeginFixedStep();                     // second fixed step of the same frame
    CHECK_FALSE(in.PressedThisFixedStep(jump));   // the edge belongs to the first only
    CHECK(in.Down(jump));
}

// Review Focus #1: a cached ActionRef across a re-Configure (hot edit, rename, project switch).
TEST_CASE("ActionRef re-resolves when the input asset is reconfigured, answers zero while its action is gone",
          "[client][input][gameinput]")
{
    Arcane::LocalInputUser user;
    REQUIRE(user.Configure(JumpAsset("Jump"), kProject));
    const Arcane::GameInput in{&user};
    Arcane::ActionRef jump{"Player", "Jump"};
    MissingJumpWarnings warnings;
    const auto first = in.Resolve(jump);
    REQUIRE(first);
    CHECK(warnings.count == 0);

    REQUIRE(user.Configure(JumpAsset("Leap"), kProject));      // same project, action renamed
    CHECK_FALSE(in.Resolve(jump).has_value());
    Arcane::InputSnapshot held;
    held.SetScancode(kScancodeW);
    user.Update(1.0 / 60.0, held);
    CHECK_FALSE(in.Down(jump));                                  // zero, not the stale id
    CHECK(warnings.count == 1);                                  // once per generation, not per query

    REQUIRE(user.Configure(JumpAsset("Jump"), kProject));      // renamed back
    const auto again = in.Resolve(jump);
    REQUIRE(again);
    CHECK(*again == *first);
    CHECK(warnings.count == 1);

    user.Clear();                                                // project closed
    CHECK_FALSE(in.Resolve(jump).has_value());
    CHECK(warnings.count == 2);                                  // a new generation warns again
}

TEST_CASE("ClientRuntime publishes GameInput on configure, every frame and every fixed step", "[client][input][gameinput]")
{
    Arcane::ClientRuntime runtime(Arcane::Test::Process());
    CHECK(runtime.Registry().GetResource<Arcane::GameInput>() == nullptr);

    runtime.UpdateGameInput(1.0 / 60.0, {});                     // unconfigured still publishes
    const auto* unconfigured = runtime.Registry().GetResource<Arcane::GameInput>();
    REQUIRE(unconfigured);
    CHECK(unconfigured->HasUser());
    CHECK_FALSE(unconfigured->Down(Arcane::ActionRef{"Player", "Jump"}));

    runtime.ResetRegistry();                                     // a swap drops resources...
    CHECK(runtime.Registry().GetResource<Arcane::GameInput>() == nullptr);
    runtime.BeginGameInputFixedStep();                           // ...the next pass republishes
    CHECK(runtime.Registry().GetResource<Arcane::GameInput>() != nullptr);

    runtime.ResetRegistry();
    REQUIRE(runtime.ConfigureGameInput(JumpAsset(), kProject));
    const auto* configured = runtime.Registry().GetResource<Arcane::GameInput>();
    REQUIRE(configured);
    CHECK(configured->Resolve(Arcane::ActionRef{"Player", "Jump"}).has_value());
}
