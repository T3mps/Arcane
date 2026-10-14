#include <catch2/catch_test_macros.hpp>
#include <Arcane/Arc/Registry.hpp>
#include "ArcDemoArgs.hpp"
#include "ArcTestSupport.hpp"
#include <algorithm>
#include <string>
#include <vector>

#if !defined(ARC_BUILD_DIST)

static Arcane::Arc::CommandResult DemoFn(const Arcane::Arc::Call& call, void*) {
    Arcane::Arc::CommandResult r;
    r.envelope = Arcane::Arc::MakeOk(call.command, call.args, Arcane::Arc::HostBlock{});
    return r;
}

static Arcane::Arc::Desc Base() {
    Arcane::Arc::Desc d;
    d.name = "demo.ok";
    d.summary = "does a thing";
    d.argsMeta = Astra::GetMeta<ArcDemoArgs>();
    d.fn = &DemoFn;
    return d;
}

TEST_CASE("registry rejects an empty summary, a bad name, and Drive with mutates None", "[arc][registry]") {
    auto& reg = Arcane::Arc::CommandRegistry::Get();
    std::string err;
    Arcane::Arc::Desc bad = Base();
    bad.summary = "";
    CHECK_FALSE(reg.Register(bad, &err));
    bad.summary = "does a thing";
    bad.name = "Demo.Ok";
    CHECK_FALSE(reg.Register(bad, &err));
    bad.name = "demo..ok";
    CHECK_FALSE(reg.Register(bad, &err));
    bad.name = "demo.drive";
    bad.access = Arcane::Arc::Access::Drive;
    bad.mutates = Arcane::Arc::Mutates::None;
    CHECK_FALSE(reg.Register(bad, &err));
}

TEST_CASE("schema rewrites vec3 and stamps ReflectionJsonWriter defaults", "[arc][registry]") {
    auto& reg = Arcane::Arc::CommandRegistry::Get();
    std::string err;
    Arcane::Arc::Desc ok = Base();
    REQUIRE(reg.Register(ok, &err));
    const auto schema = reg.SchemaOf("demo.ok");
    const auto& pos = schema["properties"]["position"];
    CHECK(pos["type"] == "array");
    CHECK(pos["minItems"] == 3);
    CHECK(pos["maxItems"] == 3);
    CHECK(pos["default"].is_array());
    CHECK(pos["default"].size() == 3);
    CHECK(pos["default"][0] == 0);
    CHECK(pos["default"][1] == 0);
    CHECK(pos["default"][2] == 0);
    CHECK(schema["properties"]["n"]["default"] == 3);
    CHECK_FALSE(reg.Register(ok, &err));
    reg.UnregisterModule("");
    CHECK(reg.Find("demo.ok") != nullptr);
    ok.module = "fixture";
    ok.name = "demo.mod";
    REQUIRE(reg.Register(ok, &err));
    const auto gen = reg.Generation();
    reg.UnregisterModule("fixture");
    CHECK(reg.Find("demo.mod") == nullptr);
    CHECK(reg.Generation() == gen + 1);
    CHECK(reg.Find("demo.ok") != nullptr);
    Arcane::Arc::Call call;
    call.command = "no.such";
    const auto missing = reg.Invoke(call);
    CHECK(missing.envelope.errors[0].code == "command.unknown");
    CHECK(Arcane::Arc::ExitFromHost(missing.envelope) == 2);
}

TEST_CASE("an editor command on a runtime host is command.host-kind", "[arc][registry]") {
    auto& reg = Arcane::Arc::CommandRegistry::Get();
    // A name no task ever registers (ruling F7): Task 25 registers the real
    // editor.selection.get in this same process singleton.
    Arcane::Arc::Call call;
    call.command = "editor.nosuch.get";
    {
        ArcTest::ScopedHostKind runtime(Arcane::Arc::HostKind::Runtime);
        const auto offEditor = reg.Invoke(call);
        CHECK(offEditor.envelope.errors[0].code == "command.host-kind");
        CHECK(Arcane::Arc::ExitFromHost(offEditor.envelope) == 2);
        Arcane::Arc::Call other;
        other.command = "no.such";
        CHECK(reg.Invoke(other).envelope.errors[0].code == "command.unknown");
    }
    {
        ArcTest::ScopedHostKind editor(Arcane::Arc::HostKind::Editor);
        CHECK(reg.Invoke(call).envelope.errors[0].code == "command.unknown");
    }
}

