// VerifyReport: probe parsing, evaluation against a captured frame, and JSON
// emission -- the pure-CPU half of the offscreen-verify path (no GPU, no
// device, no window; [verify] never touches [gpu]). This is the boundary
// between the engine tier and the separate Servitor package, which parses
// the JSON this produces without linking the engine.
#include <Arcane/Host/VerifyReport.hpp>

#include <Json.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// The three TEST_CASEs immediately below are the Task-7 brief's original
// pinned cases, UPDATED for the fix-round rename: the brief's "luma" probe
// tested the unweighted (R+G+B)/765 sum, which is now named "brightness"
// (see VerifyReport.hpp's ProbeKind comment for why -- it is not perceptual).
// "luma@640,360" is kept as a second assertion in the first case below so the
// new kind's grammar gets the same pinned coverage the old one had.
TEST_CASE("verify: probe specs parse, and malformed ones are refused", "[verify]")
{
    std::string err;
    const auto brightness = Arcane::ParseProbe("brightness@640,360", err);
    REQUIRE(brightness.has_value());
    CHECK(brightness->kind == Arcane::ProbeKind::Brightness);
    CHECK(brightness->x == 640);
    CHECK(brightness->y == 360);

    // Luma is a second, distinct positional kind (Rec.709, added alongside
    // the rename) -- same grammar, same refusal rules, exercised here too.
    const auto luma = Arcane::ParseProbe("luma@640,360", err);
    REQUIRE(luma.has_value());
    CHECK(luma->kind == Arcane::ProbeKind::Luma);
    CHECK(luma->x == 640);
    CHECK(luma->y == 360);

    const auto census = Arcane::ParseProbe("census", err);
    REQUIRE(census.has_value());
    CHECK(census->kind == Arcane::ProbeKind::Census);

    CHECK_FALSE(Arcane::ParseProbe("census@1,2", err).has_value());        // args to an argless kind
    CHECK_FALSE(Arcane::ParseProbe("brightness", err).has_value());        // kind missing its args
    CHECK_FALSE(Arcane::ParseProbe("brightness@-1,2", err).has_value());   // negative is a typo
    CHECK_FALSE(Arcane::ParseProbe("bogus@1,2", err).has_value());         // unknown kind
    CHECK_FALSE(err.empty());
}

TEST_CASE("verify: a brightness probe reads the capture and lands in the JSON", "[verify]")
{
    // 2x1 capture: black pixel, white pixel.
    const std::vector<unsigned char> rgba = { 0,0,0,255,  255,255,255,255 };

    Arcane::VerifyReport rep;
    rep.SetRun("D3D12", /*offscreen=*/true, /*framesRendered=*/5, "frames-complete");
    rep.SetCapture(2, 1, rgba);

    std::string err;
    rep.Evaluate({ *Arcane::ParseProbe("brightness@0,0", err), *Arcane::ParseProbe("brightness@1,0", err) });

    const auto doc = nlohmann::json::parse(rep.ToJson());
    // The report is the boundary between the engine tier and the Servitor
    // package, which parses this file without linking the engine -- so the
    // version is part of the contract, not decoration.
    CHECK(doc["schemaVersion"] == 1);
    CHECK(doc["backend"] == "D3D12");
    CHECK(doc["mode"] == "offscreen");
    CHECK(doc["framesRendered"] == 5);
    CHECK(doc["exitReason"] == "frames-complete");
    REQUIRE(doc["probes"].size() == 2);
    CHECK(doc["probes"][0]["kind"] == "brightness");
    CHECK(doc["probes"][0]["value"].get<double>() == Catch::Approx(0.0).margin(0.01));
    CHECK(doc["probes"][1]["value"].get<double>() == Catch::Approx(1.0).margin(0.01));
}

TEST_CASE("verify: an out-of-bounds probe reports a fact, it does not crash", "[verify]")
{
    Arcane::VerifyReport rep;
    rep.SetRun("D3D12", true, 1, "frames-complete");
    rep.SetCapture(2, 1, { 0,0,0,255, 255,255,255,255 });
    std::string err;
    rep.Evaluate({ *Arcane::ParseProbe("brightness@99,99", err) });

    const auto doc = nlohmann::json::parse(rep.ToJson());
    REQUIRE(doc["probes"].size() == 1);
    CHECK(doc["probes"][0].contains("error"));
    CHECK_FALSE(doc["probes"][0].contains("value"));
}

