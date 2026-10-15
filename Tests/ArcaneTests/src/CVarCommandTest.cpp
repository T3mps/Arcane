// Commands say whether they succeeded (settings spec s4.5, O4): CommandResult,
// not "does the text start with unknown". [cvar]

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Config/CVarDecl.hpp>
#include <Arcane/Config/CVarRegistry.hpp>

#include <string>

using namespace Arcane;

namespace
{
    CommandResult Refuse(std::string_view args, void*) { return { false, "nope: " + std::string(args) }; }
    CommandResult SaysUnknown(std::string_view, void*) { return { true, "unknown is just a word here" }; }
    void Legacy(std::string_view, std::string& out, void*) { out = "unknown-looking legacy reply"; }
    int g_macroRuns = 0;
    CommandResult MacroCommand(std::string_view, void*) { ++g_macroRuns; return { true, "macro ok" }; }
}

ARC_COMMAND("tests.macroCommand", ::Arcane::CVarFlags::None, "ARC_COMMAND probe (CVarCommandTest).", &MacroCommand);

TEST_CASE("CommandResult: success is what the command says, not what its text looks like", "[cvar]")
{
    CVarRegistry reg;
    REQUIRE(reg.RegisterCommand("t.refuse", CVarFlags::None, "Refuses.", "test", &Refuse, nullptr));
    REQUIRE(reg.RegisterCommand("t.unknownWord", CVarFlags::None, "Says unknown.", "test", &SaysUnknown, nullptr));
    REQUIRE(reg.RegisterCommand("t.legacy", CVarFlags::None, "The old text form.", "test", &Legacy, nullptr));

    const ExecResult refused = reg.Execute("t.refuse x", CVarContext::Editor);
    CHECK_FALSE(refused.ok);
    CHECK(refused.text == "nope: x");
    CHECK(reg.Execute("t.unknownWord", CVarContext::Editor).ok);         // v1 inferred failure from the text
    const ExecResult legacy = reg.Execute("t.legacy", CVarContext::Editor);
    CHECK(legacy.ok);                                                     // wrapped and treated as ok
    CHECK(legacy.text == "unknown-looking legacy reply");
    CHECK_FALSE(reg.Execute("cvar_explain no.such.cvar", CVarContext::Editor).ok);
    CHECK(reg.Execute("cvarlist", CVarContext::Editor).ok);

    const ExecResult macro = CVarRegistry::Get().Execute("tests.macroCommand", CVarContext::Editor);
    CHECK(macro.ok);
    CHECK(macro.text == "macro ok");
    CHECK(g_macroRuns == 1);
}
