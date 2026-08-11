// Plugin load diagnoses WHICH of the three failure causes occurred. Before this,
// LoadLibrary failure, a missing export, and an ABI mismatch all collapsed into
// one bool and one generic log line. ([plugin])
//
// The unit-level cases below exercise Plugin::Load/Module::Load directly (the
// resolve-cause plumbing). The integration cases at the bottom drive the actual
// deliverable -- PluginHost's load sites publishing/clearing through the engine
// Diagnostics seam -- via a raw capture sink, same pattern as
// DiagnosticSeamTest.cpp's Capture/CaptureSink.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/Diagnostics.hpp>
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Plugin/Module.hpp>
#include <Arcane/Plugin/Plugin.hpp>
#include <Arcane/Plugin/PluginABI.hpp>
#include <Arcane/Plugin/PluginHost.hpp>

#include "Helpers/TestTypeContext.hpp"
#include "../plugins/HotReloadShared.hpp"   // the SAME Pulse type the plugin uses

#include <Astra/Registry/Registry.hpp>

using Arcane::HotReloadTest::Pulse;

namespace
{
    // Records every (key, diagnostic-set) call the engine seam forwards, in
    // order -- lets a test assert the exact key PluginHost publishes under
    // ("plugin:<name>") as well as the content, which DiagnosticStore's
    // Snapshot()/Filtered() cannot: they flatten across keys and discard the
    // key string entirely.
    struct Capture
    {
        std::vector<std::pair<std::string, std::vector<Arcane::Diagnostic>>> calls;
    };

    void CaptureSink(std::string_view key, std::span<const Arcane::Diagnostic> diags, void* user)
    {
        auto* c = static_cast<Capture*>(user);
        c->calls.emplace_back(std::string(key),
                              std::vector<Arcane::Diagnostic>(diags.begin(), diags.end()));
    }
}

TEST_CASE("A DLL that cannot be loaded reports the OS error", "[plugin][diagnostics]")
{
    const std::optional<Arcane::Module> m =
        Arcane::Module::Load("this-path-does-not-exist-arcane.dll");
    CHECK_FALSE(m.has_value());
    CHECK_FALSE(Arcane::Module::LastLoadError().empty());   // the OS reason, captured
}

TEST_CASE("An ABI-mismatched plugin reports BOTH version numbers", "[plugin][diagnostics]")
{
    // HotReloadPluginBad exports ABI = kGamePluginABIVersion + 999 on purpose
    // (ArcaneTests/plugins/HotReloadPlugin.cpp:4,48), so this is unambiguously the
    // AbiMismatch cause -- not a missing export and not a load failure.
    Arcane::PluginResolveError error;
    auto plugin = Arcane::Plugin::Load(std::filesystem::path("HotReloadPluginBad.dll"), &error);

    CHECK_FALSE(plugin.has_value());
    CHECK(error.kind == Arcane::PluginResolveError::Kind::AbiMismatch);
    CHECK(error.pluginAbi == Arcane::kGamePluginABIVersion + 999);
    CHECK(error.engineAbi == Arcane::kGamePluginABIVersion);
}

TEST_CASE("A missing required export is named", "[plugin][diagnostics]")
{
    // ArcaneClient.dll itself loads fine as a module but exports none of the
    // plugin entry points -- the cheapest real MissingExport case in-tree.
    Arcane::PluginResolveError error;
    auto plugin = Arcane::Plugin::Load(std::filesystem::path("ArcaneClient.dll"), &error);

    CHECK_FALSE(plugin.has_value());
    CHECK(error.kind == Arcane::PluginResolveError::Kind::MissingExport);
    CHECK(error.symbol == std::string(Arcane::PluginEntry::kABIVersion));   // the first checked
}