TEST_CASE("catalogue is sorted and command.unknown lists same-group candidates", "[arc][registry]") {
    auto& reg = Arcane::Arc::CommandRegistry::Get();
    std::string err;
    const auto before = reg.Generation();
    Arcane::Arc::Desc d = Base();
    d.module = "cattest";
    d.name = "cattest.zeta";
    Arcane::Arc::Desc noArgs = d;
    noArgs.argsMeta = nullptr;
    CHECK_FALSE(reg.Register(noArgs, &err));
    Arcane::Arc::Desc noFn = d;
    noFn.fn = nullptr;
    CHECK_FALSE(reg.Register(noFn, &err));
    CHECK(reg.Generation() == before);
    REQUIRE(reg.Register(d, &err));
    d.name = "cattest.alpha";
    d.access = Arcane::Arc::Access::Drive;
    d.mutates = Arcane::Arc::Mutates::Scene;
    REQUIRE(reg.Register(d, &err));
    CHECK(reg.Generation() == before + 2);

    const auto catalogue = reg.Catalogue();
    CHECK(catalogue["generation"] == reg.Generation());
    std::vector<std::string> names;
    for (const auto& row : catalogue["commands"])
        names.push_back(row["name"].get<std::string>());
    CHECK(std::is_sorted(names.begin(), names.end()));
    const auto alpha = std::find(names.begin(), names.end(), "cattest.alpha");
    REQUIRE(alpha != names.end());
    const auto& row = catalogue["commands"][static_cast<std::size_t>(alpha - names.begin())];
    CHECK(row["access"] == "drive");
    CHECK(row["mutates"] == "scene");
    CHECK(row["module"] == "cattest");

    Arcane::Arc::Call call;
    call.command = "cattest.nosuch";
    const auto missing = reg.Invoke(call);
    REQUIRE(missing.envelope.errors.size() == 1);
    CHECK(missing.envelope.errors[0].code == "command.unknown");
    CHECK(missing.envelope.errors[0].remediation == "arc commands");
    CHECK(missing.envelope.errors[0].candidates == std::vector<std::string>{"cattest.alpha", "cattest.zeta"});

    reg.UnregisterModule("cattest");
    CHECK(reg.Find("cattest.alpha") == nullptr);
    CHECK(reg.Find("cattest.zeta") == nullptr);
}

struct ArcSchemaInner {
    int n = 4;
};
ARC_REFLECT_TYPE(ArcSchemaInner)
    ARC_REFLECT_FIELD(ArcSchemaInner, n)
ARC_REFLECT_TYPE_END()

struct ArcSchemaOuter {
    int plain = 7;
    ArcSchemaInner child{};
};
ARC_REFLECT_TYPE(ArcSchemaOuter)
    ARC_REFLECT_FIELD(ArcSchemaOuter, plain)
    ARC_REFLECT_FIELD(ArcSchemaOuter, child)
ARC_REFLECT_TYPE_END()

struct ArcSchemaNoDefault {
    int n;
    ArcSchemaNoDefault() = delete;
    explicit ArcSchemaNoDefault(int v) : n(v) {}
};
ARC_REFLECT_TYPE(ArcSchemaNoDefault)
    ARC_REFLECT_FIELD(ArcSchemaNoDefault, n)
ARC_REFLECT_TYPE_END()

TEST_CASE("schema defaults come from ReflectionJsonWriter and keep a nested initializer", "[arc][registry]") {
    const Astra::TypeMeta* outer = Astra::GetMeta<ArcSchemaOuter>();
    REQUIRE(outer != nullptr);
    const auto schema = Arcane::Arc::SchemaFor(*outer);
    CHECK(schema["properties"]["plain"]["default"] == 7);
    CHECK(schema["properties"]["child"]["default"]["n"] == 4);
    const Astra::TypeMeta* bare = Astra::GetMeta<ArcSchemaNoDefault>();
    REQUIRE(bare != nullptr);
    CHECK(bare->isDefaultConstructible == false);
    const auto noDefault = Arcane::Arc::SchemaFor(*bare);
    CHECK_FALSE(noDefault.dump().find("\"default\"") != std::string::npos);
}

TEST_CASE("fallback adapter drops Optional from required and emits one $defs object", "[arc][registry]") {
    auto& reg = Arcane::Arc::CommandRegistry::Get();
    std::string err;
    Arcane::Arc::Desc ok;
    ok.name = "demo.opt";
    ok.summary = "optional field";
    ok.module = "fallback";
    ok.argsMeta = Astra::GetMeta<ArcDemoArgs>();
    ok.fn = &DemoFn;
    REQUIRE(reg.Register(ok, &err));
    const auto flat = reg.SchemaOf("demo.opt");
    REQUIRE(flat.contains("required")); // `position` is required, so the list exists (ruling S3-2)
    CHECK(flat["required"].dump().find("\"n\"") == std::string::npos);
    CHECK(flat["required"].dump().find("\"position\"") != std::string::npos);
    reg.UnregisterModule("fallback");
    CHECK(reg.Find("demo.opt") == nullptr);

    const auto schema = Arcane::Arc::SchemaFor(*Astra::GetMeta<ArcSchemaOuter>());
    CHECK(schema["properties"]["child"]["$ref"] == "#/$defs/ArcSchemaInner");
    CHECK(schema["$defs"]["ArcSchemaInner"]["properties"]["n"]["default"] == 4);
    CHECK_FALSE(schema["$defs"]["ArcSchemaInner"].contains("$defs"));
    const auto& innerReq = schema["$defs"]["ArcSchemaInner"];
    REQUIRE(innerReq.contains("required")); // ruling S3-2: no `if`
    CHECK(innerReq["required"].dump().find("\"n\"") != std::string::npos);
}

#endif