TEST_CASE("verify: brightness and luma diverge on a pure-green pixel -- the whole point of the rename", "[verify]")
{
    // Pure green: brightness (unweighted) reads the same ~0.33 it would for
    // pure red or pure blue -- exactly the blind spot that made "luma" the
    // wrong name for it. Luma (Rec.709, G weighted 0.7152) reads far higher.
    // If a future change ever made these two agree again, the rename would
    // have bought nothing -- this is the test that catches that.
    const std::vector<unsigned char> rgba = { 0, 255, 0, 255 };   // 1x1, pure green
    Arcane::VerifyReport rep;
    rep.SetRun("D3D12", true, 1, "frames-complete");
    rep.SetCapture(1, 1, rgba);

    std::string err;
    rep.Evaluate({ *Arcane::ParseProbe("brightness@0,0", err), *Arcane::ParseProbe("luma@0,0", err) });

    const auto doc = nlohmann::json::parse(rep.ToJson());
    const double brightnessValue = doc["probes"][0]["value"].get<double>();
    const double lumaValue       = doc["probes"][1]["value"].get<double>();

    CHECK(brightnessValue == Catch::Approx(255.0 / 765.0).margin(0.01));   // ~0.333
    CHECK(lumaValue       == Catch::Approx(0.7152).margin(0.01));          // Rec.709 G weight
    CHECK(lumaValue - brightnessValue > 0.3);   // clearly, not marginally, different
}

// ---------------------------------------------------------------------------
// Coverage beyond the pinned Task-7 cases above: the remaining probe kinds,
// the windowed/offscreen mode string, WriteTo, and -- the one that matters
// most -- a sweep that would actually FAIL if the value/entity-xor-error
// invariant ever broke, rather than merely happening to hold for whichever
// cases someone thought to write.
// ---------------------------------------------------------------------------

TEST_CASE("verify: rgba probe reads all four channels", "[verify]")
{
    const std::vector<unsigned char> rgba = { 10, 20, 30, 40 };   // 1x1 capture
    Arcane::VerifyReport rep;
    rep.SetRun("Vulkan", true, 1, "frames-complete");
    rep.SetCapture(1, 1, rgba);

    std::string err;
    rep.Evaluate({ *Arcane::ParseProbe("rgba@0,0", err) });

    const auto doc = nlohmann::json::parse(rep.ToJson());
    const auto& v = doc["probes"][0]["value"];
    CHECK(v["r"] == 10);
    CHECK(v["g"] == 20);
    CHECK(v["b"] == 30);
    CHECK(v["a"] == 40);
}

TEST_CASE("verify: mode reports windowed when the run was not offscreen", "[verify]")
{
    Arcane::VerifyReport rep;
    rep.SetRun("D3D12", /*offscreen=*/false, 42, "window-closed");
    const auto doc = nlohmann::json::parse(rep.ToJson());
    CHECK(doc["mode"] == "windowed");
    CHECK(doc["framesRendered"] == 42);
    CHECK(doc["exitReason"] == "window-closed");
}

TEST_CASE("verify: a census probe reads AddCensus's data when set, and refuses when not", "[verify]")
{
    std::string err;

    // Not set: an honest refusal, not zeros that would read as a measured
    // empty scene.
    {
        Arcane::VerifyReport rep;
        rep.SetRun("D3D12", true, 1, "frames-complete");
        rep.Evaluate({ *Arcane::ParseProbe("census", err) });
        const auto doc = nlohmann::json::parse(rep.ToJson());
        CHECK(doc["probes"][0].contains("error"));
        CHECK_FALSE(doc["probes"][0].contains("value"));
        CHECK_FALSE(doc.contains("census"));   // no top-level census either
    }

    // Set: the census probe's value matches the top-level census object,
    // both sourced from the same AddCensus call.
    {
        Arcane::VerifyReport rep;
        rep.SetRun("D3D12", true, 1, "frames-complete");
        rep.AddCensus(/*spriteReferenced=*/4, /*spriteBound=*/1, /*postReferenced=*/false,
                      /*postBound=*/false, /*meshReferenced=*/1, /*meshBound=*/1);
        rep.Evaluate({ *Arcane::ParseProbe("census", err) });
        const auto doc = nlohmann::json::parse(rep.ToJson());

        REQUIRE(doc.contains("census"));
        CHECK(doc["census"]["spriteReferenced"] == 4);
        CHECK(doc["census"]["spriteBound"] == 1);
        CHECK(doc["census"]["postReferenced"] == false);
        CHECK(doc["census"]["postBound"] == false);
        CHECK(doc["census"]["meshReferenced"] == 1);
        CHECK(doc["census"]["meshBound"] == 1);

        const auto& probeValue = doc["probes"][0]["value"];
        CHECK_FALSE(doc["probes"][0].contains("error"));
        CHECK(probeValue["spriteReferenced"] == 4);
        CHECK(probeValue["meshBound"] == 1);
    }
}