TEST_CASE("PluginHost::Load publishes the ABI-mismatch diagnostic under plugin:<name>",
          "[plugin][diagnostics][hotreload]")
{
    // The actual deliverable: PluginHost's primary-load-failure site, not just
    // the resolve plumbing the cases above cover in isolation.
    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());

    Capture cap;
    Arcane::Diagnostics::SetSink(&CaptureSink, &cap);

    Arcane::PluginHost host(rt, std::filesystem::path("HotReloadPluginBad.dll"));
    CHECK_FALSE(host.Load());

    REQUIRE(cap.calls.size() == 1);
    CHECK(cap.calls[0].first == "plugin:HotReloadPluginBad");   // stem of the SOURCE path, not the temp copy
    REQUIRE(cap.calls[0].second.size() == 1);

    const Arcane::Diagnostic& d = cap.calls[0].second[0];
    CHECK(d.severity == Arcane::DiagSeverity::Error);
    CHECK(d.scope == Arcane::DiagScope::Plugin);
    CHECK(d.code == "plugin.abi.mismatch");
    CHECK(d.locator.kind == Arcane::DiagLocator::Kind::File);
    CHECK(d.locator.file == "HotReloadPluginBad.dll");
    // BOTH ABI numbers must be in the message, not just latched in the struct.
    CHECK(d.message.find(std::to_string(Arcane::kGamePluginABIVersion + 999)) != std::string::npos);
    CHECK(d.message.find(std::to_string(Arcane::kGamePluginABIVersion)) != std::string::npos);

    Arcane::Diagnostics::SetSink(nullptr, nullptr);
    host.Unload();
}

TEST_CASE("A failed reload publishes the cause; the next successful reload retracts it",
          "[plugin][diagnostics][hotreload]")
{
    // Guard against ordering contamination: ensure we start with genuine V1 content
    // (same idiom as PluginHostTest.cpp's "ABI mismatch rolls back to last-good").
    std::filesystem::copy_file("../HotReloadPluginV1/HotReloadPluginV1.dll", "HotReloadPluginV1.dll",
                               std::filesystem::copy_options::overwrite_existing);

    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
    rt.Components()->RegisterComponent<Pulse>();   // engine sees the type, mirrors PluginHostTest.cpp

    Arcane::PluginHost host(rt, std::filesystem::path("HotReloadPluginV1.dll"));
    REQUIRE(host.Load());   // good load first -- its Diagnostics::Clear happens BEFORE the sink below

    Capture cap;
    Arcane::Diagnostics::SetSink(&CaptureSink, &cap);

    // Swap in the ABI-mismatched image and force a reload: fails, rolls back to
    // last-good, and (this task's deliverable) publishes the real cause under
    // plugin:HotReloadPluginV1 even though the session survives on the old image.
    std::filesystem::copy_file("HotReloadPluginBad.dll", "HotReloadPluginV1.dll",
                               std::filesystem::copy_options::overwrite_existing);
    CHECK_FALSE(host.ForceReload());
    CHECK(host.IsLoaded());   // still on last-good

    REQUIRE(cap.calls.size() == 1);
    CHECK(cap.calls[0].first == "plugin:HotReloadPluginV1");
    REQUIRE(cap.calls[0].second.size() == 1);
    CHECK(cap.calls[0].second[0].code == "plugin.abi.mismatch");

    // Swap the good image back and reload again: succeeds, and retracts the row
    // via Diagnostics::Clear -- a Publish with an empty set for the SAME key.
    std::filesystem::copy_file("../HotReloadPluginV1/HotReloadPluginV1.dll", "HotReloadPluginV1.dll",
                               std::filesystem::copy_options::overwrite_existing);
    CHECK(host.ForceReload());

    REQUIRE(cap.calls.size() == 2);
    CHECK(cap.calls[1].first == "plugin:HotReloadPluginV1");
    CHECK(cap.calls[1].second.empty());

    Arcane::Diagnostics::SetSink(nullptr, nullptr);
    host.Unload();

    // restore the fixture for re-runs
    std::filesystem::copy_file("../HotReloadPluginV1/HotReloadPluginV1.dll", "HotReloadPluginV1.dll",
                               std::filesystem::copy_options::overwrite_existing);
}
