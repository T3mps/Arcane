#include <catch2/catch_test_macros.hpp>
#include <Arcane/Remote/Commands/ProblemCommands.hpp>
#include <Arcane/Base/DiagnosticStore.hpp>
#include <Arcane/Base/Diagnostics.hpp>
#include "RemoteTestSupport.hpp"

#if !defined(ARC_BUILD_DIST)

TEST_CASE("problems --search uses Filtered and a dead entity handle warns", "[remote]") {
    Arcane::DiagnosticStore store;
    Arcane::Diagnostic row;
    row.severity = Arcane::DiagSeverity::Error;
    row.scope = Arcane::DiagScope::Project;
    row.code = "remote.probe";
    row.message = "needle in the row";
    row.locator = Arcane::DiagLocator::Entity(999);
    const Arcane::Diagnostic rows[] = {row};
    store.Publish("remote-probe", rows);
    RemoteTest::ScopedRemotePointer<Arcane::DiagnosticStore> storeGuard(&Arcane::Remote::SetProblemStore, &store);
    Arcane::Remote::RegisterProblemCommand();
    Arcane::Remote::Call call;
    call.command = "problems.list";
    call.args = nlohmann::json{{"severity", "error"}, {"search", "needle"}};
    const auto hit = Arcane::Remote::CommandRegistry::Get().Invoke(call).envelope;
    CHECK(hit.ok);
    REQUIRE(hit.data["problems"].size() == 1);
    CHECK(hit.data["problems"][0]["locator"]["guid"].is_null());
    CHECK(hit.data["problems"][0]["locator"]["handle"] == 999);
    CHECK(hit.warnings[0].code == "entity.no-identity");
    call.args["search"] = "absent";
    const auto miss = Arcane::Remote::CommandRegistry::Get().Invoke(call).envelope;
    CHECK(miss.data["problems"].empty());
    call.args["follow"] = true;
    const auto follow = Arcane::Remote::CommandRegistry::Get().Invoke(call);
    CHECK(follow.completesJob == 0);
    CHECK(follow.envelope.errors[0].code == "arg.inapplicable");
    CHECK(Arcane::Remote::ExitFromHost(follow.envelope) == 2);
}

#endif