TEST_CASE("verify: a pick probe is an honest refusal when SetPick was never called -- no fabricated entity id", "[verify]")
{
    // Task 9 gave Pick a real channel (SetPick), unlike Task 7 where none
    // existed at all. But a run that never armed a pick@ readback (or a
    // caller that evaluates before calling SetPick(), same as the header's
    // "call setters before Evaluate" contract for SetCapture/AddCensus) must
    // still refuse honestly rather than reporting a fabricated id -- this is
    // the same invariant Census's "not set" case upholds. Stays an error even
    // with a capture set, so an agent never mistakes "no pick armed" for
    // "measured zero".
    Arcane::VerifyReport rep;
    rep.SetRun("D3D12", true, 1, "frames-complete");
    rep.SetCapture(1, 1, { 1, 2, 3, 4 });

    std::string err;
    const auto probe = Arcane::ParseProbe("pick@0,0", err);
    REQUIRE(probe.has_value());
    rep.Evaluate({ *probe });

    const auto doc = nlohmann::json::parse(rep.ToJson());
    CHECK(doc["probes"][0]["kind"] == "pick");
    CHECK(doc["probes"][0].contains("error"));
    CHECK_FALSE(doc["probes"][0].contains("value"));
    CHECK_FALSE(doc["probes"][0].contains("entity"));
}

TEST_CASE("verify: FirstPickProbe finds the first pick@ spec among raw probes, skipping non-pick and malformed ones", "[verify]")
{
    CHECK_FALSE(Arcane::FirstPickProbe({}).has_value());
    CHECK_FALSE(Arcane::FirstPickProbe({ "brightness@0,0", "census" }).has_value());

    const auto found = Arcane::FirstPickProbe({ "brightness@0,0", "pick@640,360", "pick@1,1" });
    REQUIRE(found.has_value());
    CHECK(found->kind == Arcane::ProbeKind::Pick);
    CHECK(found->x == 640);
    CHECK(found->y == 360);

    // A malformed entry ahead of a real pick@ spec is skipped, not fatal --
    // the real parse-and-log pass (RuntimeApp.cpp's ShutdownGraphPath)
    // reports that malformed one separately; this helper only answers "is
    // there a pick request" and must not double that log.
    const auto skipsMalformed = Arcane::FirstPickProbe({ "bogus@1,2", "pick@10,20" });
    REQUIRE(skipsMalformed.has_value());
    CHECK(skipsMalformed->x == 10);
    CHECK(skipsMalformed->y == 20);
}

TEST_CASE("verify: SetPick(landed=false) reports NO READBACK LANDED, matching --pick-probe's own wording", "[verify]")
{
    Arcane::VerifyReport rep;
    rep.SetRun("D3D12", true, 60, "frames-complete");
    rep.SetPick(/*armedX=*/640, /*armedY=*/360, /*landed=*/false,
                /*hitProxyId=*/0, /*resolved=*/false, "", "");

    std::string err;
    rep.Evaluate({ *Arcane::ParseProbe("pick@640,360", err) });

    const auto doc = nlohmann::json::parse(rep.ToJson());
    const auto& entry = doc["probes"][0];
    REQUIRE(entry.contains("error"));
    CHECK_FALSE(entry.contains("entity"));
    CHECK(entry["error"].get<std::string>().find("NO READBACK LANDED") != std::string::npos);
}

TEST_CASE("verify: a background pick miss is a FACT (id 0 as the nil Guid), never an error", "[verify]")
{
    Arcane::VerifyReport rep;
    rep.SetRun("D3D12", true, 60, "frames-complete");
    rep.SetPick(20, 700, /*landed=*/true, /*hitProxyId=*/0, /*resolved=*/false, "", "");

    std::string err;
    rep.Evaluate({ *Arcane::ParseProbe("pick@20,700", err) });

    const auto doc = nlohmann::json::parse(rep.ToJson());
    const auto& entry = doc["probes"][0];
    CHECK_FALSE(entry.contains("error"));
    REQUIRE(entry.contains("entity"));
    CHECK(entry["entity"].is_null());
    CHECK(entry["id"] == "00000000-0000-0000-0000-000000000000");
    CHECK(entry["hitProxyId"] == 0);
}

