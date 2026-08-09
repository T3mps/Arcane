// Editor diagnostic store: key -> current set, with publication-group replace
// semantics. Pure data, no ImGui ([diagnostics]).

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <DiagnosticStore.hpp>

namespace
{
    Arcane::Diagnostic Diag(Arcane::DiagSeverity sev, Arcane::DiagScope scope,
                            std::string code, std::string message)
    {
        Arcane::Diagnostic d;
        d.severity = sev;
        d.scope    = scope;
        d.code     = std::move(code);
        d.message  = std::move(message);
        return d;
    }
}

TEST_CASE("Publishing a key REPLACES that key's set rather than appending", "[diagnostics]")
{
    Arcane::Editor::DiagnosticStore store;

    const Arcane::Diagnostic first[] = {
        Diag(Arcane::DiagSeverity::Error, Arcane::DiagScope::Material, "a", "one"),
        Diag(Arcane::DiagSeverity::Error, Arcane::DiagScope::Material, "b", "two"),
    };
    store.Publish("material:x", first);
    CHECK(store.Count(Arcane::DiagSeverity::Error) == 2);

    const Arcane::Diagnostic second[] = {
        Diag(Arcane::DiagSeverity::Error, Arcane::DiagScope::Material, "c", "three"),
    };
    store.Publish("material:x", second);

    CHECK(store.Count(Arcane::DiagSeverity::Error) == 1);
    const std::vector<Arcane::Diagnostic> all = store.Snapshot();
    REQUIRE(all.size() == 1);
    CHECK(all[0].code == "c");
}

TEST_CASE("An empty republish clears the key and leaves other keys untouched", "[diagnostics]")
{
    Arcane::Editor::DiagnosticStore store;

    const Arcane::Diagnostic a[] = { Diag(Arcane::DiagSeverity::Error, Arcane::DiagScope::Material, "a", "one") };
    const Arcane::Diagnostic b[] = { Diag(Arcane::DiagSeverity::Warning, Arcane::DiagScope::Scene, "b", "two") };
    store.Publish("material:x", a);
    store.Publish("scene:y", b);
    REQUIRE(store.Snapshot().size() == 2);

    store.Publish("material:x", {});

    const std::vector<Arcane::Diagnostic> all = store.Snapshot();
    REQUIRE(all.size() == 1);
    CHECK(all[0].code == "b");
    CHECK(store.Count(Arcane::DiagSeverity::Error) == 0);
    CHECK(store.Count(Arcane::DiagSeverity::Warning) == 1);
}

TEST_CASE("Clear(key) is equivalent to an empty republish", "[diagnostics]")
{
    Arcane::Editor::DiagnosticStore store;
    const Arcane::Diagnostic a[] = { Diag(Arcane::DiagSeverity::Error, Arcane::DiagScope::Material, "a", "one") };
    store.Publish("material:x", a);
    store.Clear("material:x");
    CHECK(store.Snapshot().empty());
}

TEST_CASE("Snapshot sorts errors before warnings before info", "[diagnostics]")
{
    Arcane::Editor::DiagnosticStore store;
    const Arcane::Diagnostic mixed[] = {
        Diag(Arcane::DiagSeverity::Info,    Arcane::DiagScope::Scene, "i", "info"),
        Diag(Arcane::DiagSeverity::Warning, Arcane::DiagScope::Scene, "w", "warn"),
        Diag(Arcane::DiagSeverity::Error,   Arcane::DiagScope::Scene, "e", "err"),
    };
    store.Publish("scene:y", mixed);

    const std::vector<Arcane::Diagnostic> all = store.Snapshot();
    REQUIRE(all.size() == 3);
    CHECK(all[0].severity == Arcane::DiagSeverity::Error);
    CHECK(all[1].severity == Arcane::DiagSeverity::Warning);
    CHECK(all[2].severity == Arcane::DiagSeverity::Info);
}

TEST_CASE("MatchesDiagnosticFilter gates on severity floor and case-insensitive text", "[diagnostics]")
{
    const Arcane::Diagnostic warn =
        Diag(Arcane::DiagSeverity::Warning, Arcane::DiagScope::Assets, "assets.dup", "Duplicate id kept");

    CHECK(Arcane::Editor::MatchesDiagnosticFilter(warn, Arcane::DiagSeverity::Info, ""));
    CHECK(Arcane::Editor::MatchesDiagnosticFilter(warn, Arcane::DiagSeverity::Warning, ""));
    CHECK_FALSE(Arcane::Editor::MatchesDiagnosticFilter(warn, Arcane::DiagSeverity::Error, ""));

    CHECK(Arcane::Editor::MatchesDiagnosticFilter(warn, Arcane::DiagSeverity::Info, "duplicate"));
    CHECK(Arcane::Editor::MatchesDiagnosticFilter(warn, Arcane::DiagSeverity::Info, "DUPLICATE"));
    CHECK(Arcane::Editor::MatchesDiagnosticFilter(warn, Arcane::DiagSeverity::Info, "assets.dup"));
    CHECK_FALSE(Arcane::Editor::MatchesDiagnosticFilter(warn, Arcane::DiagSeverity::Info, "nonsense"));
}

TEST_CASE("Filtered applies both the severity floor and the search text", "[diagnostics]")
{
    Arcane::Editor::DiagnosticStore store;
    const Arcane::Diagnostic set[] = {
        Diag(Arcane::DiagSeverity::Error,   Arcane::DiagScope::Material, "m.err",  "broken shader"),
        Diag(Arcane::DiagSeverity::Warning, Arcane::DiagScope::Material, "m.warn", "unused param"),
    };
    store.Publish("material:x", set);

    CHECK(store.Filtered(Arcane::DiagSeverity::Error, "").size() == 1);
    CHECK(store.Filtered(Arcane::DiagSeverity::Info, "param").size() == 1);
    CHECK(store.Filtered(Arcane::DiagSeverity::Info, "").size() == 2);
}

TEST_CASE("The store receives diagnostics published through the engine seam", "[diagnostics]")
{
    Arcane::Editor::DiagnosticStore store;
    store.InstallAsEngineSink();

    const Arcane::Diagnostic d[] = {
        Diag(Arcane::DiagSeverity::Error, Arcane::DiagScope::Plugin, "plugin.abi.mismatch", "ABI 7 vs 8"),
    };
    Arcane::Diagnostics::Publish("plugin:Game", d);

    const std::vector<Arcane::Diagnostic> all = store.Snapshot();
    REQUIRE(all.size() == 1);
    CHECK(all[0].code == "plugin.abi.mismatch");

    store.UninstallEngineSink();
}