TEST_CASE("verify: a resolved pick reports the durable name+Guid, and the raw hit-proxy separately", "[verify]")
{
    Arcane::VerifyReport rep;
    rep.SetRun("D3D12", true, 60, "frames-complete");
    rep.SetPick(640, 360, /*landed=*/true, /*hitProxyId=*/3, /*resolved=*/true,
                "TealQuad", "3f2504e0-4f89-11d3-9a0c-0305e82c3301");

    std::string err;
    rep.Evaluate({ *Arcane::ParseProbe("pick@640,360", err) });

    const auto doc = nlohmann::json::parse(rep.ToJson());
    const auto& entry = doc["probes"][0];
    CHECK_FALSE(entry.contains("error"));
    CHECK(entry["entity"] == "TealQuad");
    CHECK(entry["id"] == "3f2504e0-4f89-11d3-9a0c-0305e82c3301");
    CHECK(entry["hitProxyId"] == 3);
}

TEST_CASE("verify: a pick hit that could not resolve an Identity is an honest error, never a churny raw id", "[verify]")
{
    // hitProxyId is frame-scoped and meaningless across runs -- reporting it
    // under `id` (the field a durable-identity contract promises) would be a
    // stable-looking value that silently churns between runs. An error says
    // so instead.
    Arcane::VerifyReport rep;
    rep.SetRun("D3D12", true, 60, "frames-complete");
    rep.SetPick(640, 360, /*landed=*/true, /*hitProxyId=*/3, /*resolved=*/false, "", "");

    std::string err;
    rep.Evaluate({ *Arcane::ParseProbe("pick@640,360", err) });

    const auto doc = nlohmann::json::parse(rep.ToJson());
    const auto& entry = doc["probes"][0];
    REQUIRE(entry.contains("error"));
    CHECK_FALSE(entry.contains("entity"));
    CHECK_FALSE(entry.contains("value"));
    CHECK(entry["error"].get<std::string>().find("Identity") != std::string::npos);
}

TEST_CASE("verify: a pick@ probe at a pixel other than the one armed refuses rather than answering the wrong question", "[verify]")
{
    // Only the FIRST pick@x,y spec in a run is ever armed (FirstPickProbe) --
    // a second, differently-positioned pick@ probe in the same run's --probe
    // list must not silently be answered with the FIRST one's hit-proxy.
    Arcane::VerifyReport rep;
    rep.SetRun("D3D12", true, 60, "frames-complete");
    rep.SetPick(640, 360, /*landed=*/true, /*hitProxyId=*/3, /*resolved=*/true, "TealQuad", "guid");

    std::string err;
    rep.Evaluate({ *Arcane::ParseProbe("pick@10,10", err) });

    const auto doc = nlohmann::json::parse(rep.ToJson());
    const auto& entry = doc["probes"][0];
    REQUIRE(entry.contains("error"));
    CHECK_FALSE(entry.contains("entity"));
    CHECK(entry["error"].get<std::string>().find("never probed") != std::string::npos);
}

TEST_CASE("verify: every pick branch carries exactly one of entity or error, never both, never neither", "[verify]")
{
    // The XOR invariant, exercised across every branch Evaluate's Pick case
    // can take -- not just the ones the cases above happen to cover. If a
    // future edit ever sets both fields (or neither) on any of these, this
    // fails.
    std::string err;
    const auto spec           = *Arcane::ParseProbe("pick@640,360", err);
    const auto mismatchedSpec = *Arcane::ParseProbe("pick@1,1", err);

    auto checkXor = [](const nlohmann::json& entry)
    {
        const bool hasResult = entry.contains("value") || entry.contains("entity");
        const bool hasError  = entry.contains("error");
        CHECK(hasResult != hasError);
    };

    {   // SetPick never called.
        Arcane::VerifyReport rep;
        rep.SetRun("D3D12", true, 1, "frames-complete");
        rep.Evaluate({ spec });
        checkXor(nlohmann::json::parse(rep.ToJson())["probes"][0]);
    }
    {   // Mismatched pixel.
        Arcane::VerifyReport rep;
        rep.SetRun("D3D12", true, 1, "frames-complete");
        rep.SetPick(640, 360, true, 3, true, "X", "guid");
        rep.Evaluate({ mismatchedSpec });
        checkXor(nlohmann::json::parse(rep.ToJson())["probes"][0]);
    }
    {   // Not landed.
        Arcane::VerifyReport rep;
        rep.SetRun("D3D12", true, 1, "frames-complete");
        rep.SetPick(640, 360, false, 0, false, "", "");
        rep.Evaluate({ spec });
        checkXor(nlohmann::json::parse(rep.ToJson())["probes"][0]);
    }
    {   // Background miss.
        Arcane::VerifyReport rep;
        rep.SetRun("D3D12", true, 1, "frames-complete");
        rep.SetPick(640, 360, true, 0, false, "", "");
        rep.Evaluate({ spec });
        checkXor(nlohmann::json::parse(rep.ToJson())["probes"][0]);
    }
    {   // Hit, unresolved.
        Arcane::VerifyReport rep;
        rep.SetRun("D3D12", true, 1, "frames-complete");
        rep.SetPick(640, 360, true, 3, false, "", "");
        rep.Evaluate({ spec });
        checkXor(nlohmann::json::parse(rep.ToJson())["probes"][0]);
    }
    {   // Hit, resolved.
        Arcane::VerifyReport rep;
        rep.SetRun("D3D12", true, 1, "frames-complete");
        rep.SetPick(640, 360, true, 3, true, "X", "guid");
        rep.Evaluate({ spec });
        checkXor(nlohmann::json::parse(rep.ToJson())["probes"][0]);
    }
}

TEST_CASE("verify: every probe entry carries exactly one of value/entity or error, never both, never neither", "[verify]")
{
    // The invariant the whole report exists to uphold, exercised across every
    // kind and every set/unset combination of the data those kinds read --
    // brightness/luma/rgba with and without a capture, in and out of bounds;
    // census with and without AddCensus; pick, always refused here (SetPick
    // is never called in this sweep -- see the dedicated pick branch sweep
    // below for SetPick's own states). If a future edit to Evaluate() ever
    // sets both fields, or neither, on ANY branch --
    // not just the ones the pinned Task-7 cases happen to cover -- this fails.
    std::string err;
    const std::vector<Arcane::ProbeSpec> specs = {
        *Arcane::ParseProbe("brightness@0,0", err),
        *Arcane::ParseProbe("brightness@50,50", err),   // will be out of bounds
        *Arcane::ParseProbe("luma@0,0", err),
        *Arcane::ParseProbe("luma@50,50", err),          // out of bounds
        *Arcane::ParseProbe("rgba@0,0", err),
        *Arcane::ParseProbe("rgba@50,50", err),          // out of bounds
        *Arcane::ParseProbe("pick@0,0", err),            // always deferred
        *Arcane::ParseProbe("census", err),
    };

    for (const bool withCapture : { false, true })
    {
        for (const bool withCensus : { false, true })
        {
            Arcane::VerifyReport rep;
            rep.SetRun("D3D12", true, 1, "frames-complete");
            if (withCapture)
                rep.SetCapture(2, 2, { 0,0,0,255, 10,10,10,255, 20,20,20,255, 30,30,30,255 });
            if (withCensus)
                rep.AddCensus(1, 1, false, false, 1, 1);

            rep.Evaluate(specs);
            const auto doc = nlohmann::json::parse(rep.ToJson());
            REQUIRE(doc["probes"].size() == specs.size());
            for (const auto& entry : doc["probes"])
            {
                const bool hasResult = entry.contains("value") || entry.contains("entity");
                const bool hasError  = entry.contains("error");
                INFO("probe raw=" << entry["raw"].get<std::string>()
                     << " withCapture=" << withCapture << " withCensus=" << withCensus);
                CHECK(hasResult != hasError);
            }
        }
    }
}

TEST_CASE("verify: WriteTo round-trips through disk", "[verify]")
{
    Arcane::VerifyReport rep;
    rep.SetRun("D3D12", true, 3, "frames-complete");
    rep.SetCapture(1, 1, { 100, 150, 200, 255 });
    std::string err;
    rep.Evaluate({ *Arcane::ParseProbe("luma@0,0", err) });

    const auto path = (std::filesystem::temp_directory_path() / "arcane_verify_report_test.json").string();
    REQUIRE(rep.WriteTo(path));

    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    std::ostringstream contents;
    contents << in.rdbuf();
    in.close();

    const auto doc = nlohmann::json::parse(contents.str());
    CHECK(doc["schemaVersion"] == 1);
    CHECK(doc["framesRendered"] == 3);

    std::remove(path.c_str());
}

TEST_CASE("verify: WriteTo fails, does not throw, when the path cannot be opened", "[verify]")
{
    Arcane::VerifyReport rep;
    rep.SetRun("D3D12", true, 1, "frames-complete");
    // A directory that does not exist -- std::ofstream cannot create the
    // intermediate path component, so the open fails.
    const auto path = (std::filesystem::temp_directory_path() /
                        "arcane_verify_report_test_missing_dir" / "report.json").string();
    CHECK_FALSE(rep.WriteTo(path));
}
